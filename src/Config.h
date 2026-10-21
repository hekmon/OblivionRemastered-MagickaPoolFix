// Config.h - MagickaPoolFix
//
// A .conf file sitting next to the .asi, written with the defaults when it is
// missing, and never overwritten once it exists, even when it cannot be read.
// Convention borrowed from "Delete Spells - OBSE - ASI" (Nexus 4934): Hungarian
// b/i/f/s prefixes, automatic regeneration of the defaults.
//
// The file DOUBLES AS THE README: every option is documented with its balance
// implications and the URLs of its sources. Since an ASI plugin has no console,
// this is the main channel for explaining anything to the player.
//
// Decision (docs/SPEC.md section 7.1): the [Magicka] section is only read when
// iPreset = 3. There is no -1 sentinel in [Magicka] because a custom profile has
// no reference value to fall back to; the sentinel stays legitimate in [Offsets],
// where the reference is the compiled value, documented in a trailing comment.

#pragma once

#include "PCH.h"
#include "Log.h"          // for mpf::Lvl
#include "MagickaMath.h"

#include <string>

namespace mpf {

enum class Preset : std::int32_t {
    Off      = 0,
    VanillaPlus = 1,
    Glass    = 2,
    Custom   = 3,
};

struct Config {
    // [General]
    Preset preset  = Preset::Glass;
    bool   dryRun  = false;  // the write path is validated in game; see docs/SPEC.md section 4.6

    // [Magicka] - only read when preset == Custom
    bool  preserveOnIncrease = true;
    bool  preserveOnDecrease = true;
    bool  spillOverflow      = true;
    bool  floorAtZero        = true;
    float epsilon            = 0.01f;
    float maxDeltaGuard      = 100000.0f;

    // [Log]
    Lvl         logLevel = Lvl::Info;
    std::string logFile  = MPF_PLUGIN_NAME ".log";
    int         heartbeatSeconds = 60;   // hard floor of 60 enforced
    long        maxLines = 200000;

    // [Probe]
    // These compiled defaults deliberately match the values written in the
    // generated .conf, so a first run with no config file behaves exactly like a
    // run with the shipped one.
    bool probeEnabled     = true;
    bool dumpOnKeyPress   = true;
    int  dumpKey          = 0x79;        // VK_F10, see the .conf for why
    int  dumpFrom         = 0x400;
    int  dumpTo           = kDefaultSizeof_PlayerCharacter;
    bool moduleInventory  = true;
    // Fallback heap search. Off by default: the static-slot search covers the
    // normal case, and a full heap traversal is slow and pointless when it does.
    bool heapScan         = false;
    bool floatWatch       = true;
    bool callVirtuals     = false;       // off by default, see the .conf
    int  pollIntervalMs   = 16;

    // [Offsets] - -1 means "use the compiled value"
    int offMagickaModifier   = -1;
    int offMagicAVModifiers  = -1;
    int offScriptAVModifiers = -1;
    int offGameAVModifiers   = -1;
    int vtblUpdate           = -1;
    int vtblGetActorFloat    = -1;
    int forceHypothesis      = 0;        // 0 auto, 1 current, 2 max

    // [Advanced] engine-policy calibration, see Corrector.h.
    int enginePolicy         = 1;        // 0 auto-calibrate, 1 ignores, 2 handles
    int policyConfirmations  = 1;

    // Parse diagnostics, reported at startup.
    bool generated       = false;  // the file was missing and has just been created
    bool unwritable      = false;  // the file was missing AND could not be created
    bool unreadable      = false;  // the file exists but could not be read: left untouched
    unsigned long fileError = 0;   // Windows error behind unwritable or unreadable
    bool magickaIgnored  = false;  // [Magicka] was edited while iPreset != 3
    bool heartbeatRaised = false;  // iHeartbeatSeconds < 60, raised to the floor

    // Profile -> booleans resolution (docs/SPEC.md sections 7.1 and 7.2).
    CompensationConfig Resolve() const {
        CompensationConfig c;
        c.epsilon     = epsilon;
        c.maxAbsDelta = maxDeltaGuard;
        switch (preset) {
        case Preset::Off:
            c.preserveOnIncrease = false;
            c.preserveOnDecrease = false;
            c.clampOverflow      = false;
            c.clampNegative      = false;
            break;
        case Preset::VanillaPlus:
            c.preserveOnIncrease = false;
            c.preserveOnDecrease = false;
            c.clampOverflow      = true;
            c.clampNegative      = true;
            break;
        case Preset::Glass:
            c.preserveOnIncrease = true;
            c.preserveOnDecrease = true;
            c.clampOverflow      = true;
            c.clampNegative      = true;
            break;
        case Preset::Custom:
            c.preserveOnIncrease = preserveOnIncrease;
            c.preserveOnDecrease = preserveOnDecrease;
            c.clampOverflow      = spillOverflow;
            c.clampNegative      = floorAtZero;
            break;
        }
        return c;
    }

    bool Active() const { return preset != Preset::Off; }

    int Off(int configured, int compiled) const {
        return configured < 0 ? compiled : configured;
    }
};

// --------------------------------------------------------------------------
// Default contents. This is the plugin's readme: editing it means editing the
// documentation the player actually reads.
// --------------------------------------------------------------------------
inline const char* DefaultConfigText() {
    return
R"CONF(; ============================================================================
;  MagickaPoolFix - configuration
;  The Elder Scrolls IV: Oblivion Remastered - ASI plugin
;
;  This file is written with the defaults when it is missing, so you can always
;  delete it to get a clean, fully commented copy back. A file that exists is
;  never overwritten, even one the plugin cannot read: the log says so instead.
;  It doubles as the mod's readme: every option below is documented.
; ============================================================================
;
;  WHAT THIS MOD DOES
;  ------------------
;  When your maximum Magicka changes, the engine does not adjust your current
;  Magicka accordingly. Internally the game does not store your current pool as
;  an absolute number; it stores your maximum plus a negative "damage" offset:
;
;      current = maximum + magickaModifier        (magickaModifier <= 0)
;
;  Nothing recalculates that offset when the maximum moves, which produces three
;  symptoms of one defect:
;
;    1. Casting Fortify Magicka or Fortify Intelligence RAISES your current
;       Magicka by the full amount. At 100/300, a +300 buff gives you 400/600 -
;       300 points of Magicka out of thin air.
;
;    2. When the buff expires, the same amount is taken back, even if you were
;       nowhere near full.
;
;    3. So the pool can go negative. At 100/600 the buff ending leaves you at
;       -200/300: no spellcasting at all until regeneration climbs back to zero.
;
;  For the original game, UESP documents Fortify Magicka as unable to take
;  current Magicka below zero when it ends, while Fortify Intelligence can.
;  In Remastered, a spell carrying both, ending at a low pool, was measured
;  going from 52 to -248: the whole 300 taken back.
;    https://en.uesp.net/wiki/Oblivion:Spell_Making#Spell_Chaining
;
;  THE MODEL: A GLASS OF WATER
;  ---------------------------
;  Making the glass bigger does not create water in it.
;  Making it smaller spills the overflow, but does not remove what still fits.
;
;    100/300 + 300 buff   ->  100/600   (vanilla: 400/600)
;    100/600, buff ends   ->  100/300   (vanilla: -200/300)
;    350/600, buff ends   ->  300/300   (vanilla: 50/300)
;
;  WHAT IT COSTS - READ THIS FIRST
;  -------------------------------
;  Preset 2 removes free Magicka on activation. It is a fix, and like any fix
;  that closes an exploit, it changes the balance for whoever relied on it.
;  Two consequences you should know about:
;
;  * You now need a preparation window. Buff your pool, wait for it to fill,
;    then strike. In Oblivion Remastered, regeneration depends only on
;    Willpower and no longer scales with your maximum pool:
;
;        regen/sec = 2 * (Willpower/50)^2 + 1.5 * (Willpower/50)
;        halved while in combat, and paused for 2 seconds after each cast
;        https://en.uesp.net/wiki/Oblivion:Willpower ("Oblivion Remastered")
;
;    That is the formula the plugin's own measurements confirm. Two other UESP
;    pages give different ones; docs/SPEC.md section 3.3 has the comparison.
;
;    Willpower 100 -> 11.0/sec out of combat, 5.5/sec in combat.
;    Willpower 150 -> 22.5/sec out of combat, 11.25/sec in combat.
;    Willpower 200 -> 38.0/sec out of combat, 19.0/sec in combat.
;
;    That curve is quadratic, so Fortify Willpower is by far the cheapest way
;    to make preset 2 comfortable.
;
;    Vanilla clamps the skill term of the spell cost equation to 100, so one
;    fifth of base cost is the floor no matter how high your magic skill goes.
;    https://en.uesp.net/wiki/Oblivion:Spell_Making
;    Skill-uncapping mods can lift that clamp, in which case cost keeps
;    falling as 20% * 100/skill -- decreasing returns, and never free.
;
;    Note for vampires: vampirism fortifies Willpower by +5 to +20 depending on
;    how long since you last fed, so your refill speed swings by up to 37%
;    without anything else changing. Vampirism never touches Intelligence or
;    Magicka, so it changes how fast the glass fills, never its size.
;    https://en.uesp.net/wiki/Oblivion:Vampirism
;
;    So fortifying Willpower is what makes this playable. If you only fortify
;    Magicka, filling +300 in combat at Willpower 100 takes about 55 seconds,
;    and the buff may well expire first. In Remastered one spell can carry
;    several Fortify Attribute effects, so Fortify Willpower can ride along
;    with Fortify Intelligence.
;      https://en.uesp.net/wiki/Oblivion:Spell_Making
;
;  * Spell chaining stops working. UESP documents loops built on the free
;    Magicka of Fortify effects, and on Drain Magicka turning a negative pool
;    back to zero; one variant keeps maximum Magicka above 1500 while it runs.
;      https://en.uesp.net/wiki/Oblivion:Spell_Making#Spell_Chaining
;    Preset 2 removes the free Magicka. Preset 1 keeps it, but by never letting
;    the pool go negative it also breaks the variants that need a negative
;    pool. If you rely on those, set iPreset = 0.
;
;  If none of that appeals to you, set iPreset = 1: it only stops current
;  Magicka from going negative, and keeps the free Magicka (see its note).
;
; ============================================================================

[General]

; Which behaviour profile to use.
;
;   0 = Off
;       No memory writes at all. Logging and the probe still work, so this is
;       the setting to use when you only want diagnostics.
;
;   1 = Vanilla+
;       Keeps vanilla behaviour, including free Magicka on activation, but
;       never lets current Magicka go below zero. Pick this if you just want
;       the -200/300 state gone. It is NOT balance-neutral: the free Magicka is
;       still granted when a buff starts, and the debt it leaves when the buff
;       ends is forgiven, so chaining fortify spells becomes a net Magicka gain
;       (docs/SPEC.md section 4.4).
;
;   2 = Glass  (default)
;       The full model described above. Current Magicka is preserved when the
;       maximum changes, the overflow is spilled, and the floor at zero
;       applies. It closes the free-Magicka exploit, which changes the balance;
;       read "WHAT IT COSTS".
;
;   3 = Custom
;       Read the four booleans in [Magicka] below.
iPreset = 2

; Dry run. When 1, the plugin computes and logs exactly what it WOULD change but
; performs no memory writes whatsoever.
;
; Ships at 0: the mod corrects. Every write is one float, read back afterwards to
; confirm it took, and reported as WRITE OK or WRITE FAILED. Nothing is silent.
;
; Set this to 1 if you want to see what the mod would do to your game before
; letting it do anything. In that mode, look for the DECISION lines in the log
; next to this file: each one says what would have been written and why. If the
; numbers match what you saw on your Magicka bar, set it back to 0.
bDryRun = 0


[Magicka]

; ---------------------------------------------------------------------------
; READ ONLY WHEN iPreset = 3. With any other preset these values are ignored.
;
; They are pre-filled with the exact behaviour of preset 2, so they are a
; working starting point rather than an empty form. Change iPreset to 3 first,
; then adjust.
; ---------------------------------------------------------------------------

; Maximum goes UP: keep current Magicka where it was (1) or let it rise with
; the maximum, i.e. free Magicka (0).
bPreserveOnIncrease = 1

; Maximum goes DOWN: keep current Magicka where it was (1) or let it fall by
; the same amount, which is what produces negative pools (0).
bPreserveOnDecrease = 1

; Spill the overflow: if preserving would leave current above the new maximum,
; clamp it down to the maximum.
bSpillOverflow = 1

; Floor at zero: never allow current Magicka below zero.
bFloorAtZero = 1

; Float comparison tolerance. Changes smaller than this are treated as noise.
fEpsilon = 0.01

; Safety rail. A maximum-Magicka change larger than this is assumed to be a
; save load or a game state reset rather than a spell, and is left
; uncorrected. Changes above 2000 points are already refused before this
; check, so the default never triggers; it only matters if set lower.
fMaxDeltaGuard = 100000.0


[Log]

; 0 = Off, 1 = Error only, 2 = Info, 3 = Debug, 4 = Trace (one line per frame).
;
; Level 2 starts with a one-line summary, the loaded mods and the character
; (level, attributes, estimated magicka), then writes one line per change of
; maximum magicka, plus level-ups and anything that needs your attention.
; Level 3 adds the detail of every change, skills and skill-ups, the plugin's
; own cost, and the startup details (configuration, game sections, every loaded
; module, discovery, memory dumps). Use 3 when testing or reporting a problem.
; Level 4 is for short diagnostic sessions only - it is bounded by iMaxLines.
iLevel = 2

; Log file name, created next to the .asi file. Truncated at every game start;
; there is deliberately no rotation, so the folder stays clean.
sFile = MagickaPoolFix.log

; Heartbeat interval. Since an ASI plugin has no console, this single-line
; summary is how you confirm the plugin is alive and working. Values below 60
; are raised to 60.
iHeartbeatSeconds = 60

; Maximum lines per session, 0 for unlimited. Protects against a runaway log
; at iLevel = 4.
iMaxLines = 200000


; =============================================================================
;
;      A D V A N C E D   --   Y O U   C A N   S T O P   R E A D I N G   H E R E
;
;   Everything above this line is what a player is expected to touch: pick a
;   preset and you are done. The defaults below are the right ones for normal
;   play and nothing below changes how Magicka behaves.
;
;   What follows is diagnostic and reverse-engineering machinery: a memory
;   probe, a hex dumper, a float watcher, and hand-pinned memory offsets. It
;   exists so that the mod can be repaired after a game update without a
;   disassembler, and so that a bug report can carry real evidence.
;
;   Two reasons to go further, and only two:
;     - the log says the player instance or the offsets could not be found
;       after a game update, and you want to help re-discover them;
;     - you were asked for a diagnostic log in a bug report.
;
;   Changing values below can make the plugin read the wrong memory, log
;   megabytes of noise, or -- for iGetActorFloatValue and bCallVirtuals only --
;   crash the game. None of it can corrupt a save.
;
; =============================================================================


[Probe]

; The probe is a diagnostic tool kept permanently in the plugin. It is how the
; memory offsets below can be re-discovered after a game update, without
; needing a disassembler.
;
; HOW TO RE-DISCOVER THE MAGICKA OFFSET
;   1. Keep bEnabled = 1, bDumpOnKeyPress = 1 and bFloatWatch = 1.
;   2. In game, with full Magicka, press the dump key.
;   3. Cast a spell whose cost you know, then press it again.
;   4. The float watch names the offset that moved and by how much. The float
;      that dropped by exactly the spell's cost IS magickaModifier. Put its
;      offset in iMagickaModifier below.
;   Same method for the per-attribute arrays: toggle a Fortify buff on and off
;   and watch which array entries move.
;
; bEnabled turns on the automatic memory dumps of the player object at startup.
; They are written at iLevel = 3 only. The dump key below works at any level.
bEnabled = 1

; Dump the player structure when the key below is pressed.
bDumpOnKeyPress = 1

; Windows Virtual-Key code of the dump key.
;
; Pick a key the game does not already use. This plugin only OBSERVES the key, it
; does not consume it, so a key shared with a game action triggers both at once.
; Oblivion Remastered binds these function keys, so do not use them:
;     F1  Character      F3  Magic
;     F5  Quicksave      F6  Quest        F7  Help        F8  System (like Escape)
;     F9  Quick Load
; F12 is Steam's screenshot key by default, so avoid that one too.
;
; Free and safe: 0x79 = F10 (the default), 0x71 = F2, 0x73 = F4, 0x7A = F11.
iDumpKey = 0x79

; Byte range of the player structure to dump.
iDumpFrom = 0x400
iDumpTo   = 0xB88

; Float watch. Snapshots every aligned float in the range above and then reports
; only the offsets that CHANGED, with old value, new value and delta. This is what
; makes offset re-discovery practical: cast a spell of known cost and look for the
; offset that moved by exactly that amount, instead of diffing hex by eye.
; Changes are reported at Debug level, and always on a manual dump.
bFloatWatch = 1

; At startup, write the loaders and mods found in the game process, one line,
; each with the timestamp of its own file (every module at iLevel = 3). This is
; how a log shows what else is installed.
bModuleInventory = 1

; Fallback search for the player object.
;
; The plugin normally finds the player by looking for a pointer to it inside the
; game executable's own writable data, which is fast and cannot fail badly. If a
; future game build stops keeping the player in a static slot, that search comes
; up empty and this option enables the old brute-force sweep of the heap instead.
; It reads several hundred megabytes every three seconds, so leave it off unless
; the log says no player instance was found long after a save was loaded.
bHeapScan = 0

; Call the game's own GetActorFloatValue to read Magicka. Leave this at 0.
;
; On this game build (PE timestamp 0xF19077A4) that call cannot be made from
; the plugin's thread. Two runs on 6 August 2026 showed it:
;   - version 0.3.1 called it once, and the call never returned. The plugin's
;     thread was lost; the game carried on.
;   - version 0.4.0 called it again, on a throwaway thread with a 2 second
;     timeout. It faulted inside game code (a write to address zero, module
;     offset 0x458F57C) and the game ended with a UE4SS "Fatal Error".
; A timeout survives a hang. Nothing survives a fault: an access violation
; belongs to the whole process.
;
; Nothing needs it. Current Magicka is not stored: the engine keeps a deficit
; and computes current = maximum + deficit on demand, and the change in maximum
; follows from the modifier arrays. Reading memory is enough.
;
; If a launch ever starts the call and does not record its return, the file
; MagickaPoolFix.vcall is left next to the plugin, and the next launch forces
; this option to 0. Deleting that file re-arms the attempt.
bCallVirtuals = 0

; How often the observation thread samples the player, in milliseconds. 16 is
; roughly one frame at 60 fps. Clamped to the 4..1000 range.
iPollIntervalMs = 16


[Offsets]

; -1 means "use the value compiled into this build", shown in the comment.
; Only touch these after a game update, if the log's first line says the game
; is not the validated build and the DECISION lines no longer match what your
; Magicka bar does. Use the probe above to find the new values.
;
; Source of the compiled defaults: reverse-engineered headers from
;   https://github.com/libxse/commonlibob64/blob/main/include/RE/P/PlayerCharacter.h
; confirmed in game on the executable with PE timestamp 0xF19077A4.

iMagickaModifier    = -1       ; 0x6B8, float, <= 0
iMagicAVModifiers   = -1       ; 0x474, float[72], magic-sourced modifiers
iScriptAVModifiers  = -1       ; 0x594, float[72]
iGameAVModifiers    = -1       ; 0x6C0, float[72]
iUpdateVtableIndex  = -1       ; 0x099, PlayerCharacter::Update(float)
iGetActorFloatValue = -1       ; 0x0B1, PlayerCharacter::GetActorFloatValue(int)

; What GetActorFloatValue(Magicka) returns, for the calibration that runs only
; when bCallVirtuals = 1, which this game build does not allow. Leave it at 0.
;   0 = auto-calibrate
;   1 = it returns CURRENT magicka
;   2 = it returns MAXIMUM magicka
iForceHypothesis = 0

; ---------------------------------------------------------------------------
; Engine behaviour
; ---------------------------------------------------------------------------
; The correction is relative: it subtracts the change in maximum from the stored
; deficit. That is right only if the game did not already adjust the deficit
; itself. Doing it on top of a game that already compensates would subtract the
; change twice and take Magicka away from you.
;
; This was measured, not assumed: on the shipping executable with PE timestamp
; 0xF19077A4 the game does NOT adjust it, which is the whole reason this mod
; exists. So the default is 1, and the mod corrects from the first frame with no
; warm-up.
;
; Whatever you set here, the plugin keeps watching. Changes that cost nothing at
; the moment they happen -- a fortification expiring, an enchanted item equipped
; or removed, a potion -- show the game's behaviour directly, and the plugin
; compares each of them against this setting. If they ever disagree it says so in
; the log in capitals. That is what a future game patch would look like.
;
;   0 = work it out from what happens.
;       The plugin corrects nothing ambiguous until it has seen one change that
;       cost nothing. A fortification you cast on YOURSELF is ambiguous, because
;       the spell is charged in the same frame that raises your maximum. So with
;       0 the mod does nothing on your first self-cast fortification of each
;       session. Pick this only if you distrust the default.
;   1 = assume the game leaves the pool alone, and correct (default, measured)
;   2 = assume the game preserves the pool, and never correct.
;       Set this if the log tells you your game disagrees with the default.
iEnginePolicy = 1

; How many unambiguous changes must agree before iEnginePolicy = 0 trusts the
; answer. Ignored for any other value. 1 is enough: the two possibilities differ
; by the entire size of the change, so there is no near-miss to guard against.
iPolicyConfirmations = 1
)CONF";
}

// --------------------------------------------------------------------------
// Reading and writing
// --------------------------------------------------------------------------

namespace detail {

inline std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) { return {}; }
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

inline std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return s;
}

// Accepts both 0x-prefixed hex and plain decimal.
inline bool ParseInt(const std::string& v, int& out) {
    if (v.empty()) { return false; }
    char* end = nullptr;
    const long r = std::strtol(v.c_str(), &end,
                               (v.size() > 2 && v[0] == '0' && Lower(v).at(1) == 'x') ? 16 : 10);
    if (end == v.c_str()) { return false; }
    out = static_cast<int>(r);
    return true;
}

inline bool ParseLong(const std::string& v, long& out) {
    int i = 0;
    if (!ParseInt(v, i)) { return false; }
    out = i;
    return true;
}

inline bool ParseFloat(const std::string& v, float& out) {
    if (v.empty()) { return false; }
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (end == v.c_str()) { return false; }
    out = static_cast<float>(d);
    return true;
}

inline bool ParseBool(const std::string& v, bool& out) {
    int i = 0;
    if (!ParseInt(v, i)) { return false; }
    out = (i != 0);
    return true;
}

}  // namespace detail

// Plain words for the Windows errors a player can actually fix.
inline const char* FileErrorText(unsigned long e) {
    switch (e) {
    case ERROR_ACCESS_DENIED:     return "access denied";
    case ERROR_SHARING_VIOLATION: return "in use by another program";
    case ERROR_LOCK_VIOLATION:    return "locked by another program";
    case ERROR_PATH_NOT_FOUND:    return "folder not found";
    case ERROR_WRITE_PROTECT:     return "write-protected";
    case ERROR_DISK_FULL:         return "disk full";
    case ERROR_DIRECTORY:         return "a folder has that name";
    default:                      return "Windows error";
    }
}

// Creates the file with the default text, CRLF line endings, and fails rather
// than replace anything: CREATE_NEW refuses a file that already exists, so this
// can never overwrite a player's settings, whatever happens between the check in
// LoadConfig and this call. A partial write removes its own half-written file.
inline bool WriteDefaultConfig(const std::string& path, unsigned long& err) {
    std::string text;
    for (const char* p = DefaultConfigText(); *p; ++p) {
        if (*p == '\n') { text += '\r'; }
        text += *p;
    }
    const HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written,
                              nullptr);
    err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok || written != text.size()) {
        if (err == 0) { err = ERROR_WRITE_FAULT; }
        DeleteFileA(path.c_str());
        return false;
    }
    return true;
}

// Why an existing file cannot be opened for reading, as a Windows error code.
inline unsigned long ReadErrorOf(const std::string& path, DWORD attributes) {
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) { return ERROR_DIRECTORY; }
    const HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return GetLastError(); }
    CloseHandle(h);
    return 0;  // readable now: it was a transient failure
}

// Returns whether the file existed. Besides a normal read, two outcomes, each
// reported in the log by the caller:
//   - missing: the defaults are written (generated), or could not be (unwritable);
//   - present but unreadable: it is left untouched (unreadable).
// Whenever the file was not read, the compiled defaults, already in cfg, apply.
inline bool LoadConfig(const std::string& path, Config& cfg) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        const DWORD why = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
        if (attributes == INVALID_FILE_ATTRIBUTES &&
            (why == ERROR_FILE_NOT_FOUND || why == ERROR_PATH_NOT_FOUND)) {
            unsigned long err = 0;
            cfg.generated  = WriteDefaultConfig(path, err);
            cfg.unwritable = !cfg.generated;
            cfg.fileError  = err;
            return false;
        }
        // It exists, or whether it exists cannot be established. Either way it is
        // not ours to replace.
        cfg.unreadable = true;
        cfg.fileError  = attributes == INVALID_FILE_ATTRIBUTES ? why
                                                               : ReadErrorOf(path, attributes);
        return true;
    }

    // Remember the [Magicka] values as they stood so we can detect a silent edit
    // made while iPreset != 3.
    bool sawMagicka = false;
    Config asRead = cfg;

    std::string section;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = detail::Trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') { continue; }
        if (s.front() == '[') {
            const auto e = s.find(']');
            if (e != std::string::npos) {
                section = detail::Lower(detail::Trim(s.substr(1, e - 1)));
            }
            continue;
        }
        const auto eq = s.find('=');
        if (eq == std::string::npos) { continue; }
        const std::string key = detail::Lower(detail::Trim(s.substr(0, eq)));
        std::string val = detail::Trim(s.substr(eq + 1));
        // Strip a trailing comment.
        const auto sc = val.find(';');
        if (sc != std::string::npos) { val = detail::Trim(val.substr(0, sc)); }
        if (val.empty()) { continue; }

        int i = 0;

        if (section == "general") {
            if (key == "ipreset" && detail::ParseInt(val, i)) {
                if (i >= 0 && i <= 3) { cfg.preset = static_cast<Preset>(i); }
            } else if (key == "bdryrun") {
                detail::ParseBool(val, cfg.dryRun);
            }
        } else if (section == "magicka") {
            sawMagicka = true;
            if (key == "bpreserveonincrease") { detail::ParseBool(val, cfg.preserveOnIncrease); }
            else if (key == "bpreserveondecrease") { detail::ParseBool(val, cfg.preserveOnDecrease); }
            else if (key == "bspilloverflow") { detail::ParseBool(val, cfg.spillOverflow); }
            else if (key == "bflooratzero") { detail::ParseBool(val, cfg.floorAtZero); }
            else if (key == "fepsilon") { detail::ParseFloat(val, cfg.epsilon); }
            else if (key == "fmaxdeltaguard") { detail::ParseFloat(val, cfg.maxDeltaGuard); }
        } else if (section == "log") {
            if (key == "ilevel" && detail::ParseInt(val, i)) {
                if (i < 0) { i = 0; }
                if (i > 4) { i = 4; }
                cfg.logLevel = static_cast<Lvl>(i);
            } else if (key == "sfile") { cfg.logFile = val; }
            else if (key == "iheartbeatseconds" && detail::ParseInt(val, i)) {
                if (i < 60) { i = 60; cfg.heartbeatRaised = true; }
                cfg.heartbeatSeconds = i;
            } else if (key == "imaxlines") { detail::ParseLong(val, cfg.maxLines); }
        } else if (section == "probe") {
            if (key == "benabled") { detail::ParseBool(val, cfg.probeEnabled); }
            else if (key == "bdumponkeypress") { detail::ParseBool(val, cfg.dumpOnKeyPress); }
            else if (key == "idumpkey" && detail::ParseInt(val, i)) { cfg.dumpKey = i; }
            else if (key == "idumpfrom" && detail::ParseInt(val, i)) { cfg.dumpFrom = i; }
            else if (key == "idumpto" && detail::ParseInt(val, i)) { cfg.dumpTo = i; }
            else if (key == "bmoduleinventory") { detail::ParseBool(val, cfg.moduleInventory); }
            else if (key == "bheapscan")        { detail::ParseBool(val, cfg.heapScan); }
            else if (key == "bfloatwatch") { detail::ParseBool(val, cfg.floatWatch); }
            else if (key == "bcallvirtuals") { detail::ParseBool(val, cfg.callVirtuals); }
            else if (key == "ipollintervalms" && detail::ParseInt(val, i)) {
                if (i < 4) { i = 4; }
                if (i > 1000) { i = 1000; }
                cfg.pollIntervalMs = i;
            }
        } else if (section == "offsets") {
            if (key == "imagickamodifier" && detail::ParseInt(val, i)) { cfg.offMagickaModifier = i; }
            else if (key == "imagicavmodifiers" && detail::ParseInt(val, i)) { cfg.offMagicAVModifiers = i; }
            else if (key == "iscriptavmodifiers" && detail::ParseInt(val, i)) { cfg.offScriptAVModifiers = i; }
            else if (key == "igameavmodifiers" && detail::ParseInt(val, i)) { cfg.offGameAVModifiers = i; }
            else if (key == "iupdatevtableindex" && detail::ParseInt(val, i)) { cfg.vtblUpdate = i; }
            else if (key == "igetactorfloatvalue" && detail::ParseInt(val, i)) { cfg.vtblGetActorFloat = i; }
            else if (key == "iforcehypothesis" && detail::ParseInt(val, i)) {
                if (i >= 0 && i <= 2) { cfg.forceHypothesis = i; }
            }
            else if (key == "ienginepolicy" && detail::ParseInt(val, i)) {
                if (i >= 0 && i <= 2) { cfg.enginePolicy = i; }
            }
            else if (key == "ipolicyconfirmations" && detail::ParseInt(val, i)) {
                if (i >= 1 && i <= 100) { cfg.policyConfirmations = i; }
            }
        }
    }
    std::fclose(f);

    // Explicit warning: [Magicka] edited without setting iPreset = 3.
    if (sawMagicka && cfg.preset != Preset::Custom) {
        const bool differs =
            cfg.preserveOnIncrease != asRead.preserveOnIncrease ||
            cfg.preserveOnDecrease != asRead.preserveOnDecrease ||
            cfg.spillOverflow      != asRead.spillOverflow      ||
            cfg.floorAtZero        != asRead.floorAtZero;
        cfg.magickaIgnored = differs;
    }

    // Clamp the dump range.
    if (cfg.dumpFrom < 0) { cfg.dumpFrom = 0; }
    if (cfg.dumpTo <= cfg.dumpFrom) { cfg.dumpTo = cfg.dumpFrom + 0x100; }
    if (cfg.dumpTo > 0x4000) { cfg.dumpTo = 0x4000; }

    return true;
}

inline const char* PresetName(Preset p) {
    switch (p) {
    case Preset::Off:         return "0 Off";
    case Preset::VanillaPlus: return "1 Vanilla+ (floor at zero only)";
    case Preset::Glass:       return "2 Glass (preserve current pool)";
    case Preset::Custom:      return "3 Custom ([Magicka] is read)";
    }
    return "?";
}

// Every setting whose value in use differs from the shipped default, as
// "key=value" pairs, or "none". One line in the log tells a bug report reader
// what this player changed, without the whole configuration dump.
inline std::string NonDefaultSettings(const Config& c) {
    const Config d{};
    std::string out;
    char buf[96];
    auto add = [&out](const char* text) {
        if (!out.empty()) { out += ", "; }
        out += text;
    };
    auto i = [&](const char* key, long v, long dv) {
        if (v == dv) { return; }
        std::snprintf(buf, sizeof(buf), "%s=%ld", key, v);
        add(buf);
    };
    auto h = [&](const char* key, int v, int dv) {
        if (v == dv) { return; }
        std::snprintf(buf, sizeof(buf), "%s=0x%X", key, static_cast<unsigned>(v));
        add(buf);
    };
    auto f = [&](const char* key, float v, float dv) {
        if (v == dv) { return; }
        std::snprintf(buf, sizeof(buf), "%s=%g", key, static_cast<double>(v));
        add(buf);
    };
    auto b = [&](const char* key, bool v, bool dv) { i(key, v ? 1 : 0, dv ? 1 : 0); };

    i("iPreset", static_cast<long>(c.preset), static_cast<long>(d.preset));
    b("bDryRun", c.dryRun, d.dryRun);
    b("bPreserveOnIncrease", c.preserveOnIncrease, d.preserveOnIncrease);
    b("bPreserveOnDecrease", c.preserveOnDecrease, d.preserveOnDecrease);
    b("bSpillOverflow", c.spillOverflow, d.spillOverflow);
    b("bFloorAtZero", c.floorAtZero, d.floorAtZero);
    f("fEpsilon", c.epsilon, d.epsilon);
    f("fMaxDeltaGuard", c.maxDeltaGuard, d.maxDeltaGuard);
    i("iLevel", static_cast<long>(c.logLevel), static_cast<long>(d.logLevel));
    if (c.logFile != d.logFile) {
        std::snprintf(buf, sizeof(buf), "sFile=%.80s", c.logFile.c_str());
        add(buf);
    }
    i("iHeartbeatSeconds", c.heartbeatSeconds, d.heartbeatSeconds);
    i("iMaxLines", c.maxLines, d.maxLines);
    b("bEnabled", c.probeEnabled, d.probeEnabled);
    b("bDumpOnKeyPress", c.dumpOnKeyPress, d.dumpOnKeyPress);
    h("iDumpKey", c.dumpKey, d.dumpKey);
    h("iDumpFrom", c.dumpFrom, d.dumpFrom);
    h("iDumpTo", c.dumpTo, d.dumpTo);
    b("bModuleInventory", c.moduleInventory, d.moduleInventory);
    b("bHeapScan", c.heapScan, d.heapScan);
    b("bFloatWatch", c.floatWatch, d.floatWatch);
    b("bCallVirtuals", c.callVirtuals, d.callVirtuals);
    i("iPollIntervalMs", c.pollIntervalMs, d.pollIntervalMs);
    h("iMagickaModifier", c.offMagickaModifier, d.offMagickaModifier);
    h("iMagicAVModifiers", c.offMagicAVModifiers, d.offMagicAVModifiers);
    h("iScriptAVModifiers", c.offScriptAVModifiers, d.offScriptAVModifiers);
    h("iGameAVModifiers", c.offGameAVModifiers, d.offGameAVModifiers);
    h("iUpdateVtableIndex", c.vtblUpdate, d.vtblUpdate);
    h("iGetActorFloatValue", c.vtblGetActorFloat, d.vtblGetActorFloat);
    i("iForceHypothesis", c.forceHypothesis, d.forceHypothesis);
    i("iEnginePolicy", c.enginePolicy, d.enginePolicy);
    i("iPolicyConfirmations", c.policyConfirmations, d.policyConfirmations);
    return out.empty() ? std::string("none") : out;
}

}  // namespace mpf
