// PlayerStats.h - MagickaPoolFix
//
// What the log says about the character: level, base attributes and base skills,
// how they change during play, and how paths are anonymised before they are
// written. No Windows dependency, so all of it is covered by host tests.
//
// Where the values come from (read in Plugin.cpp, never written):
//
//   PlayerCharacter + 0x10   formID, 0x14 for the player reference
//   PlayerCharacter + 0x50   OBJ_REFR.objectReference, the base form (TESNPC)
//   PlayerCharacter + 0x41C  readyToLevelUp (bool)
//   PlayerCharacter + 0x928  birthSign (form pointer)
//   TESNPC + 0x10            formID, 0x07 for the player's base NPC
//   TESNPC + 0x50            ACTOR_BASE_DATA (the ACBS record): base spell points
//                            +0x04, fatigue +0x06, level +0x0A
//   TESNPC + 0xF0            base health (TESHealthForm)
//   TESNPC + 0x100           base attributes, 8 bytes (TESAttributes)
//   TESNPC + 0x1B0           race (TESRaceForm, form pointer)
//   TESNPC + 0x1B8           base skills, 21 bytes (NPC_DATA)
//   TESNPC + 0x1D0           class (form pointer)
//
// Offsets: commonlibob64 headers (PlayerCharacter.h, TESObjectREFR.h, OBJ_REFR.h,
// TESForm.h, TESNPC.h, TESActorBase.h and its base components), with the TESNPC
// base-class offsets summed from their declared sizes. The field order matches
// the NPC_ record format (ACBS, DATA) documented on UESP. Form IDs 0x14 and 0x07:
// UESP Oblivion:Player and CS wiki "Player". Actor-value order: CS wiki
// "Stats List". All of it is [DEDUCTION] until a log is checked against the
// character sheet in game: docs/SPEC.md section 10.8.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mpf {

constexpr int kAttributeCount = 8;
constexpr int kSkillCount     = 21;
constexpr int kFirstSkillAV   = 12;   // Armorer; skills run 12..32

constexpr std::uint32_t kPlayerRefFormID  = 0x14;
constexpr std::uint32_t kPlayerBaseFormID = 0x07;

namespace off {
constexpr unsigned kFormID          = 0x10;    // any TESForm
constexpr unsigned kRefBaseForm     = 0x50;    // TESObjectREFR -> TESBoundObject*
constexpr unsigned kReadyToLevelUp  = 0x41C;   // PlayerCharacter
constexpr unsigned kBirthSign       = 0x928;   // PlayerCharacter
constexpr unsigned kNpcSpellPoints  = 0x54;    // TESNPC: 0x48 + 0x08 + 0x04
constexpr unsigned kNpcFatigue      = 0x56;    // TESNPC: 0x48 + 0x08 + 0x06
constexpr unsigned kNpcLevel        = 0x5A;    // TESNPC: 0x48 + 0x08 + 0x0A
constexpr unsigned kNpcHealth       = 0xF0;    // TESNPC: 0xE8 + 0x08
constexpr unsigned kNpcAttributes   = 0x100;   // TESNPC: 0xF8 + 0x08
constexpr unsigned kNpcRace         = 0x1B0;   // TESNPC: 0x1A8 + 0x08
constexpr unsigned kNpcSkills       = 0x1B8;   // TESNPC: NPC_DATA
constexpr unsigned kNpcClass        = 0x1D0;   // TESNPC: _class
}  // namespace off

// Short names, in actor-value order (CS wiki, Stats List).
inline const char* AttributeName(int i) {
    static const char* const k[kAttributeCount] = {
        "Str", "Int", "Wil", "Agi", "Spd", "End", "Per", "Luc"};
    return (i >= 0 && i < kAttributeCount) ? k[i] : "?";
}
inline const char* SkillName(int i) {
    static const char* const k[kSkillCount] = {
        "Armorer", "Athletics", "Blade", "Block", "Blunt", "HandToHand", "HeavyArmor",
        "Alchemy", "Alteration", "Conjuration", "Destruction", "Illusion", "Mysticism",
        "Restoration",
        "Acrobatics", "LightArmor", "Marksman", "Mercantile", "Security", "Sneak",
        "Speechcraft"};
    return (i >= 0 && i < kSkillCount) ? k[i] : "?";
}

// Mastery follows the BASE skill: 0-24 Novice, 25-49 Apprentice, 50-74
// Journeyman, 75-99 Expert, 100 Master (UESP, Oblivion:Skills). Fortify effects
// do not unlock perks.
inline const char* MasteryName(int base) {
    if (base >= 100) { return "Master"; }
    if (base >= 75)  { return "Expert"; }
    if (base >= 50)  { return "Journeyman"; }
    if (base >= 25)  { return "Apprentice"; }
    return "Novice";
}

struct BaseStats {
    std::int16_t  level = 0;
    std::uint16_t spellPoints = 0;
    std::uint16_t fatigue = 0;
    std::uint32_t health = 0;
    std::uint8_t  attribute[kAttributeCount] = {};
    std::uint8_t  skill[kSkillCount] = {};
    bool          readyToLevelUp = false;

    bool operator==(const BaseStats& o) const {
        return level == o.level && spellPoints == o.spellPoints &&
               fatigue == o.fatigue && health == o.health &&
               std::memcmp(attribute, o.attribute, sizeof(attribute)) == 0 &&
               std::memcmp(skill, o.skill, sizeof(skill)) == 0 &&
               readyToLevelUp == o.readyToLevelUp;
    }
    bool operator!=(const BaseStats& o) const { return !(*this == o); }
};

// What changed between two samples of the same character.
struct StatsDiff {
    bool levelChanged = false;
    int  levelFrom = 0, levelTo = 0;
    std::vector<int> attributes;   // indices that changed
    std::vector<int> skills;       // indices that changed
    bool readyChanged = false;
    bool healthChanged = false, spellPointsChanged = false, fatigueChanged = false;
    // True when the change cannot be one game event: most likely a save of
    // another character, or of the same one at another time, was loaded.
    bool wholesale = false;

    bool Any() const {
        return levelChanged || !attributes.empty() || !skills.empty() || readyChanged ||
               healthChanged || spellPointsChanged || fatigueChanged;
    }
};

// A Remastered level-up raises the level by one and at most 3 attributes
// (UESP, Oblivion:Remastered_Leveling). A skill rises one point at a time in play,
// a few at most between two samples (training, a skill book). Anything bigger is
// treated as a different save, and re-baselined rather than reported as progress.
constexpr int kMaxAttributesPerLevelUp  = 3;
constexpr int kMaxSkillsPerSample       = 3;
constexpr int kMaxSkillStepPerSample    = 5;

inline StatsDiff DiffStats(const BaseStats& a, const BaseStats& b) {
    StatsDiff d;
    d.levelChanged = a.level != b.level;
    d.levelFrom = a.level;
    d.levelTo = b.level;
    for (int i = 0; i < kAttributeCount; ++i) {
        if (a.attribute[i] != b.attribute[i]) { d.attributes.push_back(i); }
    }
    bool skillJump = false;
    for (int i = 0; i < kSkillCount; ++i) {
        if (a.skill[i] != b.skill[i]) {
            d.skills.push_back(i);
            const int step = static_cast<int>(b.skill[i]) - static_cast<int>(a.skill[i]);
            if (step < 0 || step > kMaxSkillStepPerSample) { skillJump = true; }
        }
    }
    d.readyChanged       = a.readyToLevelUp != b.readyToLevelUp;
    d.healthChanged      = a.health != b.health;
    d.spellPointsChanged = a.spellPoints != b.spellPoints;
    d.fatigueChanged     = a.fatigue != b.fatigue;

    const int levelStep = static_cast<int>(b.level) - static_cast<int>(a.level);
    d.wholesale = (d.levelChanged && levelStep != 1) ||
                  static_cast<int>(d.attributes.size()) > kMaxAttributesPerLevelUp ||
                  static_cast<int>(d.skills.size()) > kMaxSkillsPerSample || skillJump;
    return d;
}

// A modifier total as the log prints it: "+12" when whole, "+12.34" otherwise.
inline std::string FormatModifier(float v) {
    char buf[32];
    const float r = static_cast<float>(static_cast<long>(v >= 0 ? v + 0.5f : v - 0.5f));
    if (v - r < 0.005f && r - v < 0.005f) {
        std::snprintf(buf, sizeof(buf), "%+ld", static_cast<long>(r));
    } else {
        std::snprintf(buf, sizeof(buf), "%+.2f", static_cast<double>(v));
    }
    return buf;
}

// True when `path` lies inside the folder `base` (or is `base` itself), compared
// case-insensitively with / and \ treated alike. Only at a folder boundary:
// C:\Users\Ann does not contain C:\Users\Anna\x.
inline bool InFolder(const std::string& path, const std::string& base) {
    if (base.size() < 3 || path.size() < base.size()) { return false; }
    for (std::size_t i = 0; i < base.size(); ++i) {
        char a = path[i], b = base[i];
        if (a == '/') { a = '\\'; }
        if (b == '/') { b = '\\'; }
        if (a >= 'A' && a <= 'Z') { a = static_cast<char>(a - 'A' + 'a'); }
        if (b >= 'A' && b <= 'Z') { b = static_cast<char>(b - 'A' + 'a'); }
        if (a != b) { return false; }
    }
    return path.size() == base.size() || path[base.size()] == '\\' ||
           path[base.size()] == '/';
}

// Replaces the user's profile folder at the start of a path with %USERPROFILE%,
// so a log pasted into a public bug report does not carry the Windows account
// name. Paths elsewhere (Program Files, other drives) are returned unchanged.
inline std::string AnonymisePath(const std::string& path, const std::string& profile) {
    if (!InFolder(path, profile)) { return path; }
    return "%USERPROFILE%" + path.substr(profile.size());
}

// A path relative to `base` when it lies inside it ("plugins\x.asi"), otherwise
// the path unchanged.
inline std::string RelativeTo(const std::string& path, const std::string& base) {
    if (!InFolder(path, base)) { return path; }
    if (path.size() == base.size()) { return "."; }
    return path.substr(base.size() + 1);
}

}  // namespace mpf
