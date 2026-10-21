// Plugin.cpp - MagickaPoolFix
//
// Entry point, discovery and the observation/correction loop.
//
// What this plugin does to the game, and nothing more:
//
//   * It writes ONE float: the player's magicka deficit at PlayerCharacter+0x6B8,
//     only when the maximum changes, and reads it back to confirm the write took
//     (WRITE OK / WRITE FAILED in the log). bDryRun = 1 turns every write off.
//
//   * No hook is installed and no game code is patched. No vtable is touched.
//
//   * The player object is found through RTTI (the PlayerCharacter vtable) and a
//     static slot in the game module's own data sections; a chunked heap sweep
//     remains as an opt-in fallback (bHeapScan).
//
//   * All work happens on our own thread, so it cannot slow the game's frame
//     loop. Every read of game objects and every write go through
//     ReadProcessMemory/WriteProcessMemory on our own process (PeUtil.h), so a
//     page released under us makes the call fail instead of faulting. The RTTI
//     pass is the one direct reader, bounds-checked against the game module's own
//     sections, which stay mapped for the life of the process. Torn reads
//     are guarded by re-verifying the object's vtable pointer before every sample
//     and discarding non-finite values.
//
//   * Calling the game's virtual GetActorFloatValue (bCallVirtuals) is off and
//     must stay off: on the tested build it hangs or faults (docs/SPEC.md section 9.1).
//
// See docs/SPEC.md for the full model and the sources behind every number.

#include "PCH.h"

#include "Config.h"
#include "Corrector.h"
#include "HypothesisCalibrator.h"
#include "Log.h"
#include "MagickaMath.h"
#include "ModelWatch.h"
#include "PlayerStats.h"
#include "Probe.h"
#include "VirtualCallGuard.h"
#include "Rtti.h"

#include <atomic>
#include <cctype>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

HMODULE g_self = nullptr;
std::atomic<bool> g_stop{false};
HANDLE  g_thread = nullptr;
HANDLE  g_singleton = nullptr;

mpf::Config      g_cfg;
mpf::ModuleImage g_game;
mpf::MemRegions  g_mem;
mpf::Calibrator  g_cal;
mpf::FloatWatch  g_watch;

std::uintptr_t g_vtable   = 0;
std::uintptr_t g_npcVtable = 0;  // TESNPC, to verify the player's base form
std::uintptr_t g_player   = 0;   // PlayerCharacter instance
std::uintptr_t g_playerSlot = 0; // static slot in the game module that held it, once found
int            g_offMod   = 0;   // resolved magickaModifier offset
int            g_offMagicAV  = 0;  // resolved magicActorValueModifiers offset
int            g_offScriptAV = 0;  // resolved scriptActorValueModifiers offset
int            g_offGameAV   = 0;  // resolved gameActorValueModifiers offset
mpf::ModelWatch g_model;
mpf::PolicyTracker g_policy;
mpf::CompensationConfig g_comp;
bool g_warnedPinned = false;

// Sum of the three modifier sources the engine keeps for one ActorValue. A
// fortify effect from a spell lands in the magic array, a console or quest script
// writes the script array, and the engine's own adjustments use the game array.
// Maximum Magicka responds to the total, so a watcher that reads only one of the
// three would misreport the moment two sources move in opposite directions.
// The three sources one by one, magic / script / game. A disabled array reads as 0.
static bool ReadAvSources(std::uintptr_t obj, int avIndex, float out[3]) {
    const int bases[3] = { g_offMagicAV, g_offScriptAV, g_offGameAV };
    for (int i = 0; i < 3; ++i) {
        out[i] = 0.0f;
        if (bases[i] <= 0) { continue; }
        const unsigned off = static_cast<unsigned>(bases[i] + avIndex * 4);
        if (!mpf::SafeRead(obj + off, out[i])) { return false; }
        if (!std::isfinite(out[i])) { return false; }
    }
    return true;
}

static bool TotalAvModifier(std::uintptr_t obj, int avIndex, float& out) {
    float src[3];
    if (!ReadAvSources(obj, avIndex, src)) { return false; }
    out = src[0] + src[1] + src[2];
    return true;
}

// Which source moved, for the two actor values the model watches. A spell or an
// enchantment lands in the magic array, a quest or console script in the script
// array, the engine's own adjustments in the game array. Telling them apart is the
// first step in reading an unexpected change, a drain for instance (SPEC §10.5).
struct AvSources { float intel[3]; float magicka[3]; };

void LogSourceBreakdown(const AvSources& before, const AvSources& after) {
    std::string line;
    auto add = [&line](const char* name, const float b[3], const float a[3]) {
        if (!line.empty()) { line += ", "; }
        line += name;
        line += " ";
        for (int i = 0; i < 3; ++i) {
            if (i) { line += " / "; }
            line += mpf::FormatModifier(a[i] - b[i]);
        }
    };
    add("Intelligence", before.intel, after.intel);
    add("Magicka", before.magicka, after.magicka);
    MPF_INFO("  change by source (magic / script / game): %s", line.c_str());
}
int            g_vtblGet  = 0;   // resolved GetActorFloatValue index

// Paths in the log go through Anon(): the user's profile folder becomes
// %USERPROFILE%, so a log pasted into a public bug report does not carry the
// Windows account name. Filled at startup.
std::string g_profile;
std::string g_gameDir;   // folder of the game executable, no trailing slash

std::string Anon(const std::string& path) { return mpf::AnonymisePath(path, g_profile); }

void LoadProfileFolder() {
    char buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableA("USERPROFILE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) { g_profile.assign(buf, n); }
}

// PE timestamp of a module mapped in this process, read through ReadMem so a bad
// base fails cleanly instead of faulting.
bool ReadPeTimestamp(std::uintptr_t base, std::uint32_t& out) {
    std::uint16_t mz = 0;
    std::int32_t  lfanew = 0;
    std::uint32_t sig = 0;
    if (!mpf::ReadMem(base, &mz, sizeof(mz)) || mz != 0x5A4D) { return false; }
    if (!mpf::ReadMem(base + 0x3C, &lfanew, sizeof(lfanew)) || lfanew <= 0 ||
        lfanew > 0x1000) {
        return false;
    }
    if (!mpf::ReadMem(base + static_cast<unsigned>(lfanew), &sig, sizeof(sig)) ||
        sig != 0x00004550) {
        return false;
    }
    return mpf::ReadMem(base + static_cast<unsigned>(lfanew) + 8, &out, sizeof(out));
}

// Counters reported by the heartbeat.
struct Stats {
    unsigned long long samples       = 0;
    unsigned long long watchChanges  = 0;
    unsigned long long rescans       = 0;
    unsigned long long instanceLost  = 0;
    unsigned long long maxChanges    = 0;
    unsigned long long wouldCorrect  = 0;
    unsigned long long corrected     = 0;   // deficit writes that stuck
    unsigned long long writeFailed    = 0;
    unsigned long long invariantFail = 0;
    unsigned long long dumps         = 0;
} g_stats;

// The plugin's own cost, reported by the heartbeat, so that "the game feels
// slower" can be checked against numbers. Wall time on the plugin's thread from
// the end of one Sleep to the start of the next; the memory map refresh, a walk of
// the whole address space every 2 s, is also timed on its own.
struct Cost {
    long long freq = 0;            // QueryPerformanceFrequency, ticks per second
    long long windowStart = 0;     // start of the current heartbeat window
    long long busy = 0;            // ticks spent working in the window
    long long refresh = 0;         // ticks spent in memory map refreshes
    long long refreshMax = 0;
    unsigned  refreshes = 0;
} g_cost;

long long QpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Times one loop iteration; the destructor runs on every `continue`.
struct BusyScope {
    long long start = QpcNow();
    ~BusyScope() { g_cost.busy += QpcNow() - start; }
};

// " | own time ..." for the heartbeat, then a new window starts.
std::string CostText() {
    const long long now = QpcNow();
    std::string out;
    if (g_cost.freq > 0 && now > g_cost.windowStart) {
        const double ms = 1000.0 / static_cast<double>(g_cost.freq);
        const double windowS = static_cast<double>(now - g_cost.windowStart) /
                               static_cast<double>(g_cost.freq);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      " | own time %.2f ms per second, memory map refreshes %.2f ms avg "
                      "%.2f ms max (x%u)",
                      static_cast<double>(g_cost.busy) * ms / windowS,
                      g_cost.refreshes ? static_cast<double>(g_cost.refresh) * ms /
                                             g_cost.refreshes
                                       : 0.0,
                      static_cast<double>(g_cost.refreshMax) * ms, g_cost.refreshes);
        out = buf;
    }
    const long long freq = g_cost.freq;
    g_cost = Cost{};
    g_cost.freq = freq;
    g_cost.windowStart = now;
    return out;
}

using GetActorFloatValueFn = float (MPF_CALL*)(void* self, std::int32_t av);

// Minimum number of plausible non-zero floats in the observation window for the
// player object to count as populated rather than freshly allocated.
constexpr int kPopulatedFloatFloor = 18;

// A single poll changing at least this many watched offsets is treated as a break in
// continuity rather than as a game event.
constexpr int kDiscontinuityFloats = 20;

// How long to let the game breathe after the player object is populated before
// entering one of its methods, and how long to wait for that call to come back.
constexpr unsigned kVCallSettleMs  = 3000;
constexpr unsigned kVCallTimeoutMs = 2000;

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

// The game module is the running executable. Resolving it that way rather than by
// name works for every build variant (Win64-Shipping, WinGDK-Shipping, and any
// future rename).
bool LoadGameModule() {
    if (!g_game.Load(GetModuleHandleW(nullptr))) { return false; }
    {
        const std::string& p = g_game.Path();
        const auto slash = p.find_last_of("\\/");
        g_gameDir = (slash == std::string::npos) ? std::string{} : p.substr(0, slash);
    }
    // The name and PE timestamp are on the summary line; the rest is for Debug.
    MPF_DBG("---- game module ----");
    MPF_DBG("game module   : %s", g_game.Name().c_str());
    MPF_DBG("  path        : %s", Anon(g_game.Path()).c_str());
    MPF_DBG("  base        : 0x%llX  size 0x%llX",
            static_cast<unsigned long long>(g_game.Base()),
            static_cast<unsigned long long>(g_game.ImageSize()));
    MPF_DBG("  entry point : 0x%llX",
            static_cast<unsigned long long>(g_game.Entry()));
    MPF_DBG("  PE timestamp: 0x%08lX (identifies the game build)",
            static_cast<unsigned long>(g_game.Timestamp()));
    MPF_DBG("  sections    : %zu", g_game.Sections().size());
    for (const auto& s : g_game.Sections()) {
        MPF_DBG("    %-8s 0x%llX  size 0x%-8llX  %s%s", s.name,
                static_cast<unsigned long long>(s.start),
                static_cast<unsigned long long>(s.size),
                s.Executable() ? "X" : "-", s.Writable() ? "W" : "-");
    }
    return true;
}

void LogModuleInventory() {
    const auto rows = mpf::EnumerateModules();
    const std::string winDir = mpf::WindowsDirLower();

    // At Info, one compact list of what is not a stock Windows DLL: the mod
    // loaders and the mods, each with the PE timestamp of its own file, which
    // identifies its build without reading its version resource (that would mean
    // importing VERSION.dll, the very file an ASI loader usually stands in for).
    // Paths inside the game folder are shown relative to it.
    std::vector<std::string> mods;
    int selfCopies = 0;
    for (const auto& r : rows) {
        std::string lower = r.path;
        for (auto& c : lower) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        const bool system = !winDir.empty() && lower.compare(0, winDir.size(), winDir) == 0;
        if (_stricmp(r.name.c_str(), MPF_PLUGIN_NAME ".asi") == 0) { ++selfCopies; }
        // A module with no path was unloaded between the enumeration and the
        // query; it has nothing useful to print.
        if (system || r.base == g_game.Base() || r.path.empty()) { continue; }
        std::uint32_t ts = 0;
        char entry[400];
        const std::string where = mpf::RelativeTo(r.path, g_gameDir);
        if (ReadPeTimestamp(r.base, ts)) {
            std::snprintf(entry, sizeof(entry), "%s PE 0x%08X", Anon(where).c_str(),
                          static_cast<unsigned>(ts));
        } else {
            std::snprintf(entry, sizeof(entry), "%s", Anon(where).c_str());
        }
        mods.emplace_back(entry);
    }
    std::string line;
    int part = 0;
    for (std::size_t i = 0; i < mods.size(); ++i) {
        if (!line.empty() && line.size() + mods[i].size() > 800) {
            MPF_INFO("mods and loaders%s: %s", part++ ? " (cont.)" : "", line.c_str());
            line.clear();
        }
        if (!line.empty()) { line += ", "; }
        line += mods[i];
    }
    MPF_INFO("mods and loaders%s: %s", part ? " (cont.)" : "",
             line.empty() ? "none outside the Windows folder" : line.c_str());

    // The full inventory, for Debug. Two distinct products can present the same
    // module name -- a mod loader installed as a proxy DLL is literally named
    // after the system library it stands in for -- so the folder is printed for
    // anything that is not a stock Windows DLL.
    MPF_DBG("---- loaded modules ----");
    MPF_DBG("%zu modules in the process", rows.size());
    for (const auto& r : rows) {
        std::string lower = r.path;
        for (auto& c : lower) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        const bool system = !winDir.empty() && lower.compare(0, winDir.size(), winDir) == 0;
        if (system) {
            MPF_DBG("  %-40s 0x%llX  size 0x%llX", r.name.c_str(),
                    static_cast<unsigned long long>(r.base),
                    static_cast<unsigned long long>(r.size));
        } else {
            MPF_DBG("  %-40s 0x%llX  size 0x%llX  <- %s", r.name.c_str(),
                    static_cast<unsigned long long>(r.base),
                    static_cast<unsigned long long>(r.size), Anon(r.path).c_str());
        }
    }

    // An ASI loader scans several folders (the game directory, plus scripts,
    // plugins and update). A copy of the plugin sitting in two of them is loaded
    // twice into the same process, which would mean two correction loops writing
    // the same float. The singleton guard already stopped the second instance;
    // say where it was.
    if (selfCopies > 1) {
        MPF_ERR("%d copies of " MPF_PLUGIN_NAME ".asi are loaded in this process. "
                "Only one is active, the others exited immediately. Delete the "
                "duplicates: keep a single copy, in one folder only.", selfCopies);
        for (const auto& r : rows) {
            if (_stricmp(r.name.c_str(), MPF_PLUGIN_NAME ".asi") == 0) {
                MPF_ERR("  copy: %s", Anon(r.path).c_str());
            }
        }
    }
}

bool DiscoverVtable() {
    MPF_DBG("---- RTTI discovery ----");

    // Enough entries for GetActorFloatValue at 0x0B1 plus headroom.
    const std::size_t minEntries = static_cast<std::size_t>(g_vtblGet) + 8;

    mpf::RttiSearchStats stats;
    const auto loc = mpf::FindVtableByRtti(g_game, ".?AVPlayerCharacter@@",
                                           minEntries, stats);

    MPF_DBG("searched %d data sections", stats.sectionsScanned);
    MPF_DBG("  name  \".?AVPlayerCharacter@@\" : %d hit(s)", stats.nameHits);
    MPF_DBG("  object locator                 : %d candidate(s)", stats.locatorHits);
    MPF_DBG("  vtable references              : %d candidate(s)", stats.vtableHits);

    if (!loc.typeDescriptor) {
        MPF_ERR("RTTI type descriptor not found. Either the game binary was built "
                "without RTTI, or it is packed/obfuscated. Nothing further is "
                "possible without hard-coded addresses; stopping.");
        return false;
    }
    MPF_DBG("  type descriptor : 0x%llX (RVA 0x%X)",
             static_cast<unsigned long long>(loc.typeDescriptor),
             g_game.Rva(loc.typeDescriptor));

    if (!loc.locator) {
        MPF_ERR("No self-consistent RTTICompleteObjectLocator references that "
                "descriptor. Stopping.");
        return false;
    }
    MPF_DBG("  object locator  : 0x%llX",
             static_cast<unsigned long long>(loc.locator));

    if (!loc.Valid()) {
        MPF_ERR("Vtable rejected: only %zu leading entries point into executable "
                "code, fewer than the %zu required for index 0x%X. This build's "
                "vtable indices do not fit your game version.",
                loc.validEntries, minEntries, g_vtblGet);
        return false;
    }

    g_vtable = loc.vtable;
    MPF_DBG("  vtable          : 0x%llX (RVA 0x%X), %zu valid leading entries",
             static_cast<unsigned long long>(g_vtable), g_game.Rva(g_vtable),
             loc.validEntries);

    // Log the two slots this project cares about, so their plausibility can be
    // checked by hand against a disassembler if ever needed.
    std::uintptr_t fnUpdate = 0, fnGet = 0;
    std::memcpy(&fnUpdate,
                reinterpret_cast<const void*>(
                    g_vtable + static_cast<unsigned>(g_cfg.Off(g_cfg.vtblUpdate,
                                                    mpf::kDefaultVtbl_Update)) * 8),
                8);
    std::memcpy(&fnGet,
                reinterpret_cast<const void*>(g_vtable +
                                              static_cast<unsigned>(g_vtblGet) * 8),
                8);
    MPF_DBG("  slot 0x%03X Update(float)        -> 0x%llX (RVA 0x%X)",
             g_cfg.Off(g_cfg.vtblUpdate, mpf::kDefaultVtbl_Update),
             static_cast<unsigned long long>(fnUpdate), g_game.Rva(fnUpdate));
    MPF_DBG("  slot 0x%03X GetActorFloatValue   -> 0x%llX (RVA 0x%X)",
             g_vtblGet, static_cast<unsigned long long>(fnGet), g_game.Rva(fnGet));
    return true;
}

// The player's base form is checked against the TESNPC vtable before the
// character block trusts any of its fields. Not finding it is not fatal: the form
// IDs are still checked, and corrections never depend on this.
void DiscoverNpcVtable() {
    mpf::RttiSearchStats stats;
    const auto loc = mpf::FindVtableByRtti(g_game, ".?AVTESNPC@@", 4, stats);
    if (loc.Valid()) {
        g_npcVtable = loc.vtable;
        MPF_DBG("  TESNPC vtable   : 0x%llX (RVA 0x%X), %zu valid leading entries",
                static_cast<unsigned long long>(g_npcVtable), g_game.Rva(g_npcVtable),
                loc.validEntries);
    } else {
        MPF_INFO("TESNPC vtable not found through RTTI (%d name hit(s)): the character "
                 "data below is checked by form ID only", stats.nameHits);
    }
}

// ---------------------------------------------------------------------------
// Locating the live PlayerCharacter
//
// 0.1.x scanned the whole private read-write heap and dereferenced it directly.
// That crashed on first contact, and the reason is structural rather than a
// typo: a heap page that VirtualQuery reported as committed can be released by
// the game microseconds later, and reading it then raises an access violation
// this build cannot catch. 668 MB of heap traversed from a foreign thread makes
// that race close to certain.
//
// Two changes fix it. Every read now goes through ReadMem (ReadProcessMemory on
// our own handle), which returns an error instead of faulting when a page has
// gone away. And the primary search no longer touches the heap at all: it looks
// for the pointer in the game module's own writable sections, which are
// image-backed and committed for the whole process lifetime.
//
// The second point is also a better search. A singleton like the player is
// reachable from a static slot, so finding that slot yields both the object and
// a stable address we could read directly in a later build, with no scanning at
// all. It is 8 MB of .data instead of 668 MB of heap, and it cannot race.
// ---------------------------------------------------------------------------

struct ScanResult {
    std::uintptr_t instance   = 0;
    std::uintptr_t slot       = 0;   // static address that held the pointer, if any
    std::size_t    scanned    = 0;
    int            candidates = 0;
    int            derefs     = 0;   // pointer-shaped words we paid a syscall to test
    int            failedReads = 0;  // pages that vanished between snapshot and read
    const char*    how        = "not found";
};

// Reads a range in chunks, calling fn(buffer, count, chunkBase) per chunk. A
// chunk that cannot be read is skipped, not fatal: the rest of the range is
// still scanned.
template <typename Fn>
static std::size_t ScanWords(std::uintptr_t base, std::size_t size,
                             std::vector<std::uintptr_t>& buf, int& failed, Fn fn) {
    constexpr std::size_t kChunk = 1u << 20;  // 1 MiB
    std::size_t done = 0;
    while (done + sizeof(std::uintptr_t) <= size) {
        const std::size_t want  = (std::min)(kChunk, size - done);
        const std::size_t words = want / sizeof(std::uintptr_t);
        if (words == 0) { break; }
        buf.resize(words);
        if (!mpf::ReadMem(base + done, buf.data(), words * sizeof(std::uintptr_t))) {
            ++failed;
        } else if (!fn(buf.data(), words, base + done)) {
            return done + want;  // caller is satisfied, stop early
        }
        done += want;
    }
    return done;
}

// Primary search: the game module's writable sections.
static void FindPlayerViaStatics(ScanResult& out) {
    std::vector<std::uintptr_t> buf;
    for (const auto& s : g_game.Sections()) {
        if (!s.Writable() || s.size < sizeof(std::uintptr_t)) { continue; }
        out.scanned += ScanWords(
            s.start, s.size, buf, out.failedReads,
            [&out](const std::uintptr_t* w, std::size_t n, std::uintptr_t chunkBase) {
                for (std::size_t i = 0; i < n; ++i) {
                    const std::uintptr_t cand = w[i];
                    // Cheap snapshot filter first: a static slot holding the player
                    // points into private read-write memory. This rejects the vast
                    // majority of words without a syscall.
                    if (!g_mem.InHeap(cand, sizeof(std::uintptr_t))) { continue; }
                    ++out.derefs;
                    std::uintptr_t vt = 0;
                    if (!mpf::ReadMem(cand, &vt, sizeof(vt))) { continue; }
                    if (vt != g_vtable) { continue; }
                    ++out.candidates;
                    if (!out.instance) {
                        out.instance = cand;
                        out.slot     = chunkBase + i * sizeof(std::uintptr_t);
                        out.how      = "static slot in the game module";
                    }
                }
                return true;  // always finish the section, so candidates is meaningful
            });
    }
}

// Fallback search: the private read-write heap. Off by default because it is
// slow and only useful if the static search comes up empty.
static void FindPlayerViaHeap(ScanResult& out, std::size_t budgetBytes) {
    std::vector<std::uintptr_t> buf;
    for (const auto& r : g_mem.Heap()) {
        if (out.scanned >= budgetBytes) { break; }
        const std::size_t len = (std::min)(r.second - r.first, budgetBytes - out.scanned);
        out.scanned += ScanWords(
            r.first, len, buf, out.failedReads,
            [&out](const std::uintptr_t* w, std::size_t n, std::uintptr_t chunkBase) {
                for (std::size_t i = 0; i < n; ++i) {
                    if (w[i] != g_vtable) { continue; }
                    ++out.candidates;
                    if (!out.instance) {
                        out.instance = chunkBase + i * sizeof(std::uintptr_t);
                        out.how      = "heap scan";
                    }
                }
                return true;
            });
    }
}

bool FindPlayerInstance(std::size_t budgetBytes, ScanResult& out) {
    out = ScanResult{};
    // The snapshot is only used by the search, so it is refreshed here and nowhere
    // else during play. A walk of the whole address space took 106 to 178 ms on the
    // test bench (21 October 2026); done every 2 s, it was nearly all of the
    // plugin's own time.
    const long long t0 = QpcNow();
    g_mem.Refresh();
    const long long dt = QpcNow() - t0;
    g_cost.refresh += dt;
    if (dt > g_cost.refreshMax) { g_cost.refreshMax = dt; }
    ++g_cost.refreshes;
    FindPlayerViaStatics(out);
    if (!out.instance && g_cfg.heapScan) {
        FindPlayerViaHeap(out, budgetBytes);
    }
    if (!out.instance) { return false; }
    g_player = out.instance;
    return true;
}

// The object is still ours only if its vtable pointer still matches.
bool InstanceStillValid() {
    if (!g_player) { return false; }
    std::uintptr_t vt = 0;
    if (!mpf::SafeRead(g_player, vt)) { return false; }
    return vt == g_vtable;
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

// The dump key is polled with GetAsyncKeyState, which observes without consuming.
// A key shared with a game action therefore fires both at once, and two of the
// obvious candidates are actively destructive: F9 is Quick Load and F5 is
// Quicksave, so a dump bound there would reload or overwrite the save under test.
// Source: https://oblivionremastered.wiki.fextralife.com/Controls
const char* GameKeyBinding(int vk) {
    switch (vk) {
    case 0x1B: return "Escape - System menu";
    case 0x70: return "F1 - Character";
    case 0x72: return "F3 - Magic";
    case 0x74: return "F5 - QUICKSAVE (would overwrite the save under test)";
    case 0x75: return "F6 - Quest";
    case 0x76: return "F7 - Help";
    case 0x77: return "F8 - System menu (Quick Save / Quick Load live in it)";
    case 0x78: return "F9 - QUICK LOAD (would reload the save under test)";
    case 0x7B: return "F12 - Steam screenshot, by default";
    default:   return nullptr;
    }
}

// Line 1 of every log: what this binary is, what it will do, and whether the
// running game is the build its offsets were confirmed on. Written before
// anything else can fail, so even a log cut short identifies itself. The
// executable is already mapped when an ASI loader runs us; its header is read
// through ReadMem all the same, so a surprise costs a "?" rather than a crash.
void LogStartupSummary(bool confExisted) {
    char exe[MAX_PATH] = {};
    std::string exeName = "?";
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
        exeName = exe;
        const auto slash = exeName.find_last_of("\\/");
        if (slash != std::string::npos) { exeName = exeName.substr(slash + 1); }
    }
    std::uint32_t ts = 0;
    const bool haveTs =
        ReadPeTimestamp(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), ts);
    const bool validated = haveTs && ts == MPF_VALIDATED_GAME_TIMESTAMP;

    char build[48];
    if (!haveTs) {
        std::snprintf(build, sizeof(build), "PE timestamp unreadable");
    } else {
        std::snprintf(build, sizeof(build), "PE 0x%08X %s", static_cast<unsigned>(ts),
                      validated ? "(validated build)" : "(NOT the validated build)");
    }

    MPF_INFO("%s %s %s | built %s %s | game %s %s | preset %s, engine policy %d, "
             "dry run %d | conf %s",
             MPF_PLUGIN_NAME, MPF_PLUGIN_VERSION,
             g_cfg.dryRun ? "DRY RUN (no memory writes)" : "ACTIVE",
             __DATE__, __TIME__, exeName.c_str(), build,
             mpf::PresetName(g_cfg.preset), g_cfg.enginePolicy, g_cfg.dryRun ? 1 : 0,
             g_cfg.unreadable   ? "present but NOT readable, left untouched, running on defaults"
             : confExisted      ? "found"
             : g_cfg.generated  ? "was missing, created with defaults"
                                : "missing and NOT creatable, running on defaults");

    if (haveTs && !validated && !g_cfg.dryRun) {
        MPF_ERR("This game build (PE 0x%08X) is not the one the offsets were confirmed "
                "on (0x%08X). A game update may have moved them. Set bDryRun = 1 in "
                "the .conf and check this log before letting the plugin write.",
                static_cast<unsigned>(ts), MPF_VALIDATED_GAME_TIMESTAMP);
    }

    MPF_INFO("settings not at their default: %s", mpf::NonDefaultSettings(g_cfg).c_str());

    // Never apply a value silently different from the one written in the file.
    if (g_cfg.heartbeatRaised) {
        MPF_ERR("iHeartbeatSeconds was below the 60 second floor and has been "
                "raised to 60. The value in your .conf is not the one in use.");
    }
    if (g_cfg.dumpOnKeyPress) {
        if (const char* bound = GameKeyBinding(g_cfg.dumpKey)) {
            MPF_ERR("iDumpKey = 0x%02X collides with a game binding: %s. The key is "
                    "only observed, never consumed, so pressing it does BOTH. Pick a "
                    "free key: 0x79 = F10, 0x71 = F2, 0x73 = F4, 0x7A = F11.",
                    g_cfg.dumpKey, bound);
        }
    }
    if (g_cfg.magickaIgnored) {
        MPF_ERR("[Magicka] has been edited but iPreset is not 3, so every value in "
                "that section is IGNORED. Set iPreset = 3 to make them apply.");
    }

    MPF_DBG("---- configuration ----");
    MPF_DBG("iPreset          = %s", mpf::PresetName(g_cfg.preset));
    MPF_DBG("bDryRun          = %d", g_cfg.dryRun ? 1 : 0);
    MPF_DBG("iLevel           = %d", static_cast<int>(g_cfg.logLevel));
    MPF_DBG("iHeartbeatSeconds= %d", g_cfg.heartbeatSeconds);
    MPF_DBG("iMaxLines        = %ld", g_cfg.maxLines);
    MPF_DBG("bEnabled (probe) = %d", g_cfg.probeEnabled ? 1 : 0);
    MPF_DBG("bFloatWatch      = %d", g_cfg.floatWatch ? 1 : 0);
    MPF_DBG("bCallVirtuals    = %d", g_cfg.callVirtuals ? 1 : 0);
    MPF_DBG("bHeapScan        = %d (fallback sweep, normally off)",
            g_cfg.heapScan ? 1 : 0);
    MPF_DBG("iPollIntervalMs  = %d", g_cfg.pollIntervalMs);
    MPF_DBG("iForceHypothesis = %d", g_cfg.forceHypothesis);
    MPF_DBG("iEnginePolicy    = %d, iPolicyConfirmations = %d", g_cfg.enginePolicy,
            g_cfg.policyConfirmations);
}

void ReportOffsets() {
    MPF_DBG("---- offsets in use ----");
    const bool manual = g_cfg.offMagickaModifier >= 0 ||
                        g_cfg.offMagicAVModifiers >= 0 ||
                        g_cfg.offScriptAVModifiers >= 0 ||
                        g_cfg.offGameAVModifiers >= 0 ||
                        g_cfg.vtblUpdate >= 0 || g_cfg.vtblGetActorFloat >= 0;
    MPF_DBG("magickaModifier   = 0x%03X %s", g_offMod,
            g_cfg.offMagickaModifier < 0 ? "(compiled default)" : "(from .conf)");
    MPF_DBG("magicAVModifiers  = 0x%03X",
            g_cfg.Off(g_cfg.offMagicAVModifiers, mpf::kDefaultOff_MagicAVModifiers));
    MPF_DBG("scriptAVModifiers = 0x%03X",
            g_cfg.Off(g_cfg.offScriptAVModifiers, mpf::kDefaultOff_ScriptAVModifiers));
    MPF_DBG("gameAVModifiers   = 0x%03X",
            g_cfg.Off(g_cfg.offGameAVModifiers, mpf::kDefaultOff_GameAVModifiers));
    MPF_DBG("GetActorFloatValue index = 0x%03X", g_vtblGet);
    MPF_DBG("These come from the commonlibob64 reverse-engineered headers and were "
            "confirmed in game on PE timestamp 0x%08X.", MPF_VALIDATED_GAME_TIMESTAMP);

    // Steam ships OblivionRemastered-Win64-Shipping.exe, Game Pass ships
    // OblivionRemastered-WinGDK-Shipping.exe. They are two separate compilations,
    // so offsets confirmed on one are not established on the other. The module
    // name and PE timestamp on the summary line identify the build a set of
    // offsets was validated against.
    if (manual) {
        MPF_ERR("Offsets have been overridden in the .conf. They are specific to ONE "
                "game build: %s, PE timestamp 0x%08lX. If you change store, branch "
                "or game version, set them back to -1 and re-run the probe.",
                g_game.Name().c_str(),
                static_cast<unsigned long>(g_game.Timestamp()));
    }
}

// --------------------------------------------------------------------------
// The write path.
//
// Called once per reported transition of the modifier totals. Everything that decides
// anything lives in Corrector.h and is unit tested; this function is the plumbing
// around it -- calibrate, decide, log, write, verify.
// --------------------------------------------------------------------------
// Defined after the character reader, below.
bool EstimateMaxNow(float& out);

void HandleTransition(const mpf::Transition& t) {
    ++g_stats.maxChanges;

    // Current and maximum, estimated, so a log shows whether the pool went below zero
    // (docs/SPEC.md section 10.5). current = maximum + deficit.
    float maxNow = 0.0f;
    const bool haveMax = EstimateMaxNow(maxNow);
    if (haveMax) {
        const float maxBefore = maxNow - t.deltaMax;
        MPF_INFO("  estimated magicka current/max: before %.1f / %.1f, as the engine left "
                 "it %.1f / %.1f",
                 static_cast<double>(maxBefore + t.deficitBefore),
                 static_cast<double>(maxBefore),
                 static_cast<double>(maxNow + t.deficitAfter), static_cast<double>(maxNow));
    }

    // A floor at zero applied by the engine would make the correction below overshoot
    // by about this amount (Corrector.h, docs/SPEC.md section 10.5). Not observed so
    // far; said loudly if it ever is.
    const float raised = mpf::EngineRaisedDeficitOnDecrease(t.deltaMax, t.deficitBefore,
                                                            t.deficitAfter, g_cfg.epsilon);
    if (raised > 0.0f) {
        MPF_ERR("  ENGINE MOVED THE DEFICIT on a decrease: %+.2f, more than regeneration "
                "explains. Either a restore effect landed in the same instant, or the game "
                "floors current Magicka at zero here (docs/SPEC.md section 10.5). In the "
                "second case the correction below overshoots by about that amount. Please "
                "report this log.", static_cast<double>(raised));
        mpf::Log().Escalate(200, "the engine moved the deficit on a decrease");
    }

    // Calibration first, and from this same transition. A transition that settles the
    // policy is by definition unambiguous, so it is also correctable, and there is no
    // reason to make the player wait for a second one.
    if (g_policy.Observe(t.deficitBefore, t.deltaMax, t.deficitAfter)) {
        MPF_INFO("  OBSERVED: this change cost nothing at the moment it happened, so the "
                 "deficit movement is the game's own behaviour. Evidence now: %d for "
                 "\"ignores\", %d for \"handles\". The game appears to: %s",
                 g_policy.IgnoreEvidence(), g_policy.HandleEvidence(),
                 mpf::PolicyName(g_policy.Observed()));
        if (g_policy.Contradictory()) {
            MPF_ERR("  OBSERVED: the evidence contradicts itself, which means the model "
                    "is wrong somewhere. With iEnginePolicy = 0 no correction will be "
                    "applied until this is resolved.");
        }
        // The only case where this mod can do harm rather than merely be redundant, so
        // it is said once, in full, and in capitals.
        if (g_policy.ContradictsPinned() && !g_warnedPinned) {
            g_warnedPinned = true;
            mpf::Log().Separator("ENGINE BEHAVIOUR DISAGREES WITH THE CONFIGURATION");
            MPF_ERR("iEnginePolicy is set to %d, but this game build does not behave "
                    "that way. Measured on this transition: the game appears to %s.",
                    g_cfg.enginePolicy, mpf::PolicyName(g_policy.Observed()));
            MPF_ERR("This is what a game update that fixes the bug on its own would look "
                    "like. If that is what happened, this mod is now redundant AND "
                    "harmful: it is subtracting a change the game already subtracted, "
                    "which takes Magicka away from you.");
            MPF_ERR("What to do: set iEnginePolicy = 2 in MagickaPoolFix.conf to stop it "
                    "correcting, or remove the plugin. Set iEnginePolicy = 0 to let it "
                    "decide for itself from what it sees.");
            MPF_ERR("Corrections are NOT stopped automatically, because a single odd "
                    "transition is not proof and stopping silently would be worse than "
                    "saying this. The decision is yours.");
            mpf::Log().Separator("end of warning");
        }
    }

    const bool allowed = g_cfg.Active() && !g_cfg.dryRun;
    const mpf::Decision d = mpf::DecideCorrection(t.deltaMax, t.deficitBefore,
                                                  t.deficitAfter, g_policy.Policy(),
                                                  g_comp, allowed);

    if (d.reason != mpf::DecisionReason::kApply) {
        MPF_INFO("  DECISION: %s", mpf::ReasonText(d.reason));
        return;
    }

    ++g_stats.wouldCorrect;
    MPF_INFO("  DECISION: %s at +0x%03X, %.4f -> %.4f (%+.4f), which puts current "
             "Magicka back where it was before the maximum moved",
             mpf::ReasonText(d.reason), static_cast<unsigned>(g_offMod),
             static_cast<double>(t.deficitAfter), static_cast<double>(d.newDeficit),
             static_cast<double>(d.deltaApplied));

    if (!allowed) {
        MPF_INFO("  DRY RUN: nothing was written. Set bDryRun = 0 in the .conf to let "
                 "the plugin apply this.");
        return;
    }

    const std::uintptr_t addr = g_player + g_offMod;
    if (!mpf::SafeWrite(addr, d.newDeficit)) {
        ++g_stats.writeFailed;
        MPF_ERR("  WRITE FAILED at 0x%llX. The value did not stick, either because "
                  "the page is not writable or because the game overwrote it in the "
                  "same frame. No further attempt is made for this change.",
                  static_cast<unsigned long long>(addr));
        return;
    }

    // Tell the observer what we did, so the value we wrote is not mistaken for a game
    // event on the next poll and does not become a stale "before" for the next real
    // transition.
    g_model.NoteDeficitWrite(d.newDeficit);
    ++g_stats.corrected;
    if (haveMax) {
        MPF_INFO("  WRITE OK: +0x%03X now reads %.4f, estimated magicka %.1f / %.1f",
                 static_cast<unsigned>(g_offMod), static_cast<double>(d.newDeficit),
                 static_cast<double>(maxNow + d.newDeficit), static_cast<double>(maxNow));
    } else {
        MPF_INFO("  WRITE OK: +0x%03X now reads %.4f", static_cast<unsigned>(g_offMod),
                 static_cast<double>(d.newDeficit));
    }
}

// --------------------------------------------------------------------------
// The character, as the log describes it.
//
// Read-only, through SafeRead, and only after three checks: the player object's
// form ID is 0x14, its base form's form ID is 0x07, and that base form carries
// the TESNPC vtable (when RTTI found it). The offsets are in PlayerStats.h and
// are [DEDUCTION] until a log is matched against the character sheet
// (docs/SPEC.md section 10.8). Nothing here feeds the correction.
// --------------------------------------------------------------------------
struct CharacterIds {
    std::uint32_t race = 0, cls = 0, birthsign = 0;
};

std::uint32_t FormIdAt(std::uintptr_t ptrSlot) {
    std::uintptr_t form = 0;
    std::uint32_t  id = 0;
    if (!mpf::SafeRead(ptrSlot, form) || form == 0) { return 0; }
    if (!mpf::SafeRead(form + mpf::off::kFormID, id)) { return 0; }
    return id;
}

// Fills `s` and returns nullptr, or returns why the data cannot be trusted.
const char* ReadCharacter(mpf::BaseStats& s, CharacterIds& ids) {
    static char why[160];
    std::uint32_t refId = 0, baseId = 0;
    std::uintptr_t base = 0, vt = 0;
    if (!mpf::SafeRead(g_player + mpf::off::kFormID, refId)) {
        return "the player object's form ID is not readable";
    }
    if (refId != mpf::kPlayerRefFormID) {
        std::snprintf(why, sizeof(why), "the player object's form ID is 0x%08X, not 0x%08X",
                      refId, mpf::kPlayerRefFormID);
        return why;
    }
    if (!mpf::SafeRead(g_player + mpf::off::kRefBaseForm, base) || base == 0) {
        return "the player's base form pointer is not readable";
    }
    if (g_npcVtable && (!mpf::SafeRead(base, vt) || vt != g_npcVtable)) {
        return "the player's base form does not carry the TESNPC vtable";
    }
    if (!mpf::SafeRead(base + mpf::off::kFormID, baseId)) {
        return "the base form's form ID is not readable";
    }
    if (baseId != mpf::kPlayerBaseFormID) {
        std::snprintf(why, sizeof(why), "the base form's form ID is 0x%08X, not 0x%08X",
                      baseId, mpf::kPlayerBaseFormID);
        return why;
    }
    std::uint8_t ready = 0;
    if (!mpf::SafeRead(base + mpf::off::kNpcLevel, s.level) ||
        !mpf::SafeRead(base + mpf::off::kNpcSpellPoints, s.spellPoints) ||
        !mpf::SafeRead(base + mpf::off::kNpcFatigue, s.fatigue) ||
        !mpf::SafeRead(base + mpf::off::kNpcHealth, s.health) ||
        !mpf::ReadMem(base + mpf::off::kNpcAttributes, s.attribute, sizeof(s.attribute)) ||
        !mpf::ReadMem(base + mpf::off::kNpcSkills, s.skill, sizeof(s.skill)) ||
        !mpf::SafeRead(g_player + mpf::off::kReadyToLevelUp, ready)) {
        return "a field of the base form is not readable";
    }
    s.readyToLevelUp = ready != 0;
    ids.race      = FormIdAt(base + mpf::off::kNpcRace);
    ids.cls       = FormIdAt(base + mpf::off::kNpcClass);
    ids.birthsign = FormIdAt(g_player + mpf::off::kBirthSign);
    return nullptr;
}

// The estimated maximum right now (docs/SPEC.md section 10.8), or false when the
// character data cannot be read. For the log only.
bool EstimateMaxNow(float& out) {
    mpf::BaseStats s;
    CharacterIds   ids;
    float          intMod = 0.0f, magMod = 0.0f;
    if (ReadCharacter(s, ids) != nullptr ||
        !TotalAvModifier(g_player, mpf::kAV_Intelligence, intMod) ||
        !TotalAvModifier(g_player, mpf::kAV_Magicka, magMod)) {
        return false;
    }
    out = mpf::EstimatedMaxMagicka(static_cast<float>(s.attribute[mpf::kAV_Intelligence]),
                                   intMod, static_cast<float>(s.spellPoints), magMod);
    return true;
}

// The modifier with its magic / script / game split when not all of it is magic.
std::string ModifierOf(int av) {
    float src[3];
    return ReadAvSources(g_player, av, src) ? mpf::FormatModifierSources(src)
                                           : std::string("?");
}

const char* PoolState(float deficit) { return deficit >= -0.01f ? "full" : "not full"; }

// The block a bug report needs: who the character is, before any event.
void LogCharacter(const mpf::BaseStats& s, const CharacterIds& ids, float deficit) {
    MPF_INFO("PLAYER level %d | ready to level up: %s | race 0x%08X, class 0x%08X, "
             "birthsign 0x%08X | base health %u, base spell points %u, base fatigue %u "
             "| magicka deficit %.2f (pool %s)",
             s.level, s.readyToLevelUp ? "yes" : "no", ids.race, ids.cls, ids.birthsign,
             s.health, s.spellPoints, s.fatigue, static_cast<double>(deficit),
             PoolState(deficit));

    std::string line;
    for (int i = 0; i < mpf::kAttributeCount; ++i) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s%s %u/%s", i ? ", " : "", mpf::AttributeName(i),
                      s.attribute[i], ModifierOf(i).c_str());
        line += buf;
    }
    MPF_INFO("PLAYER attributes base/modifier (m/s/g = magic/script/game, shown when not "
             "all magic): %s", line.c_str());

    // Skills in actor-value order, which is also combat / magic / stealth, seven each.
    // The letter is the mastery the BASE skill gives (N, A, J, E, M).
    static const char* const kGroup[3] = {"combat", "magic", "stealth"};
    for (int g = 0; g < 3; ++g) {
        line.clear();
        for (int k = 0; k < 7; ++k) {
            const int i = g * 7 + k;
            char buf[112];
            std::snprintf(buf, sizeof(buf), "%s%s %u/%s %c", k ? ", " : "",
                          mpf::SkillName(i), s.skill[i],
                          ModifierOf(mpf::kFirstSkillAV + i).c_str(),
                          mpf::MasteryName(s.skill[i])[0]);
            line += buf;
        }
        MPF_INFO("PLAYER %s skills base/modifier: %s", kGroup[g], line.c_str());
    }

    MPF_INFO("PLAYER derived modifiers: Health %s, Magicka %s, Fatigue %s, Encumbrance %s",
             ModifierOf(mpf::kAV_Health).c_str(), ModifierOf(mpf::kAV_Magicka).c_str(),
             ModifierOf(mpf::kAV_Fatigue).c_str(), ModifierOf(mpf::kAV_Encumbrance).c_str());
    float maxNow = 0.0f;
    if (EstimateMaxNow(maxNow)) {
        MPF_INFO("PLAYER estimated magicka %.1f / %.1f (maximum = 2 x Intelligence + base "
                 "spell points + Magicka modifier; compare with the character sheet)",
                 static_cast<double>(maxNow + deficit), static_cast<double>(maxNow));
    }
}

// One line per kind of change. The deficit is printed before and after, because
// whether a level-up refills magicka is still unmeasured (docs/SPEC.md 10.3).
void LogCharacterChange(const mpf::StatsDiff& d, const mpf::BaseStats& a,
                        const mpf::BaseStats& b, float deficitBefore, float deficitNow) {
    std::string attrs;
    for (const int i : d.attributes) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s%s %u -> %u", attrs.empty() ? "" : ", ",
                      mpf::AttributeName(i), a.attribute[i], b.attribute[i]);
        attrs += buf;
    }
    std::string base;
    auto addBase = [&base](const char* what, unsigned from, unsigned to) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%sbase %s %u -> %u", base.empty() ? "" : ", ",
                      what, from, to);
        base += buf;
    };
    if (d.healthChanged)      { addBase("health", a.health, b.health); }
    if (d.spellPointsChanged) { addBase("spell points", a.spellPoints, b.spellPoints); }
    if (d.fatigueChanged)     { addBase("fatigue", a.fatigue, b.fatigue); }

    char deficit[96];
    std::snprintf(deficit, sizeof(deficit), "magicka deficit %.2f -> %.2f (pool %s)",
                  static_cast<double>(deficitBefore), static_cast<double>(deficitNow),
                  PoolState(deficitNow));

    if (d.levelChanged) {
        MPF_INFO("LEVEL UP %d -> %d | %s%s%s | %s", d.levelFrom, d.levelTo,
                 attrs.empty() ? "no attribute changed" : attrs.c_str(),
                 base.empty() ? "" : ", ", base.c_str(), deficit);
    } else if (!attrs.empty() || !base.empty()) {
        MPF_INFO("BASE VALUES CHANGED without a level-up | %s%s%s | %s", attrs.c_str(),
                 (!attrs.empty() && !base.empty()) ? ", " : "", base.c_str(), deficit);
    }
    for (const int i : d.attributes) {
        if (i != mpf::kAV_Intelligence) { continue; }
        const int delta = static_cast<int>(mpf::kMagickaPerIntelligence) *
                          (static_cast<int>(b.attribute[i]) - static_cast<int>(a.attribute[i]));
        MPF_INFO("  maximum magicka moved by %+d with no correction: base Intelligence "
                 "is not watched, only its modifiers are (docs/SPEC.md section 10.3)",
                 delta);
    }
    if (!d.skills.empty()) {
        std::string skills;
        for (const int i : d.skills) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s%s %u -> %u (%s)", skills.empty() ? "" : ", ",
                          mpf::SkillName(i), a.skill[i], b.skill[i],
                          mpf::MasteryName(b.skill[i]));
            skills += buf;
        }
        MPF_INFO("SKILL UP %s", skills.c_str());
    }
    if (d.readyChanged) {
        MPF_INFO("READY TO LEVEL UP: %s", b.readyToLevelUp ? "yes" : "no");
    }
}

void Heartbeat(const char* hypothesisName, float v, float mod, bool haveV) {
    if (haveV) {
        MPF_INFO("alive | samples=%llu maxChanges=%llu corrected=%llu "
                 "wouldCorrect=%llu policy=%s | "
                 "hypothesis=%s | value=%.2f modifier=%.2f | watch=%llu "
                 "rescans=%llu lost=%llu%s",
                 g_stats.samples, g_stats.maxChanges, g_stats.corrected,
                 g_stats.wouldCorrect,
                 mpf::PolicyName(g_policy.Policy()),
                 hypothesisName, static_cast<double>(v), static_cast<double>(mod),
                 g_stats.watchChanges, g_stats.rescans, g_stats.instanceLost,
                 CostText().c_str());
    } else {
        MPF_INFO("alive | samples=%llu | player=%s | modifier=%.2f | watch=%llu "
                 "rescans=%llu lost=%llu | virtual calls disabled%s",
                 g_stats.samples, g_player ? "found" : "not found",
                 static_cast<double>(mod), g_stats.watchChanges, g_stats.rescans,
                 g_stats.instanceLost, CostText().c_str());
    }
}

// Path of the crash/hang breadcrumb, filled in at startup. Empty means the plugin
// folder was not writable, in which case the guard degrades to timeout-only.
std::string g_sentinelPath;

// Set when a previous launch started a virtual call and never recorded its return.
bool g_vcallPoisoned = false;

void ObserveLoop() {
    MPF_DBG("---- observation ----");

    mpf::KeyEdge dumpKey(g_cfg.dumpOnKeyPress ? g_cfg.dumpKey : 0);
    if (g_cfg.floatWatch) { g_watch.Configure(g_cfg.dumpFrom, g_cfg.dumpTo); }

    const auto* getFn = reinterpret_cast<GetActorFloatValueFn*>(
        g_vtable + static_cast<unsigned>(g_vtblGet) * 8);

    unsigned long long lastBeat  = GetTickCount64();
    unsigned long long lastScan  = 0;
    unsigned long long lastSlotCheck = 0;
    bool  haveBaseline = false;
    float lastMax = 0.0f;
    bool  reportedFound = false;
    bool  dumpedPopulated = false;
    bool  reportedVerdict = false;
    bool  escalated = false;

    // The character block. Written once the object is populated, again after
    // every save load, and followed by one line per change of level, base
    // attribute or base skill. Sampled a few times a second, not every poll.
    constexpr unsigned long long kCharacterSettleMs = 1500;
    constexpr unsigned long long kCharacterPollMs   = 250;
    mpf::BaseStats     lastStats;
    bool               haveStats      = false;

    // The modifier sources of the previous poll, for the per-source breakdown.
    AvSources lastSources{};
    bool      haveSources = false;
    bool               statsRefused   = false;   // reason already logged for this load
    unsigned long long statsDueAt     = 0;       // 0 = nothing scheduled
    int                statsAttempts  = 0;       // failed first reads for this load
    constexpr int      kCharacterAttempts = 5;
    unsigned long long lastStatsPoll  = 0;
    float              deficitAtStats = 0.0f;

    // Virtual calls are earned, not assumed. They stay off until one guarded call
    // has come back, and are abandoned for the rest of the session if it does not.
    bool               vcallProven   = false;
    bool               vcallGivenUp  = false;
    unsigned long long populatedAt   = 0;

    const auto cfgComp = g_cfg.Resolve();

    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_cost.freq = f.QuadPart;
        g_cost.windowStart = QpcNow();
    }

    while (!g_stop.load(std::memory_order_relaxed)) {
        Sleep(static_cast<DWORD>(g_cfg.pollIntervalMs));
        const BusyScope busy;
        const unsigned long long now = GetTickCount64();


        if (!InstanceStillValid()) {
            if (g_player) {
                ++g_stats.instanceLost;
                MPF_INFO("player instance at 0x%llX is no longer valid (a save load, "
                         "a quit to the main menu, or the game closing). Rescanning.",
                         static_cast<unsigned long long>(g_player));
                g_player = 0;
                g_watch.Reset();
                haveStats = false;
                statsRefused = false;
                statsAttempts = 0;
                statsDueAt = 0;
                haveSources = false;
                haveBaseline = false;
                reportedFound = false;
                dumpedPopulated = false;
                populatedAt = 0;
                g_model.Discontinuity("the player object moved");
            }
            // Once the static slot is known, reading it is two checked reads, so it
            // is tried often; a full scan is the fallback, and it is expensive (a
            // memory map refresh, then the game module's writable sections), so it
            // is throttled, much harder once the slot is known. Before a save is
            // loaded there is no player at all, which is normal, not an error.
            if (g_playerSlot && now - lastSlotCheck > 250) {
                lastSlotCheck = now;
                std::uintptr_t cand = 0, vt = 0;
                if (mpf::SafeRead(g_playerSlot, cand) && cand &&
                    mpf::SafeRead(cand, vt) && vt == g_vtable) {
                    g_player = cand;
                    MPF_INFO("player object found at 0x%llX via the static slot found "
                             "earlier", static_cast<unsigned long long>(g_player));
                    continue;
                }
            }
            if (now - lastScan > (g_playerSlot ? 30000ull : 3000ull)) {
                lastScan = now;
                ++g_stats.rescans;
                ScanResult sr;
                const bool found = FindPlayerInstance(2ull << 30, sr);
                if (found) {
                    MPF_INFO("player object found at 0x%llX via %s%s",
                             static_cast<unsigned long long>(g_player), sr.how,
                             sr.slot ? "" : " (no static slot)");
                    if (sr.slot) {
                        g_playerSlot = sr.slot;
                        MPF_DBG("  static slot   : 0x%llX (RVA 0x%X) - a fixed "
                                "address for this game build, no scan needed to "
                                "reach the player again",
                                static_cast<unsigned long long>(sr.slot),
                                g_game.Rva(sr.slot));
                    }
                    MPF_DBG("  scan          : %.1f MB, %d pointer(s) tested, "
                            "%d object(s) with this vtable, %d unreadable chunk(s)",
                            static_cast<double>(sr.scanned) / (1024.0 * 1024.0),
                            sr.derefs, sr.candidates, sr.failedReads);
                    if (sr.candidates > 1) {
                        MPF_ERR("More than one object carries the PlayerCharacter "
                                "vtable. The first was taken; if the readings below "
                                "look wrong, this is why.");
                    }
                } else {
                    MPF_DBG("no player instance yet (%.1f MB scanned, %d pointer(s) "
                            "tested) - expected before a save is loaded",
                            static_cast<double>(sr.scanned) / (1024.0 * 1024.0),
                            sr.derefs);
                }
            }
            if (now - lastBeat >= static_cast<unsigned long long>(g_cfg.heartbeatSeconds) * 1000) {
                lastBeat = now;
                Heartbeat("n/a", 0.0f, 0.0f, false);
            }
            continue;
        }

        if (!reportedFound) {
            reportedFound = true;
            ReportOffsets();
            if (g_cfg.probeEnabled && mpf::Log().Enabled(mpf::Lvl::Debug)) {
                mpf::HexDump(g_player, g_cfg.dumpFrom, g_cfg.dumpTo,
                             "initial snapshot");
                mpf::LogPlausibleFloats(g_player, g_cfg.dumpFrom, g_cfg.dumpTo,
                                        "initial snapshot");
                ++g_stats.dumps;
            }
        }

        ++g_stats.samples;

        // ---- the one field this project ultimately writes -----------------
        float mod = 0.0f;
        if (!mpf::SafeRead(g_player + static_cast<unsigned>(g_offMod), mod)) {
            continue;
        }

        // The instance exists well before a save is loaded, and at that point the
        // whole actor-value block is still zero: the initial snapshot above is a
        // picture of an empty object. This fires one extra automatic dump the
        // first time the field this project cares about stops being zero, which
        // is the first moment the object is genuinely populated.
        // Version 0.5.1 used "the deficit stopped being zero" as the test, which
        // silently broke for the ordinary case of loading a save with a full pool:
        // the deficit is legitimately zero then, so the object was never declared
        // populated and the model watch never primed. The count of plausible
        // non-zero floats in the window is independent of the pool being full. Over
        // four field runs an empty object showed 10 of 482 and a populated one showed
        // 25, 26 and 60, so the threshold sits between those two clusters.
        const int liveFloats = mpf::CountPlausibleFloats(g_player, g_cfg.dumpFrom,
                                                        g_cfg.dumpTo);
        if (!dumpedPopulated && (liveFloats >= kPopulatedFloatFloor || mod != 0.0f)) {
            dumpedPopulated = true;
            populatedAt = now;
            statsDueAt = now + kCharacterSettleMs;
            if (g_cfg.probeEnabled && mpf::Log().Enabled(mpf::Lvl::Debug)) {
                MPF_DBG("---- object populated ----");
                MPF_INFO("the object now carries %d plausible non-zero floats and "
                         "magickaModifier at +0x%03X reads %.4f. The initial snapshot "
                         "was taken before the save finished loading, so this dump "
                         "supersedes it.",
                         liveFloats, g_offMod, static_cast<double>(mod));
                mpf::HexDump(g_player, g_cfg.dumpFrom, g_cfg.dumpTo, "populated");
                mpf::LogPlausibleFloats(g_player, g_cfg.dumpFrom, g_cfg.dumpTo,
                                        "populated");
                ++g_stats.dumps;
            }
        }

        // ---- float watch --------------------------------------------------
        int burst = 0;
        if (g_cfg.floatWatch && g_watch.Configured()) {
            burst = g_watch.Sample(g_player, "poll", mpf::Lvl::Debug);
            g_stats.watchChanges += static_cast<unsigned long long>(burst);
        }

        // ---- maximum-Magicka model test -----------------------------------
        // A save load rewrites the whole object between two polls, which without this
        // would be indistinguishable from a fortify of several hundred points. A
        // spell effect moves a handful of floats; a reload moves dozens. Re-priming
        // on that burst, and on any pointer rescan, is what keeps the events real.
        //
        // The MODEL baseline must be thrown away here, because it describes one
        // snapshot of one object. The POLICY evidence must not: it describes whether
        // this executable rebases the deficit, which is a property of the binary and
        // cannot change because a save was loaded or the player object was reallocated.
        // Resetting it, as 0.9.2 and earlier did, threw away hard-won evidence and
        // delayed the only warning that matters -- observed in the field on 6 Aug 2026,
        // where four confirmations were discarded at 21:01 by a save load.
        if (burst >= kDiscontinuityFloats) {
            g_model.Discontinuity("many floats changed in a single poll");
            // Most likely a save load: describe the character again once it settles.
            haveStats = false;
            statsRefused = false;
            statsAttempts = 0;
            statsDueAt = now + kCharacterSettleMs;
            haveSources = false;
        }
        if (dumpedPopulated) {
            AvSources src;
            if (ReadAvSources(g_player, mpf::kAV_Intelligence, src.intel) &&
                ReadAvSources(g_player, mpf::kAV_Magicka, src.magicka)) {
                const float intTotal = src.intel[0] + src.intel[1] + src.intel[2];
                const float magTotal = src.magicka[0] + src.magicka[1] + src.magicka[2];
                const mpf::Transition t = g_model.Feed(intTotal, magTotal, mod);
                if (t.valid) {
                    if (haveSources) { LogSourceBreakdown(lastSources, src); }
                    HandleTransition(t);
                }
                lastSources = src;
                haveSources = true;
            }

            // ---- the character ----------------------------------------------
            const bool due = statsDueAt != 0 && now >= statsDueAt;
            if (due || (haveStats && now - lastStatsPoll >= kCharacterPollMs)) {
                lastStatsPoll = now;
                mpf::BaseStats cur;
                CharacterIds   ids;
                const char*    why = ReadCharacter(cur, ids);
                if (why && haveStats) {
                    // A failed sample mid-session is a torn or racing read, not a
                    // reason to stop: keep the last good values and try again.
                } else if (why) {
                    // The first read after a load: the object may still be filling
                    // in. Retry a few times, then say why once and stop until the
                    // next load.
                    if (++statsAttempts < kCharacterAttempts) {
                        statsDueAt = now + 1000;
                    } else {
                        statsDueAt = 0;
                        if (!statsRefused) {
                            statsRefused = true;
                            MPF_INFO("character data not logged: %s. Corrections are "
                                     "not affected.", why);
                        }
                    }
                } else if (due || !haveStats) {
                    LogCharacter(cur, ids, mod);
                    lastStats = cur;
                    haveStats = true;
                    statsDueAt = 0;
                    statsAttempts = 0;
                } else if (cur != lastStats) {
                    const mpf::StatsDiff d = mpf::DiffStats(lastStats, cur);
                    if (d.wholesale) {
                        MPF_INFO("character data changed all at once (another save "
                                 "loaded?): describing it again");
                        LogCharacter(cur, ids, mod);
                    } else {
                        LogCharacterChange(d, lastStats, cur, deficitAtStats, mod);
                    }
                    lastStats = cur;
                }
                deficitAtStats = mod;
            }
        }

        // ---- manual dump --------------------------------------------------
        if (dumpKey.Pressed()) {
            ++g_stats.dumps;
            mpf::Log().Separator("manual dump");
            MPF_INFO("magickaModifier at +0x%03X = %.4f", g_offMod,
                     static_cast<double>(mod));
            {
                float it = 0.0f, mt = 0.0f;
                if (TotalAvModifier(g_player, mpf::kAV_Intelligence, it) &&
                    TotalAvModifier(g_player, mpf::kAV_Magicka, mt)) {
                    MPF_INFO("Intelligence modifier total = %.4f, Magicka modifier "
                             "total = %.4f, implied change in maximum from base = "
                             "%+.4f, model events so far = %d",
                             static_cast<double>(it), static_cast<double>(mt),
                             static_cast<double>(mpf::kMagickaPerIntelligence * it + mt),
                             g_model.Events());
                }
            }
            if (g_cfg.floatWatch && g_watch.Configured()) {
                // Diffs against the previous manual dump, not against the poll
                // baseline the poll above has already consumed.
                g_watch.SampleManual(g_player);
            }
            mpf::HexDump(g_player, g_cfg.dumpFrom, g_cfg.dumpTo, "manual");
            mpf::LogPlausibleFloats(g_player, g_cfg.dumpFrom, g_cfg.dumpTo,
                                    "manual");
        }

        // ---- everything below needs a virtual call ------------------------
        if (!g_cfg.callVirtuals) {
            if (now - lastBeat >= static_cast<unsigned long long>(g_cfg.heartbeatSeconds) * 1000) {
                lastBeat = now;
                Heartbeat("n/a", 0.0f, mod, false);
            }
            continue;
        }

        // 0.3.1 called this on an object whose entire actor-value block was still
        // zero, 8.25 seconds into the process and before any save had finished
        // loading: 7 of 482 floats carried a value. Entering a game method on a
        // half-initialised object, from a thread the game knows nothing about, is
        // the worst possible moment to try. Two gates now stand in the way.
        if (!dumpedPopulated) {
            // Gate 1: the object must have proved it is populated, which for this
            // plugin means magickaModifier has stopped being exactly zero.
            if (now - lastBeat >= static_cast<unsigned long long>(g_cfg.heartbeatSeconds) * 1000) {
                lastBeat = now;
                Heartbeat("waiting for a populated object", 0.0f, mod, false);
            }
            continue;
        }
        if (!vcallProven && !vcallGivenUp) {
            // Gate 2: let the game settle after the save finishes loading, then
            // make exactly one attempt, on a thread we can walk away from.
            if (now - populatedAt < kVCallSettleMs) { continue; }

            if (!g_sentinelPath.empty()) {
                char note[512];
                std::snprintf(note, sizeof(note),
                              "MagickaPoolFix " MPF_PLUGIN_VERSION "\r\n"
                              "A guarded call to GetActorFloatValue at vtable index "
                              "0x%03X was started and has not returned yet.\r\n"
                              "If this file is still here at the next launch, that "
                              "call never came back and bCallVirtuals will be forced "
                              "to 0 for this game build.\r\n"
                              "Deleting this file re-arms the attempt.\r\n",
                              g_vtblGet);
                mpf::VCallSentinelWrite(g_sentinelPath, note);
            }
            MPF_INFO("attempting the first guarded call to GetActorFloatValue "
                     "(vtable index 0x%03X) on a throwaway thread, %u ms budget",
                     g_vtblGet, kVCallTimeoutMs);

            static mpf::VCallJob job;  // never freed: the worker may outlive us
            job.fn   = reinterpret_cast<mpf::GetAvFn>(*getFn);
            job.self = reinterpret_cast<void*>(g_player);
            job.av   = mpf::kAV_Magicka;
            job.done = 0;

            float probed = 0.0f;
            if (mpf::VCallGuarded(&job, kVCallTimeoutMs, probed) == mpf::VCallResult::Ok) {
                vcallProven = true;
                if (!g_sentinelPath.empty()) { mpf::VCallSentinelClear(g_sentinelPath); }
                MPF_INFO("guarded call returned %.4f. Virtual calls are usable on "
                         "this build; calibration starts now.",
                         static_cast<double>(probed));
            } else {
                vcallGivenUp = true;
                MPF_ERR("the guarded call did not return within %u ms. Vtable index "
                        "0x%03X is either not GetActorFloatValue on this build, or "
                        "the game will not accept that call from a foreign thread. "
                        "Virtual calls are abandoned for this session; memory "
                        "observation continues normally. The blocked thread is left "
                        "alone on purpose -- killing it could leave a game lock held "
                        "forever.", kVCallTimeoutMs, g_vtblGet);
                mpf::Log().Escalate(200, "guarded virtual call timed out");
                continue;
            }
        }
        if (vcallGivenUp) {
            if (now - lastBeat >= static_cast<unsigned long long>(g_cfg.heartbeatSeconds) * 1000) {
                lastBeat = now;
                Heartbeat("virtual calls abandoned", 0.0f, mod, false);
            }
            continue;
        }

        const float v = (*getFn)(reinterpret_cast<void*>(g_player), mpf::kAV_Magicka);

        // ---- current-versus-maximum calibration ---------------------------
        const auto step = g_cal.Feed(v, mod);
        if (step.eventCounted && mpf::Log().Enabled(mpf::Lvl::Debug)) {
            MPF_DBG("calib | v=%.3f mod=%.3f | candA(v-mod)=%.3f candB(v)=%.3f | "
                    "stableA=%d stableB=%d | scoreA=%d scoreB=%d",
                    static_cast<double>(v), static_cast<double>(mod),
                    static_cast<double>(step.candA), static_cast<double>(step.candB),
                    step.stableA ? 1 : 0, step.stableB ? 1 : 0,
                    g_cal.ScoreA(), g_cal.ScoreB());
        }
        if (step.justResolved && !reportedVerdict) {
            reportedVerdict = true;
            MPF_INFO("CALIBRATION: GetActorFloatValue(Magicka) returns %s "
                     "(scoreA=%d scoreB=%d, %d frames). Resolved maximum = %.2f",
                     mpf::HypothesisName(g_cal.Get()), g_cal.ScoreA(), g_cal.ScoreB(),
                     g_cal.Frames(), static_cast<double>(g_cal.Get() ==
                         mpf::Hypothesis::ValueIsMax ? v : v - mod));
        }
        if (step.justGaveUp && !escalated) {
            escalated = true;
            mpf::Log().Escalate(200, "calibration failed to reach a verdict");
        }

        float current = 0.0f, maxValue = 0.0f;
        if (!g_cal.Resolve(v, mod, current, maxValue)) {
            // No verdict yet: by construction nothing downstream can run, which is
            // exactly the guarantee that no wrong-direction write is ever possible.
            if (now - lastBeat >= static_cast<unsigned long long>(g_cfg.heartbeatSeconds) * 1000) {
                lastBeat = now;
                Heartbeat("undecided", v, mod, true);
            }
            continue;
        }

        // ---- invariant self-test ------------------------------------------
        // current == max + modifier, and modifier <= 0. If this does not hold, the
        // offsets are wrong for this game version and no later build should write.
        const bool invariantOk =
            std::fabs(current - (maxValue + mod)) < 0.05f && mod <= 0.05f;
        if (!invariantOk) {
            ++g_stats.invariantFail;
            if (g_stats.invariantFail == 1) {
                MPF_ERR("INVARIANT FAILED: current=%.3f max=%.3f modifier=%.3f. "
                        "Expected current == max + modifier with modifier <= 0. "
                        "The offsets in this build do not match your game version; "
                        "use the probe to find the right ones before enabling any "
                        "build that writes.",
                        static_cast<double>(current), static_cast<double>(maxValue),
                        static_cast<double>(mod));
                mpf::Log().Escalate(200, "invariant self-test failed");
            }
        }

        // ---- what phase 2 would do ----------------------------------------
        if (!haveBaseline) {
            haveBaseline = true;
            lastMax = maxValue;
            MPF_INFO("baseline | current=%.2f max=%.2f modifier=%.2f | invariant %s",
                     static_cast<double>(current), static_cast<double>(maxValue),
                     static_cast<double>(mod), invariantOk ? "holds" : "FAILED");
        } else if (std::fabs(maxValue - lastMax) > cfgComp.epsilon) {
            ++g_stats.maxChanges;
            const auto r = mpf::ComputeCompensation(lastMax, maxValue, mod, cfgComp);
            if (r.resync) {
                MPF_INFO("max changed %.2f -> %.2f : delta beyond the safety rail, "
                         "would resync without correcting",
                         static_cast<double>(lastMax), static_cast<double>(maxValue));
            } else if (r.apply) {
                ++g_stats.wouldCorrect;
                MPF_INFO("MAX CHANGED %.2f -> %.2f (delta %+.2f) | vanilla would give "
                         "current=%.2f | WOULD SET modifier %.2f -> %.2f giving "
                         "current=%.2f | DRY RUN, nothing written",
                         static_cast<double>(lastMax), static_cast<double>(maxValue),
                         static_cast<double>(r.delta),
                         static_cast<double>(maxValue + mod),
                         static_cast<double>(mod),
                         static_cast<double>(r.newModifier),
                         static_cast<double>(r.newCurrent));
            } else {
                MPF_INFO("max changed %.2f -> %.2f : no correction needed",
                         static_cast<double>(lastMax), static_cast<double>(maxValue));
            }
            lastMax = maxValue;
        }

        if (now - lastBeat >= static_cast<unsigned long long>(g_cfg.heartbeatSeconds) * 1000) {
            lastBeat = now;
            Heartbeat(mpf::HypothesisName(g_cal.Get()), v, mod, true);
        }
    }

    MPF_INFO("observation thread stopping");
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

DWORD WINAPI InitThread(LPVOID) {
    // Single-instance guard. An ASI loader scans the game directory as well as the
    // scripts, plugins and update subfolders, so the same .asi present in two of
    // them is loaded twice. Two instances would truncate each other's log on the
    // same path and, in a later build, run two correction loops on one float.
    // The mutex is intentionally never released: the process owns it for its life.
    g_singleton = CreateMutexW(nullptr, TRUE, L"Local\\" MPF_PLUGIN_NAME ".singleton");
    if (!g_singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        // Exit without touching the log: it belongs to the first instance, which
        // reports the duplicate itself from the module inventory.
        return 0;
    }

    const std::string dir = mpf::ModuleDirectory(g_self);
    const std::string confPath = dir + MPF_PLUGIN_NAME ".conf";

    const bool confExisted = mpf::LoadConfig(confPath, g_cfg);

    // The log and the config live next to the .asi, whatever folder that is. If the
    // game is installed somewhere the process may not write to, fall back to the
    // temp directory rather than run with no log at all: a silent plugin cannot be
    // told apart from a plugin that never loaded.
    std::string logPath = dir + g_cfg.logFile;
    bool logFellBack = false;
    if (!mpf::Log().Init(logPath.c_str(), g_cfg.logLevel, g_cfg.maxLines) &&
        g_cfg.logLevel != mpf::Lvl::Off) {
        char tmp[MAX_PATH] = {0};
        const DWORD n = GetTempPathA(MAX_PATH, tmp);
        if (n > 0 && n < MAX_PATH) {
            const std::string alt = std::string(tmp) + g_cfg.logFile;
            if (mpf::Log().Init(alt.c_str(), g_cfg.logLevel, g_cfg.maxLines)) {
                logFellBack = true;
                logPath = alt;
            }
        }
    }
    if (!mpf::Log().IsOpen() && g_cfg.logLevel != mpf::Lvl::Off) {
        return 0;  // nowhere to report anything; do nothing rather than run blind
    }

    LoadProfileFolder();
    LogStartupSummary(confExisted);
    if (logFellBack) {
        MPF_ERR("The log could not be created next to the plugin, so it was written to "
                "%s instead.", Anon(logPath).c_str());
    }
    if (g_cfg.unwritable) {
        MPF_ERR("%s does not exist and could not be created (%s, error %lu). This run "
                "uses the compiled defaults; any edit you make elsewhere is ignored.",
                Anon(confPath).c_str(), mpf::FileErrorText(g_cfg.fileError),
                g_cfg.fileError);
    }
    if (g_cfg.unreadable) {
        MPF_ERR("%s exists but could not be read (%s, error %lu). It was left "
                "untouched, and this run uses the compiled defaults, not your settings. "
                "Close whatever program holds it, or fix its permissions, then restart "
                "the game.", Anon(confPath).c_str(), mpf::FileErrorText(g_cfg.fileError),
                g_cfg.fileError);
    }

    // A breadcrumb left by a previous launch means that launch entered a game
    // method and never came out. Never repeat that automatically.
    g_sentinelPath = mpf::VCallSentinelPath(dir);
    if (mpf::VCallSentinelPresent(g_sentinelPath)) {
        g_vcallPoisoned = true;
        if (g_cfg.callVirtuals) {
            g_cfg.callVirtuals = false;
            MPF_ERR("%s exists, so the previous launch started a call into game code "
                    "and never recorded its return. bCallVirtuals is forced to 0 for "
                    "this launch. Memory observation is unaffected and is where all "
                    "the useful data comes from anyway. Delete that file to try "
                    "again, ideally after checking iGetActorFloatValue.",
                    Anon(g_sentinelPath).c_str());
        }
    }

    g_offMod  = g_cfg.Off(g_cfg.offMagickaModifier, mpf::kDefaultOff_MagickaModifier);
    g_offMagicAV  = g_cfg.Off(g_cfg.offMagicAVModifiers,  mpf::kDefaultOff_MagicAVModifiers);
    g_offScriptAV = g_cfg.Off(g_cfg.offScriptAVModifiers, mpf::kDefaultOff_ScriptAVModifiers);
    g_offGameAV   = g_cfg.Off(g_cfg.offGameAVModifiers,   mpf::kDefaultOff_GameAVModifiers);
    g_comp = g_cfg.Resolve();
    g_policy.Configure(g_cfg.epsilon,
                       static_cast<mpf::EnginePolicy>(g_cfg.enginePolicy),
                       g_cfg.policyConfirmations);
    g_model.Configure(g_cfg.epsilon);
    g_model.SetDeficitOffset(static_cast<int>(g_offMod));
    g_vtblGet = g_cfg.Off(g_cfg.vtblGetActorFloat, mpf::kDefaultVtbl_GetActorFloatValue);

    if (g_cfg.forceHypothesis == 1) {
        g_cal.Force(mpf::Hypothesis::ValueIsCurrent);
        MPF_INFO("iForceHypothesis = 1 : auto-calibration disabled, assuming "
                 "GetActorFloatValue returns CURRENT magicka");
    } else if (g_cfg.forceHypothesis == 2) {
        g_cal.Force(mpf::Hypothesis::ValueIsMax);
        MPF_INFO("iForceHypothesis = 2 : auto-calibration disabled, assuming "
                 "GetActorFloatValue returns MAXIMUM magicka");
    }

    // An ASI loader runs us very early. Give the executable time to be fully
    // mapped before parsing its headers.
    Sleep(2000);

    if (!LoadGameModule()) {
        MPF_ERR("Could not parse the game executable's PE headers. Stopping.");
        return 0;
    }
    if (g_cfg.moduleInventory) { LogModuleInventory(); }
    if (!DiscoverVtable()) { return 0; }
    DiscoverNpcVtable();

    g_mem.Refresh();
    MPF_DBG("memory snapshot : %zu readable regions, %zu private read-write "
             "regions totalling %.0f MB",
             g_mem.Count(), g_mem.HeapCount(),
             static_cast<double>(g_mem.HeapBytes()) / (1024.0 * 1024.0));

    ObserveLoop();
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_self = static_cast<HMODULE>(inst);
        DisableThreadLibraryCalls(inst);
        // All work happens off the loader lock: doing any of this inside DllMain
        // is a classic way to deadlock a game at startup.
        g_thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        // Signal and let go. Waiting for the thread here can deadlock, and there
        // is nothing to undo: no code was patched, and the only writes are data.
        g_stop.store(true, std::memory_order_relaxed);
        break;

    default:
        break;
    }
    return TRUE;
}
