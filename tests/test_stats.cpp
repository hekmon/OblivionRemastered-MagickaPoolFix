// Tests for the character data written to the log: change detection between two
// samples, and path anonymisation. Builds and runs outside Windows.
//   g++ -std=c++20 -I../src test_stats.cpp -o test_stats && ./test_stats

#include "PlayerStats.h"

#include <cstdio>
#include <string>

static int g_fail = 0;

static void Check(const std::string& name, const std::string& got, const std::string& want)
{
    const bool ok = got == want;
    if (!ok) ++g_fail;
    std::printf("%-58s got=%-22s want=%-22s %s\n", name.c_str(), got.c_str(),
                want.c_str(), ok ? "OK" : "FAIL");
}

static void Check(const std::string& name, bool got, bool want)
{
    Check(name, std::string(got ? "true" : "false"), std::string(want ? "true" : "false"));
}

static void Check(const std::string& name, int got, int want)
{
    Check(name, std::to_string(got), std::to_string(want));
}

// The test character as last documented: level 33, Int 100, Wil 100,
// Restoration 100, Luck 59 (docs/SPEC.md sections 3.3 and 4.7.3).
static mpf::BaseStats Mage()
{
    mpf::BaseStats s;
    s.level = 33;
    s.health = 300;
    const std::uint8_t attr[mpf::kAttributeCount] = {40, 100, 100, 50, 60, 50, 50, 59};
    for (int i = 0; i < mpf::kAttributeCount; ++i) { s.attribute[i] = attr[i]; }
    for (int i = 0; i < mpf::kSkillCount; ++i) { s.skill[i] = 30; }
    s.skill[13] = 100;  // Restoration
    return s;
}

int main()
{
    std::printf("-- change detection ----------------------------------------------\n");
    {
        const auto a = Mage();
        const auto d = mpf::DiffStats(a, a);
        Check("identical samples: nothing changed", d.Any(), false);
        Check("identical samples: not wholesale", d.wholesale, false);
    }
    {
        // A Remastered level-up: +1 level, up to 3 attributes (UESP).
        auto a = Mage();
        auto b = a;
        b.level = 34;
        b.attribute[5] = 55;   // End +5
        b.attribute[4] = 65;   // Spd +5
        b.attribute[7] = 60;   // Luck +1
        b.health = 305;
        const auto d = mpf::DiffStats(a, b);
        Check("level-up: level changed", d.levelChanged, true);
        Check("level-up: 33 -> 34", std::to_string(d.levelFrom) + "->" +
              std::to_string(d.levelTo), std::string("33->34"));
        Check("level-up: three attributes", static_cast<int>(d.attributes.size()), 3);
        Check("level-up: Luck is one of them", d.attributes.back(), 7);
        Check("level-up: health changed", d.healthChanged, true);
        Check("level-up: is a game event, not a reload", d.wholesale, false);
    }
    {
        auto a = Mage();
        auto b = a;
        b.skill[12] = 31;   // Mysticism
        b.skill[7] = 31;    // Alchemy
        const auto d = mpf::DiffStats(a, b);
        Check("two skill-ups: two skills", static_cast<int>(d.skills.size()), 2);
        Check("two skill-ups: not wholesale", d.wholesale, false);
        Check("two skill-ups: level unchanged", d.levelChanged, false);
    }
    {
        auto a = Mage();
        auto b = a;
        b.readyToLevelUp = true;
        const auto d = mpf::DiffStats(a, b);
        Check("ready-to-level flag alone is reported", d.readyChanged, true);
        Check("ready-to-level flag alone is not wholesale", d.wholesale, false);
    }
    {
        // Loading an older save of the same character: level goes down.
        auto a = Mage();
        auto b = a;
        b.level = 31;
        const auto d = mpf::DiffStats(a, b);
        Check("level down: wholesale (another save)", d.wholesale, true);
    }
    {
        auto a = Mage();
        auto b = a;
        b.level = 35;
        const auto d = mpf::DiffStats(a, b);
        Check("level +2 in one sample: wholesale", d.wholesale, true);
    }
    {
        auto a = Mage();
        auto b = a;
        for (int i = 0; i < 4; ++i) { b.attribute[i] = static_cast<std::uint8_t>(b.attribute[i] - 1); }
        const auto d = mpf::DiffStats(a, b);
        Check("four attributes at once: wholesale", d.wholesale, true);
    }
    {
        auto a = Mage();
        auto b = a;
        b.skill[0] = 29;
        const auto d = mpf::DiffStats(a, b);
        Check("a base skill going down: wholesale", d.wholesale, true);
    }
    {
        auto a = Mage();
        auto b = a;
        b.skill[0] = 40;
        const auto d = mpf::DiffStats(a, b);
        Check("a base skill +10 in one sample: wholesale", d.wholesale, true);
    }

    std::printf("\n-- names and formatting ------------------------------------------\n");
    Check("attribute 1 is Intelligence", std::string(mpf::AttributeName(1)), std::string("Int"));
    Check("attribute 7 is Luck", std::string(mpf::AttributeName(7)), std::string("Luc"));
    Check("skill 13 is Restoration (AV 25)", std::string(mpf::SkillName(13)),
          std::string("Restoration"));
    Check("skill 20 is Speechcraft (AV 32)", std::string(mpf::SkillName(20)),
          std::string("Speechcraft"));
    Check("out of range attribute", std::string(mpf::AttributeName(8)), std::string("?"));
    Check("out of range skill", std::string(mpf::SkillName(21)), std::string("?"));
    Check("mastery 24 = Novice", std::string(mpf::MasteryName(24)), std::string("Novice"));
    Check("mastery 25 = Apprentice", std::string(mpf::MasteryName(25)), std::string("Apprentice"));
    Check("mastery 99 = Expert", std::string(mpf::MasteryName(99)), std::string("Expert"));
    Check("mastery 100 = Master", std::string(mpf::MasteryName(100)), std::string("Master"));
    Check("modifier +12", mpf::FormatModifier(12.0f), std::string("+12"));
    Check("modifier 0", mpf::FormatModifier(0.0f), std::string("+0"));
    Check("modifier -99.55", mpf::FormatModifier(-99.55f), std::string("-99.55"));
    Check("modifier +100.0001 reads as whole", mpf::FormatModifier(100.0001f), std::string("+100"));

    std::printf("\n-- path anonymisation --------------------------------------------\n");
    const std::string prof = "C:\\Users\\Ann";
    Check("profile prefix replaced",
          mpf::AnonymisePath("C:\\Users\\Ann\\Games\\Oblivion\\x.asi", prof),
          std::string("%USERPROFILE%\\Games\\Oblivion\\x.asi"));
    Check("case-insensitive",
          mpf::AnonymisePath("c:\\users\\ANN\\x.asi", prof),
          std::string("%USERPROFILE%\\x.asi"));
    Check("another account is not matched",
          mpf::AnonymisePath("C:\\Users\\Anna\\x.asi", prof),
          std::string("C:\\Users\\Anna\\x.asi"));
    Check("Program Files untouched",
          mpf::AnonymisePath("C:\\Program Files (x86)\\Steam\\x.dll", prof),
          std::string("C:\\Program Files (x86)\\Steam\\x.dll"));
    Check("empty profile changes nothing",
          mpf::AnonymisePath("C:\\Users\\Ann\\x.asi", ""),
          std::string("C:\\Users\\Ann\\x.asi"));
    const std::string game = "D:\\Steam\\steamapps\\common\\Oblivion Remastered\\Win64";
    Check("relative inside the game folder",
          mpf::RelativeTo(game + "\\plugins\\MagickaPoolFix.asi", game),
          std::string("plugins\\MagickaPoolFix.asi"));
    Check("relative: the game folder itself", mpf::RelativeTo(game, game), std::string("."));
    Check("relative: outside stays absolute",
          mpf::RelativeTo("D:\\Steam\\gameoverlayrenderer64.dll", game),
          std::string("D:\\Steam\\gameoverlayrenderer64.dll"));
    Check("relative: sibling folder is outside",
          mpf::RelativeTo(game + "2\\x.dll", game), game + "2\\x.dll");

    std::printf("\n%s\n", g_fail == 0 ? "all stats tests passed" : "SOME STATS TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
