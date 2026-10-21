// PCH.h - MagickaPoolFix
// ASI plugin for The Elder Scrolls IV: Oblivion Remastered.
// See docs/SPEC.md for the full specification.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <psapi.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define MPF_PLUGIN_NAME    "MagickaPoolFix"
#define MPF_PLUGIN_VERSION "0.9.7"

// On x64 the MS calling convention is the default for both MSVC and mingw-w64
// targeting Windows. __fastcall is a no-op on MSVC x64 and triggers an "attribute
// ignored" warning on GCC, so only emit it for MSVC.
#if defined(_MSC_VER)
#define MPF_CALL __fastcall
#else
#define MPF_CALL
#endif

namespace mpf {

// ActorValue indices, from commonlibob64 RE/A/ActorValue.h.
enum : std::int32_t {
    kAV_Strength     = 0,
    kAV_Intelligence = 1,
    kAV_Willpower    = 2,
    kAV_Agility      = 3,
    kAV_Speed        = 4,
    kAV_Endurance    = 5,
    kAV_Personality  = 6,
    kAV_Luck         = 7,
    // Health is 8 and Magicka is 9, not 8. Versions up to 0.4.0 had Magicka at 8,
    // which is Health. The correct mapping was established from the field dump of
    // 6 August 2026: with the ActorValue index applied to magicActorValueModifiers
    // at +0x474, index 11 held -99.55 (an unmistakable Feather effect on
    // Encumbrance) and indices 21 to 25 held exactly +5.0 each, landing on
    // Conjuration, Destruction, Illusion, Mysticism and Restoration -- the five
    // magic schools the character actually has fortified. That alignment fixes the
    // whole enumeration, and it puts Magicka at 9.
    kAV_Health       = 8,
    kAV_Magicka      = 9,
    kAV_Fatigue      = 10,
    kAV_Encumbrance  = 11,
};

// Default offsets inside PlayerCharacter, from commonlibob64
// include/RE/P/PlayerCharacter.h. Overridable through the [Offsets] section.
enum : std::int32_t {
    kDefaultOff_MagicAVModifiers   = 0x474,
    kDefaultOff_ScriptAVModifiers  = 0x594,
    kDefaultOff_MagickaModifier    = 0x6B8,
    kDefaultOff_GameAVModifiers    = 0x6C0,
    kDefaultSizeof_PlayerCharacter = 0xB88,

    kDefaultVtbl_Update             = 0x099,
    kDefaultVtbl_GetActorFloatValue = 0x0B1,
};

}  // namespace mpf
