// MagickaMath.h - compensation core, deliberately free of any Windows or game
// dependency so that the "glass of water" semantics can be unit-tested outside
// the game.
//
// Engine model (deduced from the reverse-engineered commonlibob64 headers,
// RE/P/PlayerCharacter.h: magicActorValueModifiers[72] @0x474,
// scriptActorValueModifiers[72] @0x594, magickaModifier @0x6B8,
// gameActorValueModifiers[72] @0x6C0):
//
//   magicka_max     = f(effective Intelligence) + race/birthsign bonus
//                     + modifiers[Magicka]
//   magicka_current = magicka_max + magickaModifier   (magickaModifier <= 0,
//                                                      i.e. accumulated damage)
//
// Storing the pool as a damage offset rather than an absolute value is what
// produces the two behaviours this plugin targets:
//   - Fortify cast: max rises, offset unchanged -> current jumps for free.
//   - Buff expiry:  max falls, offset unchanged -> current can go below zero.
//
// Fix: whenever the maximum changes by delta, compensate the offset by -delta so
// the current pool stays put, then clamp the current pool into [0, max].

#pragma once

#include <cmath>

namespace mpf {

struct CompensationConfig
{
    // Keep the current pool constant when the maximum INCREASES
    // (removes the free Magicka granted on buff activation).
    bool preserveOnIncrease = true;

    // Keep the current pool constant when the maximum DECREASES
    // (removes the abrupt -X applied on buff expiry).
    bool preserveOnDecrease = true;

    // Never let the current pool fall below zero: a glass cannot hold a
    // negative amount of water.
    bool clampNegative = true;

    // Spill the overflow when the new maximum ends up below the current pool.
    bool clampOverflow = true;

    // Float noise threshold.
    float epsilon = 0.01f;

    // Safety rail: beyond this, assume a save load or character change rather
    // than a spell, and resynchronise the baseline instead of compensating.
    float maxAbsDelta = 1.0e6f;
};

struct CompensationResult
{
    bool  apply       = false;  // should newModifier be written to memory?
    bool  resync      = false;  // reset the baseline without compensating
    float newModifier = 0.0f;
    float newCurrent  = 0.0f;
    float delta       = 0.0f;   // observed change of the maximum
};

// lastMax  : maximum Magicka observed on the previous frame
// newMax   : maximum Magicka observed now
// modifier : current value of PlayerCharacter::magickaModifier
inline CompensationResult ComputeCompensation(
    float lastMax, float newMax, float modifier, const CompensationConfig& cfg)
{
    CompensationResult r{};

    if (!std::isfinite(lastMax) || !std::isfinite(newMax) || !std::isfinite(modifier)) {
        r.resync = true;
        return r;
    }

    r.delta = newMax - lastMax;

    if (std::fabs(r.delta) > cfg.maxAbsDelta) {
        r.resync = true;
        return r;
    }

    float mod = modifier;

    const bool increased = r.delta > cfg.epsilon;
    const bool decreased = r.delta < -cfg.epsilon;

    if ((increased && cfg.preserveOnIncrease) || (decreased && cfg.preserveOnDecrease)) {
        // current = max + mod; keep current constant despite delta.
        mod -= r.delta;
    }

    // Clamp current into [0, newMax]  <=>  clamp mod into [-newMax, 0].
    if (cfg.clampOverflow && mod > 0.0f) {
        mod = 0.0f;
    }
    if (cfg.clampNegative && newMax > 0.0f && mod < -newMax) {
        mod = -newMax;
    }

    r.newModifier = mod;
    r.newCurrent  = newMax + mod;
    r.apply       = std::fabs(mod - modifier) > cfg.epsilon;
    return r;
}

// ---------------------------------------------------------------------------
// Classifying what the engine does to the deficit when maximum Magicka moves.
//
// The field run of 6 August 2026 established that current Magicka is not stored: the
// engine keeps only the deficit (magickaModifier, always <= 0) and derives
// current = maximum + deficit on demand. So the question "does the pool gain or lose
// Magicka for free when maximum moves" reduces to observing how the deficit responds
// to a known change in maximum, which needs no hook and no write.
//
// The direction of the change and the state of the pool BEFORE it both matter, and
// version 0.5.1 got this wrong: it judged the deficit response alone and reported a
// perfectly correct spill from a full pool as if it were the bug. The four outcomes
// below are the ones that actually have distinct meanings.
enum class MaxChangeBehaviour {
    // The deficit moved by exactly minus the change in maximum, so current Magicka
    // did not move at all. This is the desired behaviour on an INCREASE: a bigger
    // glass holds the same water.
    kPreserved,

    // Maximum fell, the pool was already full, and the deficit stayed at zero, so
    // current fell with maximum and the pool is still full. This is also correct:
    // the water above the new rim is exactly the water that should spill.
    kSpilledFromFull,

    // Maximum rose and the deficit did not move, so current rose by the whole
    // increase without the player paying for it. Free Magicka -- the bug.
    kFreeMagicka,

    // Maximum fell, the pool was NOT full, and the deficit did not move, so current
    // fell by the whole decrease even though there was no overflow to spill. Stolen
    // Magicka -- also the bug, and the more damaging half of it, because it is what
    // drives the pool negative when a fortify expires.
    kLostMagicka,

    // Anything else. Do not guess a rule from a single observation.
    kPartial
};

// Multiplier from a point of Intelligence to a point of maximum Magicka. Base
// Magicka is Intelligence times two plus flat race and birthsign bonuses, so the
// derivative with respect to Intelligence is exactly 2 and the flat bonuses cancel
// out of any difference. https://en.uesp.net/wiki/Oblivion:Magicka
inline constexpr float kMagickaPerIntelligence = 2.0f;

// dMax          change in maximum Magicka, computed from the modifier arrays
// dMod          change the engine applied to the deficit over the same transition
// modBefore     the deficit BEFORE the transition; zero means the pool was full
//
// The tolerance scales with the magnitude being judged. A Fortify Intelligence 110
// shifts maximum by 220 points, and float32 rounding plus a one-frame sampling skew
// make a fixed hundredth-of-a-point window useless at that scale: it would classify
// every large fortify as kPartial.
inline MaxChangeBehaviour ClassifyMaxChange(float dMax, float dMod, float modBefore,
                                           float eps)
{
    const float tol      = eps + 0.02f * std::fabs(dMax);
    const bool  followed = std::fabs(dMod + dMax) <= tol;   // current unchanged
    const bool  frozen   = std::fabs(dMod) <= tol;          // current moved by dMax

    // Checked first, so a transition that changes nothing at all is reported as
    // preserved rather than as a spill or a theft of zero points.
    if (followed) { return MaxChangeBehaviour::kPreserved; }

    if (frozen) {
        if (dMax > 0.0f) { return MaxChangeBehaviour::kFreeMagicka; }
        // Maximum fell. Whether that was correct depends entirely on whether there
        // was overflow to spill, which is exactly what a zero deficit tells us.
        const bool wasFull = std::fabs(modBefore) <= eps;
        return wasFull ? MaxChangeBehaviour::kSpilledFromFull
                       : MaxChangeBehaviour::kLostMagicka;
    }
    return MaxChangeBehaviour::kPartial;
}

}  // namespace mpf
