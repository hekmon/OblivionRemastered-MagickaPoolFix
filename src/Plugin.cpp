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
//     must stay off: on the tested build it hangs or faults (SPEC.md section 9.1).
//
// See SPEC.md for the full model and the sources behind every number.

#include "PCH.h"

#include "Config.h"
#include "Corrector.h"
#include "HypothesisCalibrator.h"
#include "Log.h"
#include "MagickaMath.h"
#include "ModelWatch.h"
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
std::uintptr_t g_player   = 0;   // PlayerCharacter instance
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
static bool TotalAvModifier(std::uintptr_t obj, int avIndex, float& out) {
    out = 0.0f;
    const int bases[3] = { g_offMagicAV, g_offScriptAV, g_offGameAV };
    for (const int base : bases) {
        if (base <= 0) { continue; }
        float v = 0.0f;
        const unsigned off = static_cast<unsigned>(base + avIndex * 4);
        if (!mpf::SafeRead(obj + off, v)) { return false; }
        if (!std::isfinite(v)) { return false; }
        out += v;
    }
    return true;
}
int            g_vtblGet  = 0;   // resolved GetActorFloatValue index

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
    MPF_INFO("game module   : %s", g_game.Name().c_str());
    MPF_INFO("  path        : %s", g_game.Path().c_str());
    MPF_INFO("  base        : 0x%llX  size 0x%llX",
             static_cast<unsigned long long>(g_game.Base()),
             static_cast<unsigned long long>(g_game.ImageSize()));
    MPF_INFO("  entry point : 0x%llX",
             static_cast<unsigned long long>(g_game.Entry()));
    MPF_INFO("  PE timestamp: 0x%08lX (identifies the game build)",
             static_cast<unsigned long>(g_game.Timestamp()));
    MPF_INFO("  sections    : %zu", g_game.Sections().size());
    for (const auto& s : g_game.Sections()) {
        MPF_INFO("    %-8s 0x%llX  size 0x%-8llX  %s%s", s.name,
                 static_cast<unsigned long long>(s.start),
                 static_cast<unsigned long long>(s.size),
                 s.Executable() ? "X" : "-", s.Writable() ? "W" : "-");
    }
    return true;
}

void LogModuleInventory() {
    const auto rows = mpf::EnumerateModules();
    mpf::Log().Separator("loaded modules");
    MPF_INFO("%zu modules in the process. Modules outside the Windows directory "
             "are listed with their full path: those are the game, the mods and "
             "the mod loaders.", rows.size());

    // Print the full path for anything that is not a stock Windows DLL. Two
    // distinct products can present the same module name -- a mod loader
    // installed as a proxy DLL is literally named after the system library it
    // stands in for -- so the bare name is not enough to tell what is loaded.
    // The directory is what identifies it, and reading this inventory without it
    // invites exactly that misidentification.
    const std::string winDir = mpf::WindowsDirLower();

    int selfCopies = 0;
    for (const auto& r : rows) {
        std::string lower = r.path;
        for (auto& c : lower) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        const bool system = !winDir.empty() && lower.compare(0, winDir.size(), winDir) == 0;
        if (system) {
            MPF_INFO("  %-40s 0x%llX  size 0x%llX", r.name.c_str(),
                     static_cast<unsigned long long>(r.base),
                     static_cast<unsigned long long>(r.size));
        } else {
            MPF_INFO("  %-40s 0x%llX  size 0x%llX  <- %s", r.name.c_str(),
                     static_cast<unsigned long long>(r.base),
                     static_cast<unsigned long long>(r.size), r.path.c_str());
        }
        if (_stricmp(r.name.c_str(), MPF_PLUGIN_NAME ".asi") == 0) { ++selfCopies; }
    }

    // An ASI loader scans several folders (the game directory, plus scripts,
    // plugins and update). A copy of the plugin sitting in two of them is loaded
    // twice into the same process, which would mean two observation threads and,
    // in a later build, two independent correction loops writing the same float.
    // The singleton guard already stopped the second instance; say where it was.
    if (selfCopies > 1) {
        MPF_ERR("%d copies of " MPF_PLUGIN_NAME ".asi are loaded in this process. "
                "Only one is active, the others exited immediately. Delete the "
                "duplicates: keep a single copy, in one folder only.", selfCopies);
        for (const auto& r : rows) {
            if (_stricmp(r.name.c_str(), MPF_PLUGIN_NAME ".asi") == 0) {
                MPF_ERR("  copy: %s", r.path.c_str());
            }
        }
    }
}

bool DiscoverVtable() {
    mpf::Log().Separator("RTTI discovery");

    // Enough entries for GetActorFloatValue at 0x0B1 plus headroom.
    const std::size_t minEntries = static_cast<std::size_t>(g_vtblGet) + 8;

    mpf::RttiSearchStats stats;
    const auto loc = mpf::FindVtableByRtti(g_game, ".?AVPlayerCharacter@@",
                                           minEntries, stats);

    MPF_INFO("searched %d data sections", stats.sectionsScanned);
    MPF_INFO("  name  \".?AVPlayerCharacter@@\" : %d hit(s)", stats.nameHits);
    MPF_INFO("  object locator                 : %d candidate(s)", stats.locatorHits);
    MPF_INFO("  vtable references              : %d candidate(s)", stats.vtableHits);

    if (!loc.typeDescriptor) {
        MPF_ERR("RTTI type descriptor not found. Either the game binary was built "
                "without RTTI, or it is packed/obfuscated. Nothing further is "
                "possible without hard-coded addresses; stopping.");
        return false;
    }
    MPF_INFO("  type descriptor : 0x%llX (RVA 0x%X)",
             static_cast<unsigned long long>(loc.typeDescriptor),
             g_game.Rva(loc.typeDescriptor));

    if (!loc.locator) {
        MPF_ERR("No self-consistent RTTICompleteObjectLocator references that "
                "descriptor. Stopping.");
        return false;
    }
    MPF_INFO("  object locator  : 0x%llX",
             static_cast<unsigned long long>(loc.locator));

    if (!loc.Valid()) {
        MPF_ERR("Vtable rejected: only %zu leading entries point into executable "
                "code, fewer than the %zu required for index 0x%X. This build's "
                "vtable indices do not fit your game version.",
                loc.validEntries, minEntries, g_vtblGet);
        return false;
    }

    g_vtable = loc.vtable;
    MPF_INFO("  vtable          : 0x%llX (RVA 0x%X), %zu valid leading entries",
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
    MPF_INFO("  slot 0x%03X Update(float)        -> 0x%llX (RVA 0x%X)",
             g_cfg.Off(g_cfg.vtblUpdate, mpf::kDefaultVtbl_Update),
             static_cast<unsigned long long>(fnUpdate), g_game.Rva(fnUpdate));
    MPF_INFO("  slot 0x%03X GetActorFloatValue   -> 0x%llX (RVA 0x%X)",
             g_vtblGet, static_cast<unsigned long long>(fnGet), g_game.Rva(fnGet));
    return true;
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
    g_mem.Refresh();
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

void LogStartupBanner(const std::string& confPath, bool confExisted) {
    // The banner must state what this run will actually DO. A log that says "no
    // memory writes" above six write confirmations is worse than no log at all,
    // because it costs the reader their trust in every other line.
    MPF_INFO("%s %s  -  %s", MPF_PLUGIN_NAME, MPF_PLUGIN_VERSION,
             g_cfg.dryRun ? "DRY RUN: observing only, no memory writes"
                          : "ACTIVE: correcting the Magicka pool in memory");
    MPF_INFO("built %s %s", __DATE__, __TIME__);
    MPF_INFO("config : %s%s", confPath.c_str(),
             confExisted        ? ""
             : g_cfg.generated  ? "  (did not exist, defaults written)"
                                : "  (missing and NOT creatable)");

    mpf::Log().Separator("configuration");
    MPF_INFO("iPreset          = %s", mpf::PresetName(g_cfg.preset));
    MPF_INFO("bDryRun          = %d", g_cfg.dryRun ? 1 : 0);
    MPF_INFO("iLevel           = %d", static_cast<int>(g_cfg.logLevel));
    MPF_INFO("iHeartbeatSeconds= %d", g_cfg.heartbeatSeconds);
    MPF_INFO("iMaxLines        = %ld", g_cfg.maxLines);
    MPF_INFO("bEnabled (probe) = %d", g_cfg.probeEnabled ? 1 : 0);
    MPF_INFO("bFloatWatch      = %d", g_cfg.floatWatch ? 1 : 0);
    MPF_INFO("bCallVirtuals    = %d", g_cfg.callVirtuals ? 1 : 0);
    MPF_INFO("bHeapScan        = %d (fallback sweep, normally off)",
             g_cfg.heapScan ? 1 : 0);
    MPF_INFO("iPollIntervalMs  = %d", g_cfg.pollIntervalMs);
    MPF_INFO("iForceHypothesis = %d", g_cfg.forceHypothesis);

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
    if (!confExisted && g_cfg.generated) {
        MPF_INFO("A fully commented %s has just been written next to the plugin. It "
                 "documents every option and what the fix costs in balance terms.",
                 MPF_PLUGIN_NAME ".conf");
    }

    // Say once, unambiguously, what this run will do, rather than letting the player
    // wonder why their game did or did not change.
    mpf::Log().Separator("scope of this run");
    if (g_cfg.dryRun) {
        MPF_INFO("bDryRun = 1, so iPreset and [Magicka] are parsed, and every correction "
                 "is computed and reported, but NOT written. No game memory is modified "
                 "and nothing in your game will behave differently. Set bDryRun = 0 in "
                 "the .conf to let it act.");
        MPF_INFO("What to check in this log: that the player object was found, that the "
                 "float watch shows a float tracking your Magicka, and that the DECISION "
                 "lines say what you would expect.");
    } else {
        MPF_INFO("bDryRun = 0, so this run WRITES to game memory. Each correction touches "
                 "one float, the Magicka modifier at +0x6B8 of the player object, and is "
                 "read back to confirm it took. No function is hooked and no game code is "
                 "patched.");
        MPF_INFO("What to check in this log: a WRITE OK line under every DECISION, and no "
                 "line saying the engine behaviour disagrees with iEnginePolicy.");
    }
}

void ReportOffsets() {
    mpf::Log().Separator("offsets in use");
    const bool manual = g_cfg.offMagickaModifier >= 0 ||
                        g_cfg.offMagicAVModifiers >= 0 ||
                        g_cfg.offScriptAVModifiers >= 0 ||
                        g_cfg.offGameAVModifiers >= 0 ||
                        g_cfg.vtblUpdate >= 0 || g_cfg.vtblGetActorFloat >= 0;
    MPF_INFO("magickaModifier   = 0x%03X %s", g_offMod,
             g_cfg.offMagickaModifier < 0 ? "(compiled default)" : "(from .conf)");
    MPF_INFO("magicAVModifiers  = 0x%03X",
             g_cfg.Off(g_cfg.offMagicAVModifiers, mpf::kDefaultOff_MagicAVModifiers));
    MPF_INFO("scriptAVModifiers = 0x%03X",
             g_cfg.Off(g_cfg.offScriptAVModifiers, mpf::kDefaultOff_ScriptAVModifiers));
    MPF_INFO("gameAVModifiers   = 0x%03X",
             g_cfg.Off(g_cfg.offGameAVModifiers, mpf::kDefaultOff_GameAVModifiers));
    MPF_INFO("GetActorFloatValue index = 0x%03X", g_vtblGet);
    MPF_INFO("These are deduced from the commonlibob64 reverse-engineered headers, "
             "not read from your binary. Confirming them is this build's job.");

    // Steam ships OblivionRemastered-Win64-Shipping.exe, Game Pass ships
    // OblivionRemastered-WinGDK-Shipping.exe. They are two separate compilations,
    // so offsets confirmed on one are not established on the other. The module
    // name and PE timestamp logged above are what identifies the build a set of
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
void HandleTransition(const mpf::Transition& t) {
    ++g_stats.maxChanges;

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
    MPF_INFO("  WRITE OK: +0x%03X now reads %.4f", static_cast<unsigned>(g_offMod),
             static_cast<double>(d.newDeficit));
}

void Heartbeat(const char* hypothesisName, float v, float mod, bool haveV) {
    if (haveV) {
        MPF_INFO("alive | samples=%llu maxChanges=%llu corrected=%llu "
                 "wouldCorrect=%llu policy=%s | "
                 "hypothesis=%s | value=%.2f modifier=%.2f | watch=%llu "
                 "rescans=%llu lost=%llu",
                 g_stats.samples, g_stats.maxChanges, g_stats.corrected,
                 g_stats.wouldCorrect,
                 mpf::PolicyName(g_policy.Policy()),
                 hypothesisName, static_cast<double>(v), static_cast<double>(mod),
                 g_stats.watchChanges, g_stats.rescans, g_stats.instanceLost);
    } else {
        MPF_INFO("alive | samples=%llu | player=%s | modifier=%.2f | watch=%llu "
                 "rescans=%llu lost=%llu | virtual calls disabled",
                 g_stats.samples, g_player ? "found" : "not found",
                 static_cast<double>(mod), g_stats.watchChanges, g_stats.rescans,
                 g_stats.instanceLost);
    }
}

// Path of the crash/hang breadcrumb, filled in at startup. Empty means the plugin
// folder was not writable, in which case the guard degrades to timeout-only.
std::string g_sentinelPath;

// Set when a previous launch started a virtual call and never recorded its return.
bool g_vcallPoisoned = false;

void ObserveLoop() {
    mpf::Log().Separator("observation");

    mpf::KeyEdge dumpKey(g_cfg.dumpOnKeyPress ? g_cfg.dumpKey : 0);
    if (g_cfg.floatWatch) { g_watch.Configure(g_cfg.dumpFrom, g_cfg.dumpTo); }

    const auto* getFn = reinterpret_cast<GetActorFloatValueFn*>(
        g_vtable + static_cast<unsigned>(g_vtblGet) * 8);

    unsigned long long lastBeat  = GetTickCount64();
    unsigned long long lastScan  = 0;
    unsigned long long lastRefresh = 0;
    bool  haveBaseline = false;
    float lastMax = 0.0f;
    bool  reportedFound = false;
    bool  dumpedPopulated = false;
    bool  reportedVerdict = false;
    bool  escalated = false;

    // Virtual calls are earned, not assumed. They stay off until one guarded call
    // has come back, and are abandoned for the rest of the session if it does not.
    bool               vcallProven   = false;
    bool               vcallGivenUp  = false;
    unsigned long long populatedAt   = 0;

    const auto cfgComp = g_cfg.Resolve();

    while (!g_stop.load(std::memory_order_relaxed)) {
        Sleep(static_cast<DWORD>(g_cfg.pollIntervalMs));
        const unsigned long long now = GetTickCount64();

        // Refresh the page snapshot periodically: the heap grows and shrinks, and
        // a stale snapshot would make valid reads look unreadable.
        if (now - lastRefresh > 2000) {
            g_mem.Refresh();
            lastRefresh = now;
        }

        if (!InstanceStillValid()) {
            if (g_player) {
                ++g_stats.instanceLost;
                MPF_INFO("player instance at 0x%llX is no longer valid (save load, "
                         "or the object moved). Rescanning.",
                         static_cast<unsigned long long>(g_player));
                g_player = 0;
                g_watch.Reset();
                haveBaseline = false;
                reportedFound = false;
                dumpedPopulated = false;
                populatedAt = 0;
                g_model.Discontinuity("the player object moved");
            }
            // Rescanning is expensive, so throttle it. Before a save is loaded
            // there is no player at all, which is normal, not an error.
            if (now - lastScan > 3000) {
                lastScan = now;
                ++g_stats.rescans;
                ScanResult sr;
                const bool found = FindPlayerInstance(2ull << 30, sr);
                if (found) {
                    MPF_INFO("player instance : 0x%llX  via %s",
                             static_cast<unsigned long long>(g_player), sr.how);
                    if (sr.slot) {
                        MPF_INFO("  static slot   : 0x%llX (RVA 0x%X) - a fixed "
                                 "address for this game build, no scan needed to "
                                 "reach the player again",
                                 static_cast<unsigned long long>(sr.slot),
                                 g_game.Rva(sr.slot));
                    }
                    MPF_INFO("  scan          : %.1f MB, %d pointer(s) tested, "
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
            if (g_cfg.probeEnabled) {
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
            if (g_cfg.probeEnabled) {
                mpf::Log().Separator("object populated");
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
        }
        if (dumpedPopulated) {
            float intTotal = 0.0f;
            float magTotal = 0.0f;
            if (TotalAvModifier(g_player, mpf::kAV_Intelligence, intTotal) &&
                TotalAvModifier(g_player, mpf::kAV_Magicka, magTotal)) {
                const mpf::Transition t = g_model.Feed(intTotal, magTotal, mod);
                if (t.valid) { HandleTransition(t); }
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

    LogStartupBanner(confPath, confExisted);
    if (logFellBack) {
        MPF_ERR("The plugin's own folder is not writable, so this log was written to "
                "%s instead. The .conf could not be created there either: the plugin "
                "is running on compiled defaults.", logPath.c_str());
    }
    if (g_cfg.unwritable) {
        MPF_ERR("%s does not exist and could not be created. Running on compiled "
                "defaults; any edit you make elsewhere will be ignored.",
                confPath.c_str());
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
                    g_sentinelPath.c_str());
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

    g_mem.Refresh();
    MPF_INFO("memory snapshot : %zu readable regions, %zu private read-write "
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
