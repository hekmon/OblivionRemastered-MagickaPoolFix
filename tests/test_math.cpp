// Tests for the "glass of water" semantics. Builds and runs outside Windows.
//   g++ -std=c++20 -I../src test_math.cpp -o test_math && ./test_math

#include "MagickaMath.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_fail = 0;

static void Check(const std::string& name, float got, float want)
{
    const bool ok = std::fabs(got - want) < 0.001f;
    if (!ok) ++g_fail;
    std::printf("%-58s got=%9.2f want=%9.2f  %s\n",
                name.c_str(), got, want, ok ? "OK" : "FAIL");
}

// Simulator: game state as deduced from the engine.
struct Sim
{
    float maxValue;   // magicka_max
    float modifier;   // magickaModifier (<= 0)
    float Current() const { return maxValue + modifier; }
};

// Apply a change of the maximum (fortify cast or expiry), then the fix.
static void ChangeMax(Sim& s, float newMax, const mpf::CompensationConfig& cfg, bool fixEnabled)
{
    const float lastMax = s.maxValue;
    s.maxValue = newMax;  // the engine recomputes the max, the offset is left as is
    if (!fixEnabled) return;
    const auto r = mpf::ComputeCompensation(lastMax, newMax, s.modifier, cfg);
    if (r.apply) s.modifier = r.newModifier;
}


// ---------------------------------------------------------------------------
// Classification of the engine's response to a change in maximum Magicka.
//
// This is the one remaining unknown in the project, and 0.5.0 answers it in the
// field with a single Fortify Intelligence cast. These tests pin the decision
// boundaries so a field log can be trusted without re-deriving them by hand.
// A switch with every case listed and no default, rather than an array indexed by
// the enum. The array version was a real defect: when the enum grew from three cases
// to five it kept compiling and read past its end, and the test binary died with a
// segmentation fault before printing a single line. This form makes the compiler
// refuse the same mistake, since -Wswitch flags an unhandled enumerator.
static const char* ClassName(mpf::MaxChangeBehaviour v)
{
    switch (v) {
    case mpf::MaxChangeBehaviour::kPreserved:       return "preserved";
    case mpf::MaxChangeBehaviour::kSpilledFromFull: return "spilled";
    case mpf::MaxChangeBehaviour::kFreeMagicka:     return "free";
    case mpf::MaxChangeBehaviour::kLostMagicka:     return "stolen";
    case mpf::MaxChangeBehaviour::kPartial:         return "partial";
    }
    return "?";
}

static void CheckClass(const std::string& name, mpf::MaxChangeBehaviour got,
                       mpf::MaxChangeBehaviour want)
{
    const bool ok = got == want;
    if (!ok) ++g_fail;
    std::printf("%-58s got=%9s want=%9s  %s\n", name.c_str(), ClassName(got),
                ClassName(want), ok ? "OK" : "FAIL");
}

static void TestClassification()
{
    using B = mpf::MaxChangeBehaviour;
    const float eps = 0.01f;

    // Fortify Intelligence 110 on a Breton mage: maximum rises by 2 x 110 = 220.
    // If the engine deepens the deficit by the same 220, current Magicka is
    // untouched and there is nothing to fix.
    CheckClass("fortify Int +110: deficit follows -> preserved",
               mpf::ClassifyMaxChange(220.0f, -220.0f, 0.0f, eps), B::kPreserved);

    // If the deficit does not move, the whole 220 points land in the pool as free
    // Magicka. That is the bug.
    CheckClass("fortify Int +110: deficit frozen -> free Magicka",
               mpf::ClassifyMaxChange(220.0f, 0.0f, 0.0f, eps), B::kFreeMagicka);

    // Expiry is the mirror image: maximum falls by 220.
    CheckClass("expiry -220: deficit follows -> preserved",
               mpf::ClassifyMaxChange(-220.0f, 220.0f, 0.0f, eps), B::kPreserved);
    // From a full pool that is the correct spill; from a pool that is 10 points down
    // the same frozen deficit steals 220 points of Magicka the player still had.
    CheckClass("expiry -220 from a full pool: deficit frozen -> spilled",
               mpf::ClassifyMaxChange(-220.0f, 0.0f, 0.0f, eps), B::kSpilledFromFull);
    CheckClass("expiry -220 from a pool 10 down: deficit frozen -> stolen",
               mpf::ClassifyMaxChange(-220.0f, 0.0f, -10.0f, eps), B::kLostMagicka);

    // Fortify Magicka 100 contributes to maximum directly, not through Intelligence.
    CheckClass("fortify Magicka +100: deficit follows -> preserved",
               mpf::ClassifyMaxChange(100.0f, -100.0f, 0.0f, eps), B::kPreserved);

    // A proportional rule must NOT be mistaken for either clean case, otherwise the
    // field log would silently endorse a compensation formula that is wrong.
    CheckClass("half-scaled response -> partial",
               mpf::ClassifyMaxChange(220.0f, -110.0f, 0.0f, eps), B::kPartial);
    CheckClass("tenth-scaled response -> partial",
               mpf::ClassifyMaxChange(220.0f, -22.0f, 0.0f, eps), B::kPartial);

    // Tolerance scales at 2 percent of the change in maximum, so 220 buys a window
    // of about 4.41 points. Just inside is preserved, just outside is partial.
    CheckClass("220 with 4.0 of slack -> preserved",
               mpf::ClassifyMaxChange(220.0f, -224.0f, 0.0f, eps), B::kPreserved);
    CheckClass("220 with 6.0 of slack -> partial",
               mpf::ClassifyMaxChange(220.0f, -226.0f, 0.0f, eps), B::kPartial);

    // Small changes must still be judged tightly: a +2 maximum gets a window of
    // 0.05, so a 1.0 discrepancy is partial, not preserved.
    CheckClass("small +2 change, 1.0 discrepancy -> partial",
               mpf::ClassifyMaxChange(2.0f, -1.0f, 0.0f, eps), B::kPartial);
    CheckClass("small +2 change, exact -> preserved",
               mpf::ClassifyMaxChange(2.0f, -2.0f, 0.0f, eps), B::kPreserved);

    // Degenerate case: no change in maximum at all. Both branches are satisfiable,
    // and kPreserved must win, because a deficit that did not move while maximum did
    // not move is trivially a preserved pool rather than evidence of a bug.
    CheckClass("no change at all -> preserved",
               mpf::ClassifyMaxChange(0.0f, 0.0f, 0.0f, eps), B::kPreserved);


    // ---- direction and pool state now change the verdict --------------------
    // Reproduces the field observation of 6 August 2026: a Fortify Intelligence 100
    // and Fortify Magicka 100 expiring together while the pool was full. Maximum fell
    // by 2 x 100 + 100 = 300 and the deficit stayed at zero. Version 0.5.1 called
    // that the bug; it is in fact the correct spill of the water above the new rim.
    CheckClass("field case: max -300 from a full pool, deficit frozen -> spilled",
               mpf::ClassifyMaxChange(-300.0f, 0.0f, 0.0f, eps), B::kSpilledFromFull);

    // The same arithmetic with the pool 50 points down is the damaging half of the
    // bug: there was no overflow, so current Magicka should not have moved at all.
    CheckClass("max -300 with a partly empty pool, deficit frozen -> stolen",
               mpf::ClassifyMaxChange(-300.0f, 0.0f, -50.0f, eps), B::kLostMagicka);

    // An increase with a frozen deficit is free Magicka whatever the pool state.
    CheckClass("max +300, full pool, deficit frozen -> free Magicka",
               mpf::ClassifyMaxChange(300.0f, 0.0f, 0.0f, eps), B::kFreeMagicka);
    CheckClass("max +300, partly empty pool, deficit frozen -> free Magicka",
               mpf::ClassifyMaxChange(300.0f, 0.0f, -50.0f, eps), B::kFreeMagicka);

    // A deficit that tracks a DECREASE exactly leaves current Magicka alone, and must
    // not be mistaken for a spill just because maximum went down.
    CheckClass("max -300 with the deficit tracking it -> preserved, not spilled",
               mpf::ClassifyMaxChange(-300.0f, 300.0f, -400.0f, eps), B::kPreserved);

    // A pool one hundredth of a point from full counts as full, so the epsilon is
    // what separates a spill from a theft rather than an exact comparison to zero.
    CheckClass("deficit -0.005 counts as full -> spilled",
               mpf::ClassifyMaxChange(-300.0f, 0.0f, -0.005f, eps), B::kSpilledFromFull);
    CheckClass("deficit -0.02 does not count as full -> stolen",
               mpf::ClassifyMaxChange(-300.0f, 0.0f, -0.02f, eps), B::kLostMagicka);

    // The Intelligence-to-Magicka derivative is the only constant in the model that
    // is taken from the wiki rather than measured, so pin it.
    Check("Magicka gained per point of Intelligence",
          mpf::kMagickaPerIntelligence, 2.0f);

    // And the composite prediction the plugin actually computes: a simultaneous
    // Fortify Intelligence 110 and Fortify Magicka 100, which is the spell the
    // player owns, must predict 2 x 110 + 100 = 320.
    Check("predicted dMax for Int +110 and Magicka +100",
          mpf::kMagickaPerIntelligence * 110.0f + 100.0f, 320.0f);
}

int main()
{
    TestClassification();

    mpf::CompensationConfig cfg{};  // full fix by default

    std::puts("--- Vanilla behaviour (bug reproduction) ---");
    {
        Sim s{300.0f, -200.0f};  // 100 / 300
        Check("vanilla: current before fortify", s.Current(), 100.0f);
        ChangeMax(s, 600.0f, cfg, /*fixEnabled=*/false);
        Check("vanilla: fortify +300 grants free magicka", s.Current(), 400.0f);

        Sim s2{600.0f, -500.0f};  // 100 / 600
        ChangeMax(s2, 300.0f, cfg, /*fixEnabled=*/false);
        Check("vanilla: expiry at 100/600 yields negative pool", s2.Current(), -200.0f);
    }

    std::puts("\n--- Fix enabled ---");
    {
        // Case 1: activation grants no free magicka.
        Sim s{300.0f, -200.0f};  // 100 / 300
        ChangeMax(s, 600.0f, cfg, true);
        Check("fix: activation leaves current untouched", s.Current(), 100.0f);
        Check("fix: activation raises max to 600", s.maxValue, 600.0f);

        // Case 2: the exact reported case, 100/600 then buff expiry.
        ChangeMax(s, 300.0f, cfg, true);
        Check("fix: expiry at 100/600 stays at 100", s.Current(), 100.0f);
        Check("fix: expiry lowers max to 300", s.maxValue, 300.0f);
    }
    {
        // Case 3: overflow spilled, 350/600 -> 300/300.
        Sim s{600.0f, -250.0f};  // 350 / 600
        Check("fix: initial state 350/600", s.Current(), 350.0f);
        ChangeMax(s, 300.0f, cfg, true);
        Check("fix: expiry at 350/600 spills overflow to 300", s.Current(), 300.0f);
    }
    {
        // Case 4: exactly at the limit, nothing to spill.
        Sim s{600.0f, -300.0f};  // 300 / 600
        ChangeMax(s, 300.0f, cfg, true);
        Check("fix: expiry at 300/600 stays at 300", s.Current(), 300.0f);
    }
    {
        // Case 5: Fortify Intelligence +100 (i.e. +200 max magicka) mid-combat.
        Sim s{300.0f, -290.0f};  // 10 / 300
        ChangeMax(s, 500.0f, cfg, true);
        Check("fix: fortify Int leaves current untouched", s.Current(), 10.0f);
        ChangeMax(s, 300.0f, cfg, true);
        Check("fix: fortify Int expiry yields no negative pool", s.Current(), 10.0f);
    }
    {
        // Case 6: Drain Magicka -300 (max drops to 0), then expiry.
        Sim s{300.0f, -100.0f};  // 200 / 300
        ChangeMax(s, 0.0f, cfg, true);
        Check("fix: drain -300 clamps current to 0", s.Current(), 0.0f);
        ChangeMax(s, 300.0f, cfg, true);
        Check("fix: drain expiry grants no free magicka", s.Current(), 0.0f);
    }
    {
        // Case 7: pool already negative (inherited from a save) -> floor at 0.
        Sim s{300.0f, -800.0f};  // -500 / 300
        const auto r = mpf::ComputeCompensation(300.0f, 300.0f, s.modifier, cfg);
        Check("fix: negative pool without max change is raised to 0",
              r.apply ? r.newCurrent : s.Current(), 0.0f);
    }
    {
        // Case 8: safety rail (save load or character change).
        const auto r = mpf::ComputeCompensation(300.0f, 1.0e9f, -100.0f, cfg);
        Check("rail: huge delta triggers resync instead of compensation",
              r.resync ? 1.0f : 0.0f, 1.0f);
        Check("rail: no memory write requested", r.apply ? 1.0f : 0.0f, 0.0f);
    }
    {
        // Case 9: "floor only" mode, free magicka deliberately kept.
        mpf::CompensationConfig soft{};
        soft.preserveOnIncrease = false;
        soft.preserveOnDecrease = false;
        Sim s{300.0f, -200.0f};
        ChangeMax(s, 600.0f, soft, true);
        Check("soft: activation keeps free magicka", s.Current(), 400.0f);
        s.modifier -= 300.0f;  // the player spends 300 -> 100 / 600
        Check("soft: after spending, 100/600", s.Current(), 100.0f);
        ChangeMax(s, 300.0f, soft, true);
        Check("soft: expiry floors at 0 instead of -200", s.Current(), 0.0f);
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL TESTS PASS" : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
