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

    // 25 Oct 2026 23:42:49 -- the buff cast on a full pool, cost 282 for a maximum
    // raised by 285. The deficit landed 3 points from the corrected value, inside the
    // 5.71 tolerance, and was counted as "the engine handles it": the warning to remove
    // the plugin was printed. A cost close to the increase must not count.
    {
        auto t = Tracker(mpf::EnginePolicy::kEngineIgnores);
        const bool informative = t.Observe(0.0f, +285.0f, -282.0f);
        Check("cast costing about its increase is not informative",
              informative ? 1.0f : 0.0f, 0.0f);
        Check("  -> no false alarm under the shipped default",
              t.ContradictsPinned() ? 1.0f : 0.0f, 0.0f);
        const auto d = mpf::DecideCorrection(+285.0f, 0.0f, -282.0f,
                                             mpf::EnginePolicy::kEngineIgnores,
                                             Glass(), true);
        Check("  -> still corrected: 322 - 282 = 40", 607.0f + d.newDeficit, 40.0f);
    }

    // The price of the rule above, accepted: an increase that cost nothing and that a
    // patched engine compensated in full looks the same, so it no longer counts. A
    // patched engine is still caught on the first decrease on a partial pool.
    {
        auto t = Tracker(mpf::EnginePolicy::kEngineIgnores);
        t.Observe(-100.0f, +300.0f, -400.0f);
        Check("increase landing on the corrected value: not counted",
              t.ContradictsPinned() ? 1.0f : 0.0f, 0.0f);
        t.Observe(-400.0f, -300.0f, -100.0f);
        Check("  -> the next decrease still warns",
              t.ContradictsPinned() ? 1.0f : 0.0f, 1.0f);
    }

    // An unchanged deficit on an increase stays evidence of "ignores": a compensating
    // engine would need the spell to refund magicka (§5.3.6).
    {
        auto t = Tracker();
        t.Observe(-100.0f, +300.0f, -100.0f);
        CheckPolicy("item equipped, deficit frozen", t.Policy(),
                    mpf::EnginePolicy::kEngineIgnores);
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

// Recasting an active spell, which the game sometimes does in two steps (Corrector.h,
// kRecastWindowMs; docs/SPEC.md section 5.5). Field values from 21 October 2026,
// session 4. "Current" is maximum + deficit.
static void TestRecast()
{
    std::printf("\n-- recast of an active spell: the give-back window ------------------\n");
    const auto cfg = Glass();
    const auto pol = mpf::EnginePolicy::kEngineIgnores;

    {
        // 21:36:08.324 then .439, 115 ms apart: the 95 % buff leaves (current 322.71 of
        // 607), then comes back with its cost. Without the give-back the 0.71 above 322
        // was lost; with it the recast costs exactly 278.
        const auto a = mpf::DecideCorrection(-285.0f, -284.2858f, -284.2858f, pol, cfg, true);
        CheckReason("115 ms split, step 1: the expiry-shaped half is corrected", a.reason,
                    mpf::DecisionReason::kApply);
        Check("115 ms split, step 1: deficit written", a.newDeficit, 0.0f);
        const float spill = mpf::SpilledOnDecrease(-285.0f, -284.2858f, true);
        Check("115 ms split: spill remembered", spill, 0.7142f);
        const float back = mpf::RecastGiveBack(spill, 115, 285.0f);
        Check("115 ms split: given back in full", back, 0.7142f);
        const auto b = mpf::DecideCorrection(285.0f, 0.0f, -278.0f, pol, cfg, true, back);
        Check("115 ms split, step 2: deficit written", b.newDeficit, -562.2858f);
        Check("115 ms split: current after = 322.71 - 278", 607.0f + b.newDeficit, 44.7142f);
    }
    {
        // The same shape on a FULL pool (622 of 622), which the session did not hit:
        // without the give-back the recast would cost 278 + 300.
        const auto a = mpf::DecideCorrection(-300.0f, 0.0f, 0.0f, pol, cfg, true);
        CheckReason("full pool split, step 1: pool full at 322, nothing to write",
                    a.reason, mpf::DecisionReason::kAlreadyCorrect);
        const float spill = mpf::SpilledOnDecrease(-300.0f, 0.0f, true);
        Check("full pool split: 300 spilled", spill, 300.0f);
        const auto without = mpf::DecideCorrection(300.0f, 0.0f, -278.0f, pol, cfg, true);
        Check("full pool split, no give-back: current 44 (pays 578)",
              622.0f + without.newDeficit, 44.0f);
        const float back = mpf::RecastGiveBack(spill, 115, 300.0f);
        const auto b = mpf::DecideCorrection(300.0f, 0.0f, -278.0f, pol, cfg, true, back);
        CheckReason("full pool split, step 2: the engine's value is already right",
                    b.reason, mpf::DecisionReason::kAlreadyCorrect);
        const float dEnd = b.reason == mpf::DecisionReason::kApply ? b.newDeficit : -278.0f;
        Check("full pool split, with give-back: current 344 (pays 278)", 622.0f + dEnd,
              344.0f);
    }
    {
        // 21:34:02.649 then .666: removal and cost in the same poll, so nothing spills
        // and nothing is given back; the log's values are reproduced.
        const auto a = mpf::DecideCorrection(-300.0f, -283.0f, -561.0f, pol, cfg, true);
        Check("one-poll recast, step 1: deficit written", a.newDeficit, -261.0f);
        const float spill = mpf::SpilledOnDecrease(-300.0f, -561.0f, true);
        Check("one-poll recast: nothing spilled", spill, 0.0f);
        const auto b = mpf::DecideCorrection(300.0f, -261.0f, -261.0f, pol, cfg, true,
                                             mpf::RecastGiveBack(spill, 17, 300.0f));
        Check("one-poll recast, step 2: deficit written as logged", b.newDeficit, -561.0f);
    }
    {
        // 22:45:38.396 then 22:45:40.489: the buff expired (current 339 of 622, 17 above
        // the new 322) and was cast again 2.09 s later. Not a recast: no give-back.
        const float spill = mpf::SpilledOnDecrease(-300.0f, -283.0f, true);
        Check("expiry then cast 2.09 s later: 17 spilled", spill, 17.0f);
        Check("expiry then cast 2.09 s later: nothing given back",
              mpf::RecastGiveBack(spill, 2093, 300.0f), 0.0f);
        const auto b = mpf::DecideCorrection(300.0f, 0.0f, -278.0f, pol, cfg, true, 0.0f);
        Check("expiry then cast 2.09 s later: deficit written as logged", b.newDeficit,
              -578.0f);
    }
    {
        // 21:46:05.428 then 21:46:06.108, 680 ms apart: the buff leaves (current 324 of
        // 607, 2 above the new 322), and comes back with its cost. Inside 1000 ms.
        const auto a = mpf::DecideCorrection(-285.0f, -283.0f, -283.0f, pol, cfg, true);
        Check("680 ms split, step 1: deficit written", a.newDeficit, 0.0f);
        const float spill = mpf::SpilledOnDecrease(-285.0f, -283.0f, true);
        Check("680 ms split: 2 spilled", spill, 2.0f);
        const auto b = mpf::DecideCorrection(285.0f, 0.0f, -278.0f, pol, cfg, true,
                                             mpf::RecastGiveBack(spill, 680, 285.0f));
        Check("680 ms split: current after = 324 - 278", 607.0f + b.newDeficit, 46.0f);
    }
    {
        // 28 August 21:29:09 then :10, 0.7 s apart: an item off and on by hand on a full
        // pool. The fix spilled 20 then; with the window the round trip is neutral,
        // which is the harmless side of the judgement (Corrector.h).
        const float spill = mpf::SpilledOnDecrease(-20.0f, 0.0f, true);
        Check("ring off and on by hand, 0.7 s: neutral now",
              mpf::RecastGiveBack(spill, 700, 20.0f), 20.0f);
        // Session 1 of 21 October, 1.25 s: outside, the 20 stay spilled.
        Check("ring off and on by hand, 1.25 s: 20 stay spilled",
              mpf::RecastGiveBack(spill, 1250, 20.0f), 0.0f);
    }
    // The window's edges, and the caps.
    Check("give-back at exactly 1000 ms", mpf::RecastGiveBack(300.0f, 1000, 300.0f), 300.0f);
    Check("give-back at 1001 ms", mpf::RecastGiveBack(300.0f, 1001, 300.0f), 0.0f);
    Check("give-back capped by the increase", mpf::RecastGiveBack(300.0f, 50, 20.0f), 20.0f);
    Check("no give-back on a decrease", mpf::RecastGiveBack(300.0f, 50, -20.0f), 0.0f);
    Check("no spill on an increase", mpf::SpilledOnDecrease(300.0f, 0.0f, true), 0.0f);
    Check("no spill when the overflow is kept", mpf::SpilledOnDecrease(-300.0f, 0.0f, false),
          0.0f);
    Check("no spill without overflow", mpf::SpilledOnDecrease(-300.0f, -400.0f, true), 0.0f);
    {
        // A give-back passed with a decrease is ignored.
        const auto a = mpf::DecideCorrection(-20.0f, -100.0f, -100.0f, pol, cfg, true, 50.0f);
        Check("give-back ignored on a decrease", a.newDeficit, -80.0f);
        // And it never lifts the deficit above what the engine left.
        const auto b = mpf::DecideCorrection(20.0f, -100.0f, -100.0f, pol, cfg, true, 500.0f);
        CheckReason("give-back never above the engine's deficit: nothing to write",
                    b.reason, mpf::DecisionReason::kAlreadyCorrect);
    }
    {
        // Scenario 6 of chapter 8 with the worst timing for the fix's neutrality: the
        // buff expires on a full pool and is recast within the window, ten times. Each
        // loop must cost exactly the spell, never less: no positive drift.
        float current = 622.0f;
        float minPaid = 1e9f, maxPaid = -1e9f;
        for (int i = 0; i < 10; ++i) {
            // expiry at the current pool: max 622 -> 322
            const float dBefore = current - 622.0f;
            const auto a = mpf::DecideCorrection(-300.0f, dBefore, dBefore, pol, cfg, true);
            const float d1 = (a.reason == mpf::DecisionReason::kApply) ? a.newDeficit : dBefore;
            const float spill = mpf::SpilledOnDecrease(-300.0f, dBefore, true);
            // recast 100 ms later, cost 278 charged in the same poll
            const float dAfter = d1 - 278.0f;
            const auto b = mpf::DecideCorrection(300.0f, d1, dAfter, pol, cfg, true,
                                                 mpf::RecastGiveBack(spill, 100, 300.0f));
            const float d2 = (b.reason == mpf::DecisionReason::kApply) ? b.newDeficit : dAfter;
            const float next = 622.0f + d2;
            const float paid = current - next;
            if (paid < minPaid) { minPaid = paid; }
            if (paid > maxPaid) { maxPaid = paid; }
            current = 622.0f;   // regenerate fully before the next loop
        }
        Check("ten recasts at expiry, full pool: cheapest loop costs the spell", minPaid,
              278.0f);
        Check("ten recasts at expiry, full pool: dearest loop costs the spell", maxPaid,
              278.0f);
    }
}

int main()
{
    TestCorrectDeficit();
    TestCalibration();
    TestDecision();
    TestFieldCases();
    TestEngineMoveWarning();
    TestRecast();
    std::printf("\n-- short texts for the one-line summary ---------------------------\n");
    {
        const auto chk = [](const char* name, const char* got, const char* want) {
            const bool ok = std::string(got) == want;
            if (!ok) ++g_fail;
            std::printf("%-66s got=%-18s want=%-18s %s\n", name, got, want, ok ? "OK" : "FAIL");
        };
        chk("reason: apply", mpf::ReasonShort(mpf::DecisionReason::kApply), "corrected");
        chk("reason: already correct", mpf::ReasonShort(mpf::DecisionReason::kAlreadyCorrect),
            "nothing to do");
        chk("verdict: free magicka", mpf::VerdictName(mpf::MaxChangeBehaviour::kFreeMagicka),
            "FREE MAGICKA");
        chk("verdict: stolen", mpf::VerdictName(mpf::MaxChangeBehaviour::kLostMagicka),
            "MAGICKA STOLEN");
    }
    std::printf("\n%s\n", g_fail == 0 ? "all corrector tests passed"
                                      : "THERE WERE FAILURES");
    return g_fail == 0 ? 0 : 1;
}
