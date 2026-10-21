// ModelWatch.h -- testing the maximum-Magicka model without calling the game.
//
// Background. Two field runs proved that calling the game's own accessor from a
// foreign thread is not viable on this build: 0.3.1 entered the call and never
// returned, and 0.4.0's guarded retry faulted inside game code with a write to
// address zero. So current and maximum Magicka have to come from memory.
//
// The field run of 6 August 2026 then produced a decisive negative result. Over
// twenty-four seconds of pure passive Magicka regeneration -- no spell cast, only an
// equipped +10 buff -- exactly two floats in the whole 0x400..0xB88 window moved:
// +0x06B8 and +0x08E0. If current Magicka were stored as a float anywhere in that
// window it would necessarily have moved on every frame, because the pool was
// visibly filling. It did not.
//
// [DEDUCTION] Current Magicka is not a stored float in the player object window.
// The engine keeps only the DEFICIT -- magickaModifier at +0x06B8, always <= 0 --
// and derives current = maximum + modifier on demand, with maximum computed from
// Intelligence. This is the classic Oblivion actor-value damage model, and it is
// the only explanation consistent with a filling pool that moves a single float.
//
// Why that is good news. It means the mod never needs to locate "current" or
// "maximum" at all. It needs the CHANGE in maximum, and that is fully readable:
//
//     maximum      = 2 * Intelligence + racial bonus + birthsign bonus
//     d(maximum)   = 2 * d(total Intelligence modifier) + d(total Magicka modifier)
//
// where each total is the sum of the magic, script and game modifier arrays at the
// relevant ActorValue index. Everything on the right-hand side sits at a known
// offset. The compensation the mod exists to perform then reduces to adjusting
// +0x06B8, one field, by a quantity computed from fields next to it.
//
// What this class does. Nothing but watch and report. On every transition of either
// total it logs the predicted change in maximum next to the change the game actually
// applied to the deficit, which discriminates between the three possible engine
// behaviours in a single spell cast:
//
//     d(modifier) == -d(maximum)   the pool is preserved -- nothing to fix on increase
//     d(modifier) == 0             the pool is inflated with free Magicka -- the bug
//     anything else                a scaling rule, reported verbatim for analysis
//
// A single Fortify Intelligence cast is enough to settle it. That is the entire
// remaining unknown in the project.

#pragma once

#include "PCH.h"
#include "Log.h"
#include "MagickaMath.h"

namespace mpf {

// What one transition of the modifier totals looked like. Returned by Feed() so the
// caller can hand it to the corrector; ModelWatch itself never writes anything.
struct Transition {
    bool  valid       = false;  // a real event, not a prime or a re-prime
    float deltaMax    = 0.0f;   // predicted change in maximum Magicka
    float deltaInt    = 0.0f;
    float deltaMag    = 0.0f;
    float deficitBefore = 0.0f; // deficit one tick before the change
    float deficitAfter  = 0.0f; // deficit as the engine left it
    MaxChangeBehaviour verdict = MaxChangeBehaviour::kPartial;
};

class ModelWatch {
public:
    void Configure(float eps) {
        eps_   = eps;
        primed_ = false;
        events_ = 0;
    }

    // intTotal  : magic[1] + script[1] + game[1]
    // magTotal  : magic[9] + script[9] + game[9]
    // modifier  : the float at +0x06B8
    //
    // Returns a Transition whose valid flag is set only when a real change of maximum
    // Magicka was reported, so the caller can hand it straight to the corrector.
    Transition Feed(float intTotal, float magTotal, float modifier) {
        Transition t{};
        if (!primed_) {
            Prime(intTotal, magTotal, modifier, true);
            return t;
        }

        const float dInt = intTotal - int_;
        const float dMag = magTotal - mag_;

        // Only a change in one of the two modifier totals can change maximum
        // Magicka. A moving deficit on its own is regeneration or a spell being paid
        // for, and says nothing about the model -- but its latest value has to be
        // kept, because it is the "before" state of the next real transition.
        if (std::fabs(dInt) <= eps_ && std::fabs(dMag) <= eps_) {
            mod_ = modifier;
            return t;
        }

        const float dMax = kMagickaPerIntelligence * dInt + dMag;
        const float dMod = modifier - mod_;

        // A save load rewrites the whole object at once, which looks like a fortify
        // of several hundred points arriving in a single frame. The caller signals
        // that case with Discontinuity(), but a burst can be missed if it lands
        // between two polls, so an implausible magnitude is refused outright. No
        // legitimate effect moves maximum Magicka by more than this.
        if (std::fabs(dMax) > kMaxPlausibleDMax) {
            MPF_INFO("MODELWATCH ignoring a %+.1f point jump in maximum Magicka: too "
                     "large to be a spell effect, so this is a save load or an object "
                     "reuse. Re-priming.", static_cast<double>(dMax));
            Prime(intTotal, magTotal, modifier, false);
            return t;
        }

        ++events_;
        const MaxChangeBehaviour verdict =
            ClassifyMaxChange(dMax, dMod, mod_, eps_);

        MPF_DBG("MODELWATCH event %d ---------------------------------------------",
                 events_);
        MPF_DBG("  Intelligence modifier total : %10.4f -> %10.4f  (delta %+.4f)",
                 static_cast<double>(int_), static_cast<double>(intTotal),
                 static_cast<double>(dInt));
        MPF_DBG("  Magicka modifier total      : %10.4f -> %10.4f  (delta %+.4f)",
                 static_cast<double>(mag_), static_cast<double>(magTotal),
                 static_cast<double>(dMag));
        MPF_DBG("  predicted change in maximum : %+.4f  (= 2 x %+.4f %+.4f)",
                 static_cast<double>(dMax), static_cast<double>(dInt),
                 static_cast<double>(dMag));
        MPF_DBG("  deficit at +0x%03X           : %10.4f -> %10.4f  (delta %+.4f)",
                 deficitOffset_, static_cast<double>(mod_),
                 static_cast<double>(modifier), static_cast<double>(dMod));
        // A self-cast fortification pays for itself in the same transition that raises
        // maximum, so the raw deficit delta mixes the spell cost with any compensation
        // the engine applied. One equation, two unknowns -- but the unknowns are not
        // free. Writing c for what the engine compensated and k for what the spell cost,
        //
        //     deltaDeficit = -c - k     hence    k = -deltaDeficit - c
        //
        // and k cannot be negative, because no spell refunds Magicka. Printing k under
        // each of the two candidate values of c therefore turns an underdetermined
        // transition into a decidable one whenever exactly one of them is non-negative.
        //
        // Worked example, measured 6 Aug 2026 17:58:23: deltaDeficit -270 with a
        // predicted change in maximum of +300 gives 270 if the engine compensated
        // nothing, and -30 if it compensated in full. The second is impossible, so this
        // build does not compensate -- established from the ambiguous transition itself.
        MPF_DBG("  deficit if preserved        : %10.4f  (observed %10.4f)",
                 static_cast<double>(mod_ - dMax), static_cast<double>(modifier));
        MPF_DBG("  implied spell cost if the engine compensated nothing : %10.4f",
                 static_cast<double>(-dMod));
        MPF_DBG("  implied spell cost if the engine compensated in full : %10.4f%s",
                 static_cast<double>(-dMod - dMax),
                 (-dMod - dMax) < -eps_ ? "  <-- negative, impossible, so the engine "
                                          "did NOT compensate" : "");
        MPF_DBG("  pool before the change      : %s",
                 std::fabs(mod_) <= eps_ ? "FULL (deficit zero)" : "NOT full");
        MPF_DBG("  VERDICT: %s", Describe(verdict, dMax > 0.0f));

        t.valid         = true;
        t.deltaMax      = dMax;
        t.deltaInt      = dInt;
        t.deltaMag      = dMag;
        t.deficitBefore = mod_;
        t.deficitAfter  = modifier;
        t.verdict       = verdict;

        int_ = intTotal; mag_ = magTotal; mod_ = modifier;
        return t;
    }

    // Called after the corrector writes the deficit. Without this the next tick would
    // see the value we wrote as an unexplained movement and carry a stale "before" into
    // the following real transition.
    void NoteDeficitWrite(float written) { mod_ = written; }

    // Called by the observation loop when the player object was rescanned, or when a
    // single poll changed so many floats at once that the object cannot have gone
    // through a continuous transition. Re-priming after such a break is what stops a
    // save load being reported as a fortify of several hundred points.
    void Discontinuity(const char* why) {
        if (!primed_) { return; }
        primed_ = false;
        MPF_DBG("MODELWATCH re-priming after a discontinuity (%s)", why);
    }

    int Events() const { return events_; }

    // The classification itself lives in MagickaMath.h, which builds and is unit
    // tested off Windows; this only turns a verdict into prose.
    static const char* Describe(MaxChangeBehaviour v, bool increase) {
        switch (v) {
        case MaxChangeBehaviour::kPreserved:
            return "pool PRESERVED -- the deficit moved by exactly minus the change "
                   "in maximum, so current Magicka did not move. Correct on an "
                   "increase; nothing to do here.";
        case MaxChangeBehaviour::kSpilledFromFull:
            return "OVERFLOW SPILLED -- maximum fell, the pool was already full, and "
                   "the deficit stayed at zero, so the pool is still full at the new "
                   "maximum. That is exactly the water above the new rim, so this is "
                   "correct and needs no fix.";
        case MaxChangeBehaviour::kFreeMagicka:
            return "FREE MAGICKA -- maximum rose and the deficit did not move, so "
                   "current Magicka rose by the whole increase without being paid "
                   "for. This is the bug; the fix subtracts the change in maximum "
                   "from the deficit.";
        case MaxChangeBehaviour::kLostMagicka:
            return "MAGICKA STOLEN -- maximum fell, the pool was NOT full, and the "
                   "deficit did not move, so current Magicka fell by the whole "
                   "decrease although there was no overflow to spill. This is the "
                   "bug, and it is the half that drives the pool negative when a "
                   "fortify expires.";
        case MaxChangeBehaviour::kPartial:
            if (increase) {
                return "PARTIAL -- the deficit moved, but by neither zero nor minus the "
                       "change in maximum. On an increase this is the ordinary shape "
                       "when you cast the fortification on yourself: the engine charges "
                       "the spell in the same transition that raises your maximum, so "
                       "the movement mixes the two. The 'implied spell cost' lines above "
                       "separate them: a negative cost is impossible, since no spell "
                       "refunds Magicka, so whichever hypothesis produces one is refuted.";
            }
            return "PARTIAL -- the deficit moved, but by neither zero nor minus the "
                   "change in maximum. On a decrease this is a spell cast in the same "
                   "poll (a recast, docs/SPEC.md section 5.5), regeneration over a "
                   "small change, or another effect landing at once. The decision "
                   "below does not depend on the verdict.";
        }
        return "?";
    }

    void SetDeficitOffset(int off) { deficitOffset_ = off; }

private:
    void Prime(float intTotal, float magTotal, float modifier, bool announce) {
        int_ = intTotal; mag_ = magTotal; mod_ = modifier;
        primed_ = true;
        if (!announce) { return; }
        MPF_DBG("MODELWATCH primed: Intelligence modifier total %.4f, Magicka "
                 "modifier total %.4f, deficit %.4f (pool %s). Cast a Fortify "
                 "Intelligence or Fortify Magicka spell to test the model.",
                 static_cast<double>(int_), static_cast<double>(mag_),
                 static_cast<double>(mod_),
                 std::fabs(mod_) <= eps_ ? "full" : "not full");
    }

    // No legitimate effect moves maximum Magicka by more than this. The largest
    // transitions measured are +/-300 points, one spell combining Fortify
    // Intelligence 100 and Fortify Magicka 100; several effects ending in the same
    // poll add up, hence the margin. A whole object being rewritten by a save load is
    // in the thousands. docs/SPEC.md section 6.4.
    static constexpr float kMaxPlausibleDMax = 2000.0f;

    int   deficitOffset_ = 0x6B8;
    float eps_ = 0.01f;
    float int_ = 0.0f;
    float mag_ = 0.0f;
    float mod_ = 0.0f;
    bool  primed_ = false;
    int   events_ = 0;
};

}  // namespace mpf
