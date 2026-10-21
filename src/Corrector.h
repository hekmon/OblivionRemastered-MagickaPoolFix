// Corrector.h -- deciding whether to touch the deficit, and to what value.
//
// This header is pure arithmetic on floats. It has no Windows dependency and no memory
// access, so it compiles and runs on the host and is covered by the unit tests. The
// actual write lives in Plugin.cpp; everything difficult about the decision lives here.
//
// ---------------------------------------------------------------------------
// The problem this file exists to solve
//
// The correction is relative: current Magicka is not stored, only the deficit is, and
// the maximum is not readable at all, so the only rule available is
//
//     newDeficit = min(deficit - deltaMax, 0)
//
// That rule is correct if and only if the engine did NOT already adjust the deficit
// itself. Applying it on top of an engine that already compensates would subtract the
// change twice and steal Magicka from the player. So the mod has to know which of the
// two the engine does.
//
// The obvious test -- look at how the deficit moved across this one transition -- fails
// on the single most common case. A self-cast fortification is charged in the same
// frame that raises the maximum, so the observed movement of the deficit is
//
//     deltaDeficit = -(spell cost) + (whatever the engine compensated)
//
// and one equation cannot separate two unknowns. Measured example: deficit 0 ->
// -270.0000 with a predicted change in maximum of +300. That is consistent with "no
// compensation, spell cost 270" and, arithmetically, with other splits too.
//
// ---------------------------------------------------------------------------
// The solution: calibrate on unambiguous transitions, then act on all of them
//
// The engine's policy is a property of the build, not of the moment, so it does not
// have to be decided from the transition being corrected. Some transitions are
// unambiguous, because nothing was being paid for while they happened:
//
//   - a fortification EXPIRING costs nothing;
//   - equipping or unequipping an enchanted item costs nothing;
//   - drinking a potion costs nothing.
//
// On those, the deficit movement is pure engine behaviour. PolicyTracker watches for
// them, decides the policy once, and DecideCorrection then applies it to every
// transition including the ambiguous ones.
//
// ---------------------------------------------------------------------------
// Why this is not the shipped default
//
// Auto-calibration exists, but iEnginePolicy ships at 1: assume the engine ignores the
// change and correct from the first frame. The measurements are already in, on
// OblivionRemastered-Win64-Shipping.exe with PE timestamp 0xF19077A4, and making every
// player re-derive them means the mod does nothing at all until their first buff expires
// -- including on the self-cast fortification that is the commonest way to meet the bug.
// A mod that works immediately for everybody is worth more than a mod that proves the
// bug to itself first.
//
// What is kept is the observation, which runs whatever the policy is set to. Pinning the
// policy decides what the mod DOES; it does not stop it looking. If an official patch
// ever rebases the deficit itself, the evidence will contradict the pinned value and the
// mod says so in the log, loudly and by name, instead of quietly subtracting the change
// twice. That warning costs nothing and covers the only case where this mod can do harm
// rather than merely be redundant.
//
// With iEnginePolicy = 0 the mod corrects only unambiguous transitions until the policy
// settles, and abstains on the rest.

#pragma once

#include <cmath>

#include "MagickaMath.h"

namespace mpf {

enum class EnginePolicy : int {
    // No unambiguous transition seen yet. Correct only what is unambiguous.
    kUnknown = 0,

    // The engine leaves the deficit alone when the maximum moves, so current Magicka
    // moves with the maximum. This is the buggy behaviour, and the one measured on
    // OblivionRemastered-Win64-Shipping.exe with PE timestamp 0xF19077A4. The
    // correction applies.
    kEngineIgnores = 1,

    // The engine already keeps current Magicka where it was. Nothing to correct, and
    // correcting anyway would take Magicka away from the player.
    kEngineHandles = 2
};

// The value the deficit ought to hold after a change of deltaMax, given what it held
// before. This is the rule from the specification, and the only place it is written.
//
//   increase                 -> deficit deepens by the increase, current is unchanged
//   decrease, no overflow    -> deficit shallows by the decrease, current is unchanged
//   decrease, with overflow  -> clamped to zero, pool full at the new maximum
//
// No floor is applied. With a valid starting state the result cannot make current
// Magicka negative: from deficit >= -max it follows that deficit - deltaMax >= -newMax.
// A floor would only ever act on a save that is already corrupt, and it cannot be
// implemented anyway without the absolute maximum, which is not readable.
inline float CorrectDeficit(float deficitBefore, float deltaMax, bool spillOverflow)
{
    const float target = deficitBefore - deltaMax;
    if (target > 0.0f) { return spillOverflow ? 0.0f : target; }
    return target;
}

// ---------------------------------------------------------------------------
// Recasting a spell that is still active: the give-back window.
//
// The game does not always swap an active fortification for its recast in one step.
// Measured on 21 October 2026 (docs/SPEC.md section 5.5), the maximum fell and rose
// again 17 ms, 115 ms and 680 ms apart. The 17 ms one is certainly a recast: the spell
// cost came with the fall, and an expiry costs nothing. The other two carry the cost
// with the rise, so they are either a recast in two steps or an expiry landing just
// before a cast already under way; the log cannot tell them apart, and to the player
// both are the same act, keeping the buff up. In every case the fall looks like an
// expiry, and the rule spills whatever sits above the unbuffed maximum. On a full pool
// that is the whole fortification: a recast that costs 278 in the game would cost 578
// with the fix, depending only on timing.
//
// So the spill is remembered for a short time, and an increase that follows within
// that time gives it back, up to the size of the increase. A recast then ends where a
// one-step recast ends: the spell cost paid, nothing else.
//
// The window is a JUDGEMENT, not a measurement. Measured: the three gaps above, and the
// fastest item swap by hand, 700 ms (28 August 2026). The two ways of missing are not
// equal. Too short, and a recast loses up to the whole fortification. Too long, and a
// quick swap by hand gets back the overflow it would otherwise lose, which only makes
// the swap neutral: the give-back never exceeds what was spilled, so it cannot create
// Magicka. 1000 ms covers every gap measured and leans toward the harmless side. Every
// rise within kRecastWatchMs of a spill is logged (RECAST WINDOW), inside the window or
// not, so the choice can be checked against more play.
inline constexpr unsigned long long kRecastWindowMs = 1000;

// How long after a spill a rise is still logged, to see what the window misses.
inline constexpr unsigned long long kRecastWatchMs = 5000;

// What the clamp throws away on a decrease: the part of the pool above the new
// maximum. Zero when nothing overflows, on an increase, or when the overflow is kept.
inline float SpilledOnDecrease(float deltaMax, float deficitAfter, bool spillOverflow)
{
    if (!(deltaMax < 0.0f) || !spillOverflow) { return 0.0f; }
    const float over = deficitAfter - deltaMax;
    return over > 0.0f ? over : 0.0f;
}

// How much of a recent spill an increase gives back.
inline float RecastGiveBack(float spilled, unsigned long long msSinceSpill, float deltaMax)
{
    if (!(spilled > 0.0f) || !(deltaMax > 0.0f) || msSinceSpill > kRecastWindowMs) {
        return 0.0f;
    }
    return spilled < deltaMax ? spilled : deltaMax;
}

// Tolerance for every comparison in this file. The proportional term is required: a
// Fortify Intelligence 110 moves the maximum by 220 points, and a fixed hundredth of a
// point window would classify every large fortification as unexpected and leave the mod
// permanently idle. The constant term keeps a two-point change judged tightly.
inline float Tolerance(float deltaMax, float eps)
{
    return eps + 0.02f * std::fabs(deltaMax);
}

// ---------------------------------------------------------------------------
// The engine moving the deficit on a decrease.
//
// The relative rule assumes the engine leaves the deficit alone when the maximum
// moves (§4 of docs/SPEC.md). For the original game, UESP describes a floor at zero
// when a Fortify Magicka ends or a Drain Magicka lands. If the Remastered engine
// applies one, it raises the deficit itself on that decrease, and the correction,
// computed from the deficit the engine left, grants the difference a second time
// (docs/SPEC.md section 10.5). Not observed so far; this makes it impossible to miss.
//
// Regeneration also raises the deficit between two polls, so the threshold sits above
// what it can explain: 38 points per second at Willpower 200 (docs/SPEC.md §10.7)
// over a poll stretched to 50 ms is under 2 points. A restore effect landing in the
// same poll would also trip it, which the log line says.
inline constexpr float kEngineMoveFloor = 2.0f;

// Returns the amount the engine raised the deficit by on a decrease, or 0 when the
// transition is not a decrease or the movement stays within what regeneration explains.
inline float EngineRaisedDeficitOnDecrease(float deltaMax, float deficitBefore,
                                           float deficitAfter, float eps)
{
    if (!(deltaMax < 0.0f)) { return 0.0f; }
    const float moved = deficitAfter - deficitBefore;
    return moved > kEngineMoveFloor + Tolerance(deltaMax, eps) ? moved : 0.0f;
}

// ---------------------------------------------------------------------------
// Policy calibration.
class PolicyTracker {
public:
    void Configure(float eps, EnginePolicy forced, int confirmationsNeeded) {
        eps_    = eps > 0.0f ? eps : 0.01f;
        forced_ = forced;
        needed_ = confirmationsNeeded > 0 ? confirmationsNeeded : 1;
        ignores_ = 0;
        handles_ = 0;
    }

    // Reset the counters but not the forced setting. Called on a save load, because a
    // different save could in principle be running under a different game build.
    void Reset() { ignores_ = 0; handles_ = 0; }

    EnginePolicy Policy() const {
        if (forced_ != EnginePolicy::kUnknown) { return forced_; }
        return Observed();
    }

    // What the evidence alone says, ignoring any pinned value. Kept separate so a pinned
    // policy can be checked against reality rather than replacing it.
    EnginePolicy Observed() const {
        if (ignores_ >= needed_ && handles_ == 0) { return EnginePolicy::kEngineIgnores; }
        if (handles_ >= needed_ && ignores_ == 0) { return EnginePolicy::kEngineHandles; }
        // Contradictory evidence is treated as "still unknown" on purpose. Two
        // transitions that disagree mean the model is wrong somewhere, and the safe
        // response to a wrong model is to stop correcting rather than pick a side.
        return EnginePolicy::kUnknown;
    }

    // The evidence disagrees with itself. Two transitions that contradict each other
    // mean the model is wrong somewhere, and the safe response to a wrong model is to
    // stop correcting rather than pick a side.
    bool Contradictory() const { return ignores_ > 0 && handles_ > 0; }

    // The evidence disagrees with the value pinned in the .conf. This is the case a
    // future game patch would produce, and the reason observation keeps running even
    // when the policy is not being derived from it.
    bool ContradictsPinned() const {
        if (forced_ == EnginePolicy::kUnknown) { return false; }
        if (forced_ == EnginePolicy::kEngineIgnores) { return handles_ > 0; }
        return ignores_ > 0;
    }

    bool Pinned() const { return forced_ != EnginePolicy::kUnknown; }
    int  IgnoreEvidence() const { return ignores_; }
    int  HandleEvidence() const { return handles_; }

    // What a single transition tells us, if anything. Returns true when this
    // observation was informative, so the caller can log the moment the policy is
    // settled instead of on every event.
    //
    // deficitBefore   deficit one tick before the modifier totals moved
    // deltaMax        predicted change in maximum, from the modifier totals
    // deficitAfter    deficit observed on the tick the modifier totals moved
    bool Observe(float deficitBefore, float deltaMax, float deficitAfter) {
        // Deliberately NOT skipped when the policy is pinned. See the header comment:
        // pinning decides what the mod does, not whether it keeps checking.
        if (std::fabs(deltaMax) <= eps_) { return false; }

        const float tol     = Tolerance(deltaMax, eps_);
        const float correct = CorrectDeficit(deficitBefore, deltaMax, true);

        // A transition where doing nothing happens to BE the correct behaviour teaches
        // nothing about the engine. This is not a corner case: it is exactly what a
        // fortification expiring on a full pool looks like, where the only right answer
        // is to leave the deficit at zero and let the overflow go. Both a correct engine
        // and a negligent one produce an unchanged deficit there.
        if (std::fabs(correct - deficitBefore) <= tol) { return false; }

        const bool unchanged = std::fabs(deficitAfter - deficitBefore) <= tol;
        const bool corrected = std::fabs(deficitAfter - correct) <= tol;

        if (unchanged) { ++ignores_; return true; }
        if (corrected) { ++handles_; return true; }

        // Neither: something was paid for during the same transition, or the model is
        // incomplete. Not informative, and deliberately not counted as evidence for
        // either side.
        return false;
    }

private:
    float        eps_     = 0.01f;
    EnginePolicy forced_  = EnginePolicy::kUnknown;
    int          needed_  = 1;
    int          ignores_ = 0;
    int          handles_ = 0;
};

// ---------------------------------------------------------------------------
// The decision for one transition.
enum class DecisionReason : int {
    // Acting.
    kApply,

    // Not acting, and each of these is a distinct thing to tell the player.
    kInactive,             // the preset is Off, or the mod is not allowed to write
    kNoChange,             // the maximum did not really move
    kImplausible,          // change too large to be an effect: save load or bad offsets
    kDirectionDisabled,    // this direction is switched off in the configuration
    kAlreadyCorrect,       // the deficit already holds the right value
    kEngineHandlesIt,      // the engine preserves the pool on its own
    kNeedsCalibration      // ambiguous transition and the policy is not settled yet
};

struct Decision {
    bool           act         = false;
    float          newDeficit  = 0.0f;
    float          deltaApplied = 0.0f;
    DecisionReason reason      = DecisionReason::kNoChange;
};

// deltaMax        predicted change in maximum for this transition
// deficitBefore   deficit one tick before the change
// deficitAfter    deficit as it stands now, after the engine has had its say. This is
//                 the value the correction is computed FROM, so that anything the
//                 player paid for in the same tick is preserved rather than refunded.
// giveBack        what RecastGiveBack() allows on this transition, 0 otherwise. Used
//                 only on an increase that the rule corrects.
inline Decision DecideCorrection(float deltaMax, float deficitBefore, float deficitAfter,
                                 EnginePolicy policy, const CompensationConfig& cfg,
                                 bool allowedToWrite, float giveBack = 0.0f)
{
    Decision d{};
    const float eps = cfg.epsilon > 0.0f ? cfg.epsilon : 0.01f;

    if (!std::isfinite(deltaMax) || !std::isfinite(deficitBefore) ||
        !std::isfinite(deficitAfter)) {
        d.reason = DecisionReason::kImplausible;
        return d;
    }
    if (std::fabs(deltaMax) > cfg.maxAbsDelta) {
        d.reason = DecisionReason::kImplausible;
        return d;
    }
    if (std::fabs(deltaMax) <= eps) {
        d.reason = DecisionReason::kNoChange;
        return d;
    }

    const bool increased = deltaMax > eps;
    if ((increased && !cfg.preserveOnIncrease) || (!increased && !cfg.preserveOnDecrease)) {
        d.reason = DecisionReason::kDirectionDisabled;
        return d;
    }

    if (policy == EnginePolicy::kEngineHandles) {
        d.reason = DecisionReason::kEngineHandlesIt;
        return d;
    }

    const float tol       = Tolerance(deltaMax, eps);
    const bool  unchanged = std::fabs(deficitAfter - deficitBefore) <= tol;

    // An unchanged deficit is unambiguous: nothing was paid for, so whatever the engine
    // did or did not do is visible directly and the policy is not needed. A deficit
    // that moved is ambiguous, because the movement mixes a possible spell cost with a
    // possible engine compensation, and only the calibrated policy can resolve it.
    if (!unchanged && policy != EnginePolicy::kEngineIgnores) {
        d.reason = DecisionReason::kNeedsCalibration;
        return d;
    }

    // Computed from the deficit as it stands NOW, not from the one before. That is what
    // keeps a spell cast in the same tick charged to the player.
    float target = CorrectDeficit(deficitAfter, deltaMax, cfg.clampOverflow);
    if (increased && giveBack > 0.0f) {
        // Never above the deficit the engine left: the give-back returns spilled
        // Magicka, it never adds to what the increase itself would have granted.
        target += giveBack;
        if (target > deficitAfter) { target = deficitAfter; }
    }

    if (std::fabs(target - deficitAfter) <= eps) {
        // The commonest instance is a fortification expiring on a full pool: the target
        // is a positive deficit, clamped back to the zero it already holds. The pool is
        // full at the new maximum, which is what should happen, so there is no bug here
        // to fix and no write to make.
        d.reason = DecisionReason::kAlreadyCorrect;
        return d;
    }

    d.newDeficit   = target;
    d.deltaApplied = target - deficitAfter;
    d.reason       = DecisionReason::kApply;
    d.act          = allowedToWrite;
    if (!allowedToWrite) {
        // The arithmetic is still reported so a dry run shows exactly what would have
        // been written, but the reason stays kApply rather than kInactive so the caller
        // can tell "would have acted" from "had nothing to do".
    }
    return d;
}

inline const char* PolicyName(EnginePolicy p)
{
    switch (p) {
    case EnginePolicy::kUnknown:       return "unknown";
    case EnginePolicy::kEngineIgnores: return "engine ignores the change (correction applies)";
    case EnginePolicy::kEngineHandles: return "engine preserves the pool itself (nothing to do)";
    }
    return "?";
}

inline const char* ReasonText(DecisionReason r)
{
    switch (r) {
    case DecisionReason::kApply:
        return "correcting the deficit";
    case DecisionReason::kInactive:
        return "no action: the mod is inactive (iPreset = 0)";
    case DecisionReason::kNoChange:
        return "no action: the maximum did not really move";
    case DecisionReason::kImplausible:
        return "no action: the change is too large to be a spell effect, so this is a "
               "save load or the memory offsets are wrong for this game build";
    case DecisionReason::kDirectionDisabled:
        return "no action: this direction of change is switched off in the configuration";
    case DecisionReason::kAlreadyCorrect:
        return "no action needed: the deficit already holds the right value. On a "
               "fortification expiring while the pool was full this is the expected "
               "outcome -- the pool stays full at the new maximum and the overflow is "
               "correctly lost";
    case DecisionReason::kEngineHandlesIt:
        return "no action: this game build preserves the pool on its own, so correcting "
               "would take Magicka away from you";
    case DecisionReason::kNeedsCalibration:
        return "no action yet: something was paid for during this same transition, so "
               "the deficit movement cannot be attributed, and no unambiguous "
               "transition has been seen yet to settle how this game build behaves. Let "
               "one fortification expire, or equip and unequip an enchanted item, and "
               "the mod will calibrate itself and correct this case from then on";
    }
    return "?";
}

}  // namespace mpf
