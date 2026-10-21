// Tests for the decision layer: engine-policy calibration and the write decision.
//   g++ -std=c++20 -I../src test_corrector.cpp -o test_corrector && ./test_corrector
//
// The three "field" cases at the end replay measurements taken in game, so a regression
// in the decision logic is caught against reality rather than against invented numbers.

#include "Corrector.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_fail = 0;

static void Check(const std::string& name, float got, float want)
{
    const bool ok = std::fabs(got - want) < 0.001f;
    if (!ok) ++g_fail;
    std::printf("%-66s got=%9.2f want=%9.2f  %s\n",
                name.c_str(), got, want, ok ? "OK" : "FAIL");
}

static const char* ReasonName(mpf::DecisionReason r)
{
    switch (r) {
    case mpf::DecisionReason::kApply:             return "Apply";
    case mpf::DecisionReason::kInactive:          return "Inactive";
    case mpf::DecisionReason::kNoChange:          return "NoChange";
    case mpf::DecisionReason::kImplausible:       return "Implausible";
    case mpf::DecisionReason::kDirectionDisabled: return "DirectionDisabled";
    case mpf::DecisionReason::kAlreadyCorrect:    return "AlreadyCorrect";
    case mpf::DecisionReason::kEngineHandlesIt:   return "EngineHandlesIt";
    case mpf::DecisionReason::kNeedsCalibration:  return "NeedsCalibration";
    }
    return "?";
}

static const char* PolicyShort(mpf::EnginePolicy p)
{
    switch (p) {
    case mpf::EnginePolicy::kUnknown:       return "unknown";
    case mpf::EnginePolicy::kEngineIgnores: return "ignores";
    case mpf::EnginePolicy::kEngineHandles: return "handles";
    }
    return "?";
}

static void CheckReason(const std::string& name, mpf::DecisionReason got,
                        mpf::DecisionReason want)
{
    const bool ok = got == want;
    if (!ok) ++g_fail;
    std::printf("%-66s got=%-18s want=%-18s %s\n", name.c_str(), ReasonName(got),
                ReasonName(want), ok ? "OK" : "FAIL");
}

static void CheckPolicy(const std::string& name, mpf::EnginePolicy got,
                        mpf::EnginePolicy want)
{
    const bool ok = got == want;
    if (!ok) ++g_fail;
    std::printf("%-66s got=%-18s want=%-18s %s\n", name.c_str(), PolicyShort(got),
                PolicyShort(want), ok ? "OK" : "FAIL");
}

static mpf::CompensationConfig Glass()
{
    mpf::CompensationConfig c;
    c.preserveOnIncrease = true;
    c.preserveOnDecrease = true;
    c.clampOverflow      = true;
    c.clampNegative      = true;
    c.epsilon            = 0.01f;
    c.maxAbsDelta        = 100000.0f;
    return c;
}

static mpf::PolicyTracker Tracker(mpf::EnginePolicy forced = mpf::EnginePolicy::kUnknown,
                                  int needed = 1)
{
    mpf::PolicyTracker t;
    t.Configure(0.01f, forced, needed);
    return t;
}

// ---------------------------------------------------------------------------
static void TestCorrectDeficit()
{
    std::puts("\n-- the target deficit ---------------------------------------------");

    // Increase: the deficit deepens by exactly the increase, so current is unchanged.
    Check("max +300 from a pool 50 short -> deficit",
          mpf::CorrectDeficit(-50.0f, +300.0f, true), -350.0f);

    // Decrease with room to absorb it: the deficit shallows, current is unchanged.
    Check("max -300 from a pool 568 short -> deficit",
          mpf::CorrectDeficit(-567.769f, -300.0f, true), -267.769f);

    // Decrease from a full pool: the target would be a positive deficit, which is not a
    // state the field can hold. Clamped to zero: full at the new maximum, overflow lost.
    Check("max -20 from a full pool, spill on -> deficit",
          mpf::CorrectDeficit(0.0f, -20.0f, true), 0.0f);

    // With spilling off the overflow is kept as a positive deficit, i.e. current above
    // maximum. Only reachable through iPreset = 3.
    Check("max -20 from a full pool, spill off -> deficit",
          mpf::CorrectDeficit(0.0f, -20.0f, false), +20.0f);

    // Partial overflow: 10 short, maximum drops 20, so 10 points of the drop is absorbed
    // by the existing deficit and the remaining 10 overflow away.
    Check("max -20 from a pool 10 short, spill on -> deficit",
          mpf::CorrectDeficit(-10.0f, -20.0f, true), 0.0f);

    // Round trip: cast a fortification, let it expire, end where you started.
    const float up   = mpf::CorrectDeficit(-100.0f, +300.0f, true);
    const float down = mpf::CorrectDeficit(up, -300.0f, true);
    Check("round trip +300 then -300 from a pool 100 short", down, -100.0f);
}

// ---------------------------------------------------------------------------
static void TestCalibration()
{
    std::puts("\n-- engine-policy calibration ---------------------------------------");

    CheckPolicy("no evidence yet", Tracker().Policy(), mpf::EnginePolicy::kUnknown);

    // An expiry that costs nothing, on a pool that was not full, and the deficit did not
    // move. Doing nothing was wrong here, so this is real evidence.
    {
        auto t = Tracker();
        const bool informative = t.Observe(-567.769f, -300.0f, -567.769f);
        Check("frozen deficit on a partial pool is informative",
              informative ? 1.0f : 0.0f, 1.0f);
        CheckPolicy("  -> policy", t.Policy(), mpf::EnginePolicy::kEngineIgnores);
    }

    // Same shape, but the engine moved the deficit to exactly the right value.
    {
        auto t = Tracker();
        t.Observe(-567.769f, -300.0f, -267.769f);
        CheckPolicy("engine landed on the correct deficit", t.Policy(),
                    mpf::EnginePolicy::kEngineHandles);
    }

    // A decrease from a full pool teaches nothing: leaving the deficit at zero is what
    // BOTH a correct engine and a negligent one do, because the correct target is zero
    // too. Counting this as evidence would be the subtle bug in this whole scheme.
    {
        auto t = Tracker();
        const bool informative = t.Observe(0.0f, -20.0f, 0.0f);
        Check("frozen deficit on a FULL pool is not informative",
              informative ? 1.0f : 0.0f, 0.0f);
        CheckPolicy("  -> policy stays", t.Policy(), mpf::EnginePolicy::kUnknown);
    }

    // A self-cast fortification: the deficit moved, but by the spell cost, not by the
    // change in maximum. Cannot be attributed, must not be counted.
    {
        auto t = Tracker();
        const bool informative = t.Observe(0.0f, +300.0f, -270.0f);
        Check("self-cast fortification is not informative",
              informative ? 1.0f : 0.0f, 0.0f);
        CheckPolicy("  -> policy stays", t.Policy(), mpf::EnginePolicy::kUnknown);
    }

    // Contradiction is reported as unknown rather than resolved by majority.
    {
        auto t = Tracker();
        t.Observe(-500.0f, -300.0f, -500.0f);   // ignores
        t.Observe(-500.0f, -300.0f, -200.0f);   // handles
        Check("contradictory evidence flagged", t.Contradictory() ? 1.0f : 0.0f, 1.0f);
        CheckPolicy("  -> policy refuses to choose", t.Policy(),
                    mpf::EnginePolicy::kUnknown);
    }

    // Two confirmations required.
    {
        auto t = Tracker(mpf::EnginePolicy::kUnknown, 2);
        t.Observe(-500.0f, -300.0f, -500.0f);
        CheckPolicy("one of two confirmations", t.Policy(), mpf::EnginePolicy::kUnknown);
        t.Observe(-400.0f, -200.0f, -400.0f);
        CheckPolicy("two of two confirmations", t.Policy(),
                    mpf::EnginePolicy::kEngineIgnores);
    }

    // A pinned policy decides what the mod DOES, but must not stop it looking. This is
    // the whole safety net against a future game patch, so it is tested explicitly:
    // observation still runs, the pinned value still wins, and the disagreement is
    // reported.
    {
        auto t = Tracker(mpf::EnginePolicy::kEngineHandles);
        const bool informative = t.Observe(-500.0f, -300.0f, -500.0f);
        Check("pinned policy still observes", informative ? 1.0f : 0.0f, 1.0f);
        CheckPolicy("  -> pinned value still wins", t.Policy(),
                    mpf::EnginePolicy::kEngineHandles);
        CheckPolicy("  -> but the evidence is available separately", t.Observed(),
                    mpf::EnginePolicy::kEngineIgnores);
        Check("  -> disagreement reported", t.ContradictsPinned() ? 1.0f : 0.0f, 1.0f);
    }

    // The shipped default: pinned to "ignores". Evidence that agrees must not raise the
    // warning, or every normal session would print it.
    {
        auto t = Tracker(mpf::EnginePolicy::kEngineIgnores);
        t.Observe(-567.769f, -300.0f, -567.769f);
        CheckPolicy("shipped default, agreeing evidence", t.Policy(),
                    mpf::EnginePolicy::kEngineIgnores);
        Check("  -> no false alarm", t.ContradictsPinned() ? 1.0f : 0.0f, 0.0f);
    }

    // A patched game under the shipped default: the engine now does the right thing, and
    // the mod must say so rather than keep subtracting twice in silence.
    {
        auto t = Tracker(mpf::EnginePolicy::kEngineIgnores);
        t.Observe(-567.769f, -300.0f, -267.769f);
        Check("shipped default vs a patched game -> warned",
              t.ContradictsPinned() ? 1.0f : 0.0f, 1.0f);
        CheckPolicy("  -> still corrects until the player decides", t.Policy(),
                    mpf::EnginePolicy::kEngineIgnores);
    }

    // An uninformative transition must not raise the warning either.
    {
        auto t = Tracker(mpf::EnginePolicy::kEngineIgnores);
        t.Observe(0.0f, -20.0f, 0.0f);          // full pool, teaches nothing
        t.Observe(0.0f, +300.0f, -270.0f);      // self-cast, ambiguous
        Check("uninformative transitions raise nothing",
              t.ContradictsPinned() ? 1.0f : 0.0f, 0.0f);
    }

    // A save load resets the evidence.
    {
        auto t = Tracker();
        t.Observe(-500.0f, -300.0f, -500.0f);
        t.Reset();
        CheckPolicy("reset clears the evidence", t.Policy(), mpf::EnginePolicy::kUnknown);
    }

    // Tolerance is proportional: a 220 point fortification measured 0.5 points off must
    // still classify, and a 2 point change 0.5 points off must not.
    {
        auto t = Tracker();
        t.Observe(-500.0f, -220.0f, -499.5f);
        CheckPolicy("large change, 0.5 point measurement noise", t.Policy(),
                    mpf::EnginePolicy::kEngineIgnores);
    }
    {
        auto t = Tracker();
        t.Observe(-500.0f, -2.0f, -499.5f);
        CheckPolicy("small change, 0.5 point drift is not frozen", t.Policy(),
                    mpf::EnginePolicy::kUnknown);
    }
}

// ---------------------------------------------------------------------------
static void TestDecision()
{
    std::puts("\n-- the write decision ----------------------------------------------");
    const auto cfg = Glass();
    const auto ign = mpf::EnginePolicy::kEngineIgnores;
    const auto unk = mpf::EnginePolicy::kUnknown;
    const auto hnd = mpf::EnginePolicy::kEngineHandles;

    // Unambiguous and wrong: correct it, and the policy is not even needed.
    {
        const auto d = mpf::DecideCorrection(-300.0f, -567.769f, -567.769f, unk, cfg, true);
        CheckReason("frozen deficit, policy unknown -> still corrected", d.reason,
                    mpf::DecisionReason::kApply);
        Check("  new deficit", d.newDeficit, -267.769f);
        Check("  acted", d.act ? 1.0f : 0.0f, 1.0f);
    }

    // Ambiguous with no policy: abstain. Only reachable with iEnginePolicy = 0, since
    // the shipped default pins the policy. This is also the case the first design of the
    // guard got wrong, by treating a moved deficit as proof the engine had compensated.
    {
        const auto d = mpf::DecideCorrection(+300.0f, 0.0f, -270.0f, unk, cfg, true);
        CheckReason("self-cast, policy unknown -> abstain", d.reason,
                    mpf::DecisionReason::kNeedsCalibration);
        Check("  did not act", d.act ? 1.0f : 0.0f, 0.0f);
    }

    // Same transition once the policy is known. The target is computed from the deficit
    // AFTER the engine had its say, so the 270 the player paid stays paid: the new
    // deficit is -570 against a maximum of 620, i.e. 50 current, not 320.
    {
        const auto d = mpf::DecideCorrection(+300.0f, 0.0f, -270.0f, ign, cfg, true);
        CheckReason("self-cast, policy known -> corrected", d.reason,
                    mpf::DecisionReason::kApply);
        Check("  new deficit keeps the spell cost charged", d.newDeficit, -570.0f);
    }

    // An engine that already does the right thing must never be touched.
    {
        const auto d = mpf::DecideCorrection(-300.0f, -567.769f, -267.769f, hnd, cfg, true);
        CheckReason("engine handles it -> hands off", d.reason,
                    mpf::DecisionReason::kEngineHandlesIt);
        Check("  did not act", d.act ? 1.0f : 0.0f, 0.0f);
    }

    // The already-correct outcome: full pool, maximum drops, overflow spills. Nothing to
    // write, and this must not be reported as a fix.
    {
        const auto d = mpf::DecideCorrection(-20.0f, 0.0f, 0.0f, ign, cfg, true);
        CheckReason("full pool, maximum drops -> nothing to do", d.reason,
                    mpf::DecisionReason::kAlreadyCorrect);
    }

    // Dry run reports the arithmetic without acting.
    {
        const auto d = mpf::DecideCorrection(-300.0f, -567.769f, -567.769f, ign, cfg, false);
        CheckReason("dry run still reports the intent", d.reason,
                    mpf::DecisionReason::kApply);
        Check("  but did not act", d.act ? 1.0f : 0.0f, 0.0f);
        Check("  and still computes the value", d.newDeficit, -267.769f);
    }

    // Guards.
    {
        const auto d = mpf::DecideCorrection(0.0f, -100.0f, -100.0f, ign, cfg, true);
        CheckReason("no real change -> no action", d.reason,
                    mpf::DecisionReason::kNoChange);
    }
    {
        auto tight = cfg;
        tight.maxAbsDelta = 2000.0f;
        const auto d = mpf::DecideCorrection(50000.0f, 0.0f, 0.0f, ign, tight, true);
        CheckReason("implausible change -> no action", d.reason,
                    mpf::DecisionReason::kImplausible);
    }
    {
        const auto d = mpf::DecideCorrection(NAN, -100.0f, -100.0f, ign, cfg, true);
        CheckReason("non-finite input -> no action", d.reason,
                    mpf::DecisionReason::kImplausible);
    }
    {
        auto only = cfg;
        only.preserveOnIncrease = false;
        const auto d = mpf::DecideCorrection(+300.0f, -50.0f, -50.0f, ign, only, true);
        CheckReason("increase disabled -> no action", d.reason,
                    mpf::DecisionReason::kDirectionDisabled);
        const auto e = mpf::DecideCorrection(-300.0f, -500.0f, -500.0f, ign, only, true);
        CheckReason("decrease still corrected", e.reason, mpf::DecisionReason::kApply);
    }

    // Preset 0 resolves to both directions off, so nothing is ever corrected.
    {
        mpf::CompensationConfig off;
        off.preserveOnIncrease = false;
        off.preserveOnDecrease = false;
        off.epsilon            = 0.01f;
        off.maxAbsDelta        = 100000.0f;
        const auto d = mpf::DecideCorrection(-300.0f, -500.0f, -500.0f, ign, off, true);
        CheckReason("preset Off -> no action either way", d.reason,
                    mpf::DecisionReason::kDirectionDisabled);
    }
}

// ---------------------------------------------------------------------------
// The three transitions actually measured in game, replayed end to end.
static void TestFieldCases()
{
    std::puts("\n-- replay of measured transitions ----------------------------------");
    const auto cfg = Glass();

    // 6 Aug 2026 17:03:11 -- a 100 point Fortify Intelligence expiring on a pool that
    // was 567.769 short of its 620 maximum. Maximum fell to 320, the deficit did not
    // move, so current Magicka went from 52.23 to -247.77 and the player waited 19
    // seconds for a bar that was already showing empty.
    {
        auto t = Tracker();
        t.Observe(-567.769f, -300.0f, -567.769f);
        CheckPolicy("expiry: calibrates", t.Policy(), mpf::EnginePolicy::kEngineIgnores);
        const auto d = mpf::DecideCorrection(-300.0f, -567.769f, -567.769f, t.Policy(),
                                             cfg, true);
        CheckReason("expiry: decision", d.reason, mpf::DecisionReason::kApply);
        // Current after the fix: 320 - 267.769 = 52.231, exactly what it was before.
        Check("expiry: current Magicka preserved", 320.0f + d.newDeficit, 52.231f);
    }

    // 6 Aug 2026 17:02:15 -- a 270 point self-cast Fortify Intelligence on a full 320
    // pool. Maximum rose to 620 and the deficit went to -270, which is the spell cost
    // alone: the pool refilled to the new maximum for free, then paid. Observed result
    // 350 current, i.e. 30 more than before casting a 270 point spell.
    {
        auto primed = Tracker();
        primed.Observe(-567.769f, -300.0f, -567.769f);  // an earlier expiry calibrated it
        const auto d = mpf::DecideCorrection(+300.0f, 0.0f, -270.0f, primed.Policy(),
                                             cfg, true);
        CheckReason("self-cast: decision", d.reason, mpf::DecisionReason::kApply);
        // 620 - 570 = 50, which is 320 - 270. The free refill is gone.
        Check("self-cast: current Magicka charged the spell", 620.0f + d.newDeficit, 50.0f);
        Check("self-cast: free Magicka removed", (620.0f + d.newDeficit) - 350.0f, -300.0f);
    }

    // 6 Aug 2026 17:58:23 -- the same spell cast on a pool that was 40.2146 short of
    // its 320 maximum, so the free Magicka cannot be explained away as "the pool merely
    // refilled to the new maximum because it was already full". Observed: 279.79 current
    // before, 309.79 after, having paid 270. The engine handed over the whole +300.
    //
    // Under the shipped default this is corrected on the first frame, with no warm-up.
    {
        const auto d = mpf::DecideCorrection(+300.0f, -40.2146f, -310.2146f,
                                             mpf::EnginePolicy::kEngineIgnores, cfg, true);
        CheckReason("partial pool self-cast: decision", d.reason,
                    mpf::DecisionReason::kApply);
        Check("partial pool self-cast: current is 279.79 - 270",
              620.0f + d.newDeficit, 9.7854f);
        Check("partial pool self-cast: free Magicka removed",
              (620.0f + d.newDeficit) - 309.7854f, -300.0f);
    }

    // The same transition with iEnginePolicy = 0 and nothing learned yet: abstain. This
    // is the cost of auto-calibration, and the reason it is not the default.
    {
        const auto d = mpf::DecideCorrection(+300.0f, -40.2146f, -310.2146f,
                                             mpf::EnginePolicy::kUnknown, cfg, true);
        CheckReason("partial pool self-cast, uncalibrated: abstains", d.reason,
                    mpf::DecisionReason::kNeedsCalibration);
    }

    // 6 Aug 2026 17:59:19 -- the matching expiry 56 seconds later. The deficit moved by
    // +0.2076, not zero: that is regeneration inside the same tick. The tolerance is
    // 0.01 + 0.02 * 300 = 6.01, so it still reads as frozen. With a fixed hundredth-of-a
    // -point window this transition would have been misread and, under iEnginePolicy = 0,
    // calibration would never have happened at all.
    {
        auto t = Tracker();
        const bool informative = t.Observe(-591.9780f, -300.0f, -591.7704f);
        Check("expiry with regeneration in the same tick is still informative",
              informative ? 1.0f : 0.0f, 1.0f);
        CheckPolicy("  -> calibrates", t.Policy(), mpf::EnginePolicy::kEngineIgnores);
        const auto d = mpf::DecideCorrection(-300.0f, -591.9780f, -591.7704f, t.Policy(),
                                             cfg, true);
        Check("  -> new deficit", d.newDeficit, -291.7704f);
        Check("  -> current 28.23 instead of -271.77", 320.0f + d.newDeficit, 28.2296f);
    }

    // 6 Aug 2026 17:16:16 -- a ring worth 10 points of Intelligence removed after the
    // pool had refilled. Maximum 320 -> 300, deficit 0 -> 0. Nothing wrong here.
    {
        auto t = Tracker();
        const bool informative = t.Observe(0.0f, -20.0f, 0.0f);
        Check("ring removal: not usable for calibration",
              informative ? 1.0f : 0.0f, 0.0f);
        const auto d = mpf::DecideCorrection(-20.0f, 0.0f, 0.0f,
                                             mpf::EnginePolicy::kEngineIgnores, cfg, true);
        CheckReason("ring removal: decision", d.reason,
                    mpf::DecisionReason::kAlreadyCorrect);
    }
}

// The warning for an engine that moves the deficit on a decrease (a floor at zero).
// Measured cases must stay silent; a synthetic floor must not.
static void TestEngineMoveWarning()
{
    std::puts("\n-- engine moving the deficit on a decrease --------------------------");
    const float eps = 0.01f;
    // 28 Aug 2026, event 13: Intelligence -1 (max -2), regeneration +0.1373 in the poll.
    Check("28 Aug event 13 (regen in the poll): silent",
          mpf::EngineRaisedDeficitOnDecrease(-2.0f, -261.7483f, -261.6110f, eps), 0.0f);
    // 28 Aug 2026, event 15: Magicka -22, regeneration +0.0238.
    Check("28 Aug event 15 (drain, engine idle): silent",
          mpf::EngineRaisedDeficitOnDecrease(-22.0f, -356.9238f, -356.9000f, eps), 0.0f);
    // 6 Aug 2026 17:03:11: the -300 expiry the engine did not touch at all.
    Check("6 Aug 17:03 expiry (deficit untouched): silent",
          mpf::EngineRaisedDeficitOnDecrease(-300.0f, -567.769f, -567.769f, eps), 0.0f);
    // A full pool losing its overflow: deficit 0 -> 0.
    Check("full pool, overflow spilled: silent",
          mpf::EngineRaisedDeficitOnDecrease(-300.0f, 0.0f, 0.0f, eps), 0.0f);
    // Increases are not this check's business, even with a big move.
    Check("increase with a large move: silent",
          mpf::EngineRaisedDeficitOnDecrease(300.0f, 0.0f, -270.0f, eps), 0.0f);
    // Synthetic floor: maximum 620 -> 520, current 50 would become -50; an engine
    // flooring at zero sets the deficit to -520 instead of -570, a move of +50.
    Check("synthetic floor at zero: reported (+50)",
          mpf::EngineRaisedDeficitOnDecrease(-100.0f, -570.0f, -520.0f, eps), 50.0f);
    // Just over what one stretched poll of regeneration explains.
    Check("move of 5 points on a -22 change: reported",
          mpf::EngineRaisedDeficitOnDecrease(-22.0f, -100.0f, -95.0f, eps), 5.0f);
}

int main()
{
    TestCorrectDeficit();
    TestCalibration();
    TestDecision();
    TestFieldCases();
    TestEngineMoveWarning();
    std::printf("\n%s\n", g_fail == 0 ? "all corrector tests passed"
                                      : "THERE WERE FAILURES");
    return g_fail == 0 ? 0 : 1;
}
