// test_calibration.cpp - MagickaPoolFix
//
// Validates the hypothesis calibrator against two simulated engines:
//   engine A: GetActorFloatValue(Magicka) returns CURRENT magicka
//   engine B: GetActorFloatValue(Magicka) returns MAXIMUM magicka
//
// The calibrator does not know which one it faces and must work it out alone.
//
// Build:
//   g++ -std=c++20 -Wall -Wextra -I../src -o test_calibration test_calibration.cpp

#include "HypothesisCalibrator.h"

#include <cmath>
#include <cstdio>
#include <string>

using mpf::Calibrator;
using mpf::CalibrationConfig;
using mpf::Hypothesis;

static int g_fail = 0;

static void CheckStr(const std::string& label, const std::string& got,
                     const std::string& want) {
    const bool ok = (got == want);
    if (!ok) { ++g_fail; }
    std::printf("%-62s got=%-22s want=%-22s %s\n", label.c_str(), got.c_str(),
                want.c_str(), ok ? " OK" : " FAIL");
}

static void CheckNum(const std::string& label, float got, float want) {
    const bool ok = std::fabs(got - want) < 0.01f;
    if (!ok) { ++g_fail; }
    std::printf("%-62s got=%9.2f want=%9.2f %s\n", label.c_str(), got, want,
                ok ? " OK" : " FAIL");
}

static const char* Name(Hypothesis h) {
    switch (h) {
    case Hypothesis::ValueIsCurrent: return "CURRENT";
    case Hypothesis::ValueIsMax:     return "MAX";
    default:                         return "UNKNOWN";
    }
}

// Simulated engine. The internal truth is (max, modifier) with modifier <= 0 and
// current = max + modifier. The engine exposes a single value v depending on the
// mode, exactly like the game does.
struct FakeEngine {
    enum class Mode { ExposesCurrent, ExposesMax };

    Mode  mode;
    float max      = 300.0f;
    float modifier = 0.0f;

    float Current() const { return max + modifier; }
    float Exposed() const { return mode == Mode::ExposesCurrent ? Current() : max; }

    // Natural regeneration: the modifier climbs back towards zero.
    void Regen(float amount) {
        modifier += amount;
        if (modifier > 0.0f) { modifier = 0.0f; }
    }

    // Casting: the modifier goes down.
    void Cast(float cost) { modifier -= cost; }

    // Fortify / Drain: only the maximum moves, the modifier is not adjusted.
    // That is precisely the pathology this plugin fixes.
    void ChangeMax(float delta) { max += delta; }
};

// Nominal scenario: the player has spent magicka and is regenerating. No
// specific player action is required, natural regeneration is enough.
static Hypothesis RunRegenScenario(FakeEngine::Mode mode, int frames,
                                   const CalibrationConfig& cfg,
                                   Calibrator* outCal = nullptr) {
    FakeEngine eng{mode};
    eng.modifier = -180.0f;  // save loaded at 120/300

    Calibrator cal(cfg);
    for (int i = 0; i < frames; ++i) {
        eng.Regen(0.18f);  // ~11 pts/s at 60 fps, Willpower 100 out of combat
        cal.Feed(eng.Exposed(), eng.modifier);
    }
    if (outCal) { *outCal = cal; }
    return cal.Get();
}

int main() {
    const CalibrationConfig cfg;  // 8 events, margin 4, 3600 frames

    std::printf("--- Nominal scenario: regeneration only, 60 frames (1 s) ---\n");
    {
        Calibrator calA;
        const Hypothesis hA = RunRegenScenario(FakeEngine::Mode::ExposesCurrent,
                                               60, cfg, &calA);
        CheckStr("engine exposes CURRENT -> verdict", Name(hA), "CURRENT");
        CheckNum("  score A (candidate v-modifier stays stable)", (float)calA.ScoreA(), 8);
        CheckNum("  score B", (float)calA.ScoreB(), 0);

        Calibrator calB;
        const Hypothesis hB = RunRegenScenario(FakeEngine::Mode::ExposesMax,
                                               60, cfg, &calB);
        CheckStr("engine exposes MAX -> verdict", Name(hB), "MAX");
        CheckNum("  score A", (float)calB.ScoreA(), 0);
        CheckNum("  score B (candidate v stays stable)", (float)calB.ScoreB(), 8);
    }

    std::printf("\n--- Speed: 9 frames are enough (0.15 s at 60 fps) ---\n");
    {
        Calibrator cal;
        const Hypothesis h = RunRegenScenario(FakeEngine::Mode::ExposesCurrent,
                                             9, cfg, &cal);
        CheckStr("verdict reached within 9 frames", Name(h), "CURRENT");
        CheckNum("  frames consumed", (float)cal.Frames(), 9);
    }

    std::printf("\n--- Casting instead of regenerating ---\n");
    {
        for (int mode = 0; mode < 2; ++mode) {
            FakeEngine eng{mode == 0 ? FakeEngine::Mode::ExposesCurrent
                                     : FakeEngine::Mode::ExposesMax};
            Calibrator cal(cfg);
            for (int i = 0; i < 12; ++i) {
                eng.Cast(20.0f);  // 12 spells at 20: the pool goes below zero
                cal.Feed(eng.Exposed(), eng.modifier);
            }
            CheckStr(mode == 0 ? "spending, CURRENT engine -> verdict"
                               : "spending, MAX engine -> verdict",
                     Name(cal.Get()), mode == 0 ? "CURRENT" : "MAX");
        }
    }

    std::printf("\n--- Robustness: Fortify during calibration ---\n");
    {
        // The maximum changes while the modifier does not: dm = 0, so no event is
        // counted. Calibration must not be polluted.
        FakeEngine eng{FakeEngine::Mode::ExposesCurrent};
        eng.modifier = -180.0f;
        Calibrator cal(cfg);
        for (int i = 0; i < 40; ++i) {
            if (i == 5)  { eng.ChangeMax(+300.0f); }  // buff activation
            if (i == 25) { eng.ChangeMax(-300.0f); }  // buff expiry
            eng.Regen(0.18f);
            cal.Feed(eng.Exposed(), eng.modifier);
        }
        CheckStr("verdict despite two changes of the maximum", Name(cal.Get()),
                 "CURRENT");
        CheckNum("  no point awarded to the wrong hypothesis",
                 (float)cal.ScoreB(), 0);
    }

    std::printf("\n--- Robustness: ambiguous event (max and modifier both move) ---\n");
    {
        // Fortify on the exact same frame as a regeneration tick: neither candA
        // nor candB is stable, so the event must be discarded rather than
        // misattributed.
        FakeEngine eng{FakeEngine::Mode::ExposesCurrent};
        eng.modifier = -180.0f;
        Calibrator cal(cfg);
        cal.Feed(eng.Exposed(), eng.modifier);  // priming frame

        eng.ChangeMax(+300.0f);
        eng.Regen(0.18f);
        const auto step = cal.Feed(eng.Exposed(), eng.modifier);
        CheckNum("event detected", step.eventObserved ? 1.0f : 0.0f, 1);
        CheckNum("event NOT counted (ambiguous)", step.eventCounted ? 1.0f : 0.0f, 0);
        CheckNum("  score A unchanged", (float)cal.ScoreA(), 0);
        CheckNum("  score B unchanged", (float)cal.ScoreB(), 0);
    }

    std::printf("\n--- Undecidable case: full magicka, nothing moves ---\n");
    {
        FakeEngine eng{FakeEngine::Mode::ExposesCurrent};
        eng.modifier = 0.0f;  // 300/300, no variation possible
        CalibrationConfig quick = cfg;
        quick.maxFrames = 30;
        Calibrator cal(quick);
        bool gaveUp = false;
        for (int i = 0; i < 40; ++i) {
            const auto st = cal.Feed(eng.Exposed(), eng.modifier);
            if (st.justGaveUp) { gaveUp = true; }
        }
        CheckStr("no verdict is forced by default", Name(cal.Get()), "UNKNOWN");
        CheckNum("give-up reported after maxFrames", gaveUp ? 1.0f : 0.0f, 1);
        float c = 0.0f, m = 0.0f;
        CheckNum("Resolve() refuses to answer -> no memory write possible",
                 cal.Resolve(100.0f, -50.0f, c, m) ? 1.0f : 0.0f, 0);
    }

    std::printf("\n--- Non-finite values ignored without consequence ---\n");
    {
        Calibrator cal(cfg);
        const float nan = std::nanf("");
        for (int i = 0; i < 5; ++i) { cal.Feed(nan, nan); }
        CheckNum("non-finite frames are not counted", (float)cal.Frames(), 0);
        CheckStr("state unchanged", Name(cal.Get()), "UNKNOWN");
    }

    std::printf("\n--- Resolve(): correct reading under each hypothesis ---\n");
    {
        Calibrator cal;
        float cur = 0.0f, max = 0.0f;

        cal.Force(Hypothesis::ValueIsCurrent);
        cal.Resolve(120.0f, -180.0f, cur, max);
        CheckNum("hypothesis CURRENT: current", cur, 120);
        CheckNum("hypothesis CURRENT: max", max, 300);

        cal.Force(Hypothesis::ValueIsMax);
        cal.Resolve(300.0f, -180.0f, cur, max);
        CheckNum("hypothesis MAX: current", cur, 120);
        CheckNum("hypothesis MAX: max", max, 300);
    }

    std::printf("\n--- Manual escape hatch (iForceHypothesis) ---\n");
    {
        Calibrator cal(cfg);
        cal.Force(Hypothesis::ValueIsMax);
        CheckStr("verdict forced", Name(cal.Get()), "MAX");
        // A forced hypothesis must never be recomputed.
        FakeEngine eng{FakeEngine::Mode::ExposesCurrent};
        eng.modifier = -180.0f;
        for (int i = 0; i < 60; ++i) {
            eng.Regen(0.18f);
            cal.Feed(eng.Exposed(), eng.modifier);
        }
        CheckStr("calibration does not override the forced verdict",
                 Name(cal.Get()), "MAX");
        CheckNum("  no score accumulated", (float)(cal.ScoreA() + cal.ScoreB()), 0);
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL TESTS PASS" : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
