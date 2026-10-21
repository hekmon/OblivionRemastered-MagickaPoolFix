# MagickaPoolFix — Specification

Fix for the semantics of the magicka pool in *The Elder Scrolls IV: Oblivion
Remastered* when maximum magicka changes.

This document is self-contained. It assumes no prior knowledge of the project and
does not refer to any earlier discussion. Every claim is either **measured** on
a specific version of the game, or **deduced** and then explicitly marked
`[DEDUCTION]`, or **sourced** from an external reference. What remains unknown is
grouped in chapter 10 rather than filled in with a guess.

The raw history of the measurements, including abandoned hypotheses, is in
`FIELD-JOURNAL.md`. It is not necessary to read it to understand or implement
what follows.

---

## 1. The problem, as the player experiences it

A mage character constantly uses effects that modify their maximum magicka:
Fortify Intelligence, Fortify Magicka, enchanted rings and amulets,
potions, birthsign, racial abilities. Every activation and every expiry
moves the ceiling of the pool.

Three symptoms follow from this.

**A — Free magicka on activation.** Casting a fortify spell fills the
pool by the amount of the enlargement, with nothing in return. On a well-equipped
character, the effect is large enough that the fortify spell pays for itself and
leaves a net profit. It is a trivial exploit: chaining activation and expiry
generates magicka.

**B — Confiscation on expiry.** When the fortification wears off, the entire
reduction of the maximum is taken out of current magicka, including when the pool
was already far below its ceiling. There was nothing to spill over, and yet the
player loses points they owned.

**C — Negative pool.** Symptom B is unbounded. Current magicka can drop
below zero, and regeneration must then climb back through the entire overdraft before the first
point becomes spendable again. The player sees an empty bar that does not move for
several tens of seconds after a buff expires, with no indication of the cause.

Symptom C is the most punishing in play, and it is the one that makes the character
unusable at the precise moment they have just lost their reinforcements.

## 2. The behavior this mod implements — the glass-of-water model

The pool is a glass. The maximum is the capacity of the glass, current magicka is
the water it contains.

| Action | Expected consequence |
|---|---|
| Enlarge the glass | The amount of water does not change. The glass is simply less full. |
| Shrink the glass, water below the new rim | The amount of water does not change. |
| Shrink the glass, water above the new rim | Only the excess is lost. The glass stays full. |

A glass never contains a negative amount of water.

This model addresses the three symptoms of chapter 1 without introducing any new rule:
there is neither a bonus nor a penalty attached to the change in capacity, only a
geometric consequence.

It departs, on purpose, from what UESP documents for the original game. The Spell Making
page, in its "Spell Chaining" section, describes:

- Fortify Magicka and Fortify Intelligence as adding their magnitude (twice it, for
  Intelligence) to both current and maximum magicka;
- Drain Magicka as removing its magnitude from both;
- on expiry, Fortify Magicka and Drain Magicka as unable to take current magicka below
  zero, while Fortify Intelligence can.

It presents the free magicka as a mechanic players exploit, spell chaining, "allowing the
player to cast the same spell infinitely"
([UESP, Spell Making](https://en.uesp.net/wiki/Oblivion:Spell_Making#Spell_Chaining)).
The glass of water removes that mechanic, for all three effects alike.

Withdrawn: an earlier version of this paragraph said that page describes Fortify Magicka
as *not* adding current magicka, unlike Fortify Intelligence. The page says the opposite.

Whether Remastered keeps the floor at zero for Fortify Magicka and Drain Magicka is
discussed in §10.5. The measurement of §4.1 rules it out for a combined expiry.

**Position.** This document treats the defect of chapter 4 as a bug and the mod as a fix.
The free magicka and the negative pool follow from a deficit the engine never rebases
(§4), and a fix that closes an exploit changes the balance for the players who used it
(§7.1). Whether the original designers meant the side effect is not documented; UESP
describes it as something players exploit. The same position is taken for the two
uncapper mods of §10.7, which this document does not otherwise judge.

## 3. How the engine actually stores magicka

This section states facts measured on the Steam executable
`OblivionRemastered-Win64-Shipping.exe`, PE timestamp `0xF19077A4`. Offsets are
relative to the `PlayerCharacter` instance, whose size is `0xB88` bytes.

### 3.1 Current magicka is not stored

During 24 seconds of passive regeneration — pool visibly filling up, no player
action — the 482 aligned floats in the window
`0x400..0xB88` were sampled continuously. **Only three moved**, and only
one is a magicka quantity.

So there is no "current magicka" field. The engine keeps the **deficit** and
recomputes the current value on demand:

```
current = maximum + deficit          with deficit ≤ 0
```

The deficit is a single-precision float at `+0x06B8`.

### 3.2 The maximum is not stored in the observable window

No float in `0x400..0xB88` contained the value of the maximum, either before or after
a change from 320 to 300 verified on screen. The maximum is therefore **recomputed** from
the attributes, or stored outside this window.

This is the decisive architectural constraint of the mod: **only the *change* in the
maximum is accessible, never its absolute value.** Chapter 5 draws all the
consequences from this.

This value is reachable in theory through the virtual function
`GetActorFloatValue`, at index `0x0B1` of the `PlayerCharacter` virtual table.
In practice this call is unusable: see chapter 9.1.

### 3.3 The deficit is indeed the regeneration field

On a pool that is filling up, the deficit climbs toward zero. Measured at
`−572.45` then `−567.769` on the same load, with no player action.

The measured rate is **12.98 points per second** for an effective Willpower of 110.
It is obtained as the ratio between the changes in the deficit and those of a counter
in seconds located at `+0x08E0`: out of 103 matched sample pairs, 102 fall within
±0.005 of this value. The result agrees to four significant figures with the
relation `2 × (V/50)² + 1.5 × (V/50)` per second, i.e. `9.68 + 3.30 = 12.98`
([UESP, Willpower](https://en.uesp.net/wiki/Oblivion:Willpower)).

Regeneration depends on **Willpower**, not Intelligence. Remastered changed this
mechanic in depth: in the original Oblivion the rate was
`0.01 × (0.75 + 0.02 × Willpower) × total_magicka`, hence **proportional to the size of the
pool**; in Remastered it depends only on Willpower, it is halved in
combat, and it is suspended for 2 seconds after each cast
([UESP, Willpower](https://en.uesp.net/wiki/Oblivion:Willpower), section "Oblivion
Remastered"). The Remastered formula is the one used here, and it is the one the
measurement confirms: the original formula would predict 17.05 /s on a pool of 620, which
the measurement at 12.98 /s rules out.

**UESP contradicts itself on the Remastered formula.** Three pages give three formulas,
here evaluated out of combat at the effective Willpower of 110:

| UESP page | Remastered formula, out of combat | at W = 110 |
|---|---|---:|
| [Willpower](https://en.uesp.net/wiki/Oblivion:Willpower) | `2 × (W/50)² + 1.5 × (W/50)`, halved in combat | **12.98 /s** |
| [Magicka](https://en.uesp.net/wiki/Oblivion:Magicka) | `2.0 × (W × W × 0.0003 + W × 0.015)` | 10.56 /s |
| [Remastered Changes](https://en.uesp.net/wiki/Oblivion:Remastered_Changes) | `W × W × 0.02 + W × 0.015`, "increased 2.0x when out of combat" | 243.65 /s or more |

The measurement, 12.98 /s with 102 of 103 samples within ±0.005, agrees with the
Willpower page only. The Magicka page's formula does not reach it for any plausible
Willpower either (W = 105, 110, 115, 120 give 9.76 to 12.24 /s). The Willpower page is the one used here.

The gap between the Willpower 100 on the character sheet and the **effective Willpower
110** of the measurement comes from a modifier, and **not from vampirism** as an earlier
version of this section said. The same run recorded `+10.0000` on all eight attributes in
the magic modifier array (`+0x0474` to `+0x0490`, `FIELD-JOURNAL.md`, 6 August 2026,
0.4.0-probe). Vampirism fortifies only Strength, Speed and Willpower, by +5, +10, +15 or
+20 depending on the stage ([UESP, Vampirism](https://en.uesp.net/wiki/Oblivion:Vampirism)),
so a +10 on Intelligence, Agility, Endurance, Personality and Luck is something else.
`[DEDUCTION]`: an item or effect fortifying every attribute by 10. **Identified on
21 October 2026**: the ring the game calls "Anneau de perfection" in French, whose item
screen lists Fortify +10 on each of the eight attributes. A vampirism bonus on
top of it would have given Willpower 115 or more and at least 14.03 /s, so none was in
effect during that measurement. Withdrawn: "the +10 corresponds to the 50% stage".

The test character is a vampire, so its rate can still swing with the stage: from 12.98 /s
(Willpower 110, no stage bonus) to 17.42 /s (Willpower 130, +20), by the formula above.
Which stage the character was at on any given day is not in the logs. On 21 October 2026
the Active Effects screen showed Vampirism 50, the 50 % stage, and its +10 appears to sit
in the **base** Willpower the log reads (110 = 100 + 10), not in a modifier (§10.8,
`[DEDUCTION]`).

Practical consequence: an overdraft of 250 points costs 19 seconds of waiting before
the first point of magicka is spendable again.

### 3.4 How to read the change in the maximum

The base maximum follows `maximum = 2 × Intelligence + flat bonuses`, the flat
bonuses coming from race and birthsign
([UESP, Magicka](https://en.uesp.net/wiki/Oblivion:Magicka)). Differentiating, the
flat terms cancel out in any difference and what remains is:

```
Δmaximum = 2 × Δ(Intelligence) + Δ(Magicka)
```

This relation remains valid beyond Intelligence 100, which matters on an installation
equipped with an uncapper mod: the Spell Making page states, about the magicka gain
from Intelligence, that "this is true even when Intelligence surpasses 100"
([UESP, Spell Making](https://en.uesp.net/wiki/Oblivion:Spell_Making)). The coefficient 2 does
not saturate.

Both terms are read from three attribute value modifier arrays,
each of 72 floats indexed by attribute identifier:

| Offset | Modifier source |
|---|---|
| `+0x0474` | magic effects |
| `+0x0594` | scripts |
| `+0x06C0` | game engine |

The total for an attribute is the sum of the three arrays at its index. The useful
indices are `1` for Intelligence and `9` for Magicka. The full order is Strength 0,
Intelligence 1, Willpower 2, Agility 3, Speed 4, Endurance 5, Personality 6, Luck 7,
Health 8, Magicka 9, Fatigue 10, Encumbrance 11.

The four offsets above are `[DEDUCTION]`: they come from the
`PlayerCharacter` layout published by
[commonlibob64](https://github.com/libxse/commonlibob64/blob/main/include/RE/P/PlayerCharacter.h),
not from a debug symbol. They have been confirmed empirically: `+0x06B8`
behaves as described in 3.1 and 3.3, and the formula above predicted two independent
maximum changes to the hundredth (chapter 4).

The array at `+0x0474` was also seen holding `+5.0` on indices 21 to 25,
corresponding to the five fortified magic schools of the test character, and `−99.55` on
index 11 under the effect of a Feather spell. These side values confirm
the indexing.

## 4. The defect, stated formally

The deficit at `+0x06B8` is a **raw counter of spent magicka**. The engine never
rebases it when the maximum changes, and `current = maximum + deficit` is evaluated
**without any bound**.

It is a single defect. Symptoms A, B and C of chapter 1 are three
manifestations of it, not three bugs.

### 4.1 Direct proof of symptom C

Observation of 6 August 2026, 17:03:11. The character has a Fortify
Intelligence 100 and a Fortify Magicka 100 expire simultaneously.

| Quantity | Before | After |
|---|---|---|
| Intelligence modifier total | 110.0000 | 10.0000 |
| Magicka modifier total | 100.0000 | 0.0000 |
| Predicted Δmaximum | | `2 × (−100) + (−100) = −300.0000` |
| Deficit at `+0x06B8` | −567.7690 | −567.7690 |
| Maximum | 620 | 320 |
| **Current** | **52.23** | **−247.77** |

The deficit did not move by even a ten-thousandth. The pool was at 52 points out of 620, so
there was no excess to evacuate, and yet the 300 points were taken
in full. Current magicka went negative.

### 4.2 Proof of symptom A

Observation of 6 August 2026, 17:02:15, pool full at 320/320. The player casts a
fortify spell costing 270 that raises the maximum by 300.

| Quantity | Value |
|---|---|
| Predicted Δmaximum | `+300.0000` |
| Deficit before | `0.0000` |
| Deficit after | `−270.0000` |
| Deficit under preservation | `0 − 300 − 270 = −570` |

`[DEDUCTION]` based on the known cost of the spell and on the exact value `−270.0000`:
the only compatible decomposition is **zero compensation** plus payment of the spell. The
alternative decomposition, a cost of 570 with a compensation of `+300`, is ruled out
since a character with a maximum of 320 cannot pay 570.

The actual sequence is therefore: the maximum goes to 620, the deficit stays at 0 so the
pool is **filled for free** up to 620, then the 270 is debited. The player
had 320 before casting a spell costing 270; they have 350 after. Net profit of 30
points.

This measurement is structurally confounded: a fortify spell cast on oneself is
paid in the same transition as the one that raises the maximum. Chapter 10.1 describes
the test that would remove the last doubt.

### 4.3 The case that is already correct

Observation of 6 August 2026, 17:16:16, pool full, removal of a `+10`
Intelligence ring.

| Quantity | Value |
|---|---|
| Δ Intelligence modifier total | `−10.0000` |
| Predicted Δmaximum | `2 × (−10) = −20.0000` |
| Maximum observed on screen | 320 → 300 |
| Deficit | `0.0000` → `0.0000` |

Zero deficit before, so pool full; zero deficit after, so pool still full
at the new maximum. That is exactly the excess that had to be lost: **this case is
correct and must not be touched.** A correction that preserved the current value here
would leave 320 points of water in a glass of 300.

This observation also validates the formula of 3.4 on a zero-cost change, therefore
without a confounder, and at a magnitude differing by a factor of 15.

### 4.4 Overall conservation, and why it saves nothing

Over a complete round trip, the arithmetic is conservative: the points gained
for free on activation are exactly those taken back on expiry. The defect
is not a magicka leak.

The damage is that **the intermediate state allows an overdraft**. The player spends
magicka that exists only thanks to the enlargement of the glass, then the glass shrinks and
carries the debt into the negative. This is why correcting only the expiry would be
a mistake: it would turn a conservative exploit into a net magicka leak.

**Both halves must be corrected together or not at all.**

### 4.5 Increase on a pool that is not full

6 August 2026, 17:58:23. `Δ = +300` on a pool 40.2146 below its maximum of
320. The gift is still worth 300, not 40.2146: the pool does not merely fill
up to the new maximum; the engine moves the maximum under a pool that it does not
rebase. The competing explanation "it was full, it just filled up" is
eliminated.

### 4.6 In-game validation of the write path

6 August 2026, 18:44 to 18:50, version 0.9.1 with `bDryRun = 0`. Three
fortify / expiry cycles, six transitions, **six writes read back and confirmed, zero
failures**. Base maximum 320, fortified 620.

| # | event | max | current before | left by the engine | after write |
|---|-------|-----|----------------|--------------------|-------------|
| 1 | cast   18:44:16 | 320→620 | 320.0000 | 350.0000 | 50.0000 |
| 2 | expire 18:45:11 | 620→320 | 323.9062 | 24.1011 | 320.0000 |
| 3 | cast   18:45:21 | 320→620 | 320.0000 | 350.0000 | 50.0000 |
| 4 | expire 18:46:16 | 620→320 | 153.8036 | **−145.9885** | 154.0115 |
| 5 | cast   18:48:45 | 320→620 | 25.4453 | 325.4453 | 25.4453 |
| 6 | expire 18:49:40 | 620→320 | 133.6403 | **−166.3597** | 133.6403 |

The glass-of-water model holds to within a single frame of drift. On expiries 4
and 6 the absolute value is preserved to `+0.2079` and `−0.0000` — the first gap is
regeneration occurring in the same tick, not a calculation error. Event 2 loses
3.9062 points, and that is correct: 323.9062 does not fit into a glass of 320;
capping at `min(d − Δ, 0)` is what the model of chapter 2 prescribes.

On casts 1 and 3, the gap of `−270.0000` is the **spell cost**, not drift.
Fourth and fifth independent confirmations of this cost.

Event 5 is instructive. `Δd = 0`, hence the verdict `FREE MAGICKA` and not `PARTIAL`:
the spell cost had been applied by the engine in an earlier poll, not in
the one that saw the maximum move. **The correction gives the same result in both
cases** — 25.4453 preserved exactly — because it starts from the deficit as it stands after
the engine has had its say, without ever needing to know what the engine has just done.
This is the property the rule of 5.3.3 was meant to have, measured.

The two negative values in bold are the bare bug: without the correction, the player's pool
would have dropped below zero and they would have waited, respectively, about 13 and 15 seconds of
invisible regeneration before their bar moved.

Four unambiguous transitions were observed, four in favor of "the engine
ignores", **zero contradictions** with the shipped default `iEnginePolicy = 1` (5.3.5).

What this session does not cover: enchanted items, potions, drain,
reloading a save, and level-up. See chapter 10.

### 4.7 Long session of 6 August 2026, 18:30 to 23:08

Version 0.9.1, `bDryRun = 0`, 4 h 37 min of continuous play, **938,783 polls**. Sixteen
transitions, fourteen writes, **zero failures, zero error lines, zero crashes**. The
two transitions without a write are the two `OVERFLOW SPILLED` of events 8 and 9, where
the rule correctly decides that there is nothing to do.

Base maximum 320 with its +10 Intelligence ring, 300 without.

| # | time | Δmax | before | left by the engine | after | note |
|---|---|------|-------|----------------------|-------|------|
| 7 | 21:01:29 | +300 | 320.00 | 350.00 | 52.00 | cost 270 |
| 8 | 21:02:55 | −300 | 320.00 | 320.00 | — | pool full, correct overflow |
| 9 | 21:26:09 | −20 | 300.00 | 300.00 | — | **ring removed**, pool full |
| 10 | 21:28:18 | +20 | 23.5872 | 43.5872 | 23.5872 | **ring put back on**, pool empty |
| 11 | 22:11:39 | +300 | 320.00 | 352.00 | 52.00 | cost 268 |
| 12 | 22:12:59 | −300 | 543.3721 | 243.6843 | 320.0000 | capped, see below |
| 13 | 22:13:52 | +300 | 285.7708 | 317.7708 | 17.7709 | cost 268 |
| 14 | 22:15:56 | −300 | 173.9605 | **−126.0192** | 173.9808 | +0.0203 of regen |
| 15 | 22:19:11 | +300 | 318.6917 | 350.6917 | 50.6917 | cost 268 |
| 16 | 22:20:25 | −300 | 96.6643 | **−202.9829** | 97.0171 | +0.3528 of regen |

Over the whole session, four expiries would have pushed the pool **below zero**, down to
−202.98, i.e. **641.35 points** of cumulative overdraft. Converted to waiting time with the
Remastered formula of §3.3, out of combat:

| Effective Willpower | Regeneration | Wait eliminated |
|---:|---:|---:|
| 100 | 11.00 /s | 58.3 s |
| 110 (value measured in §3.3) | 12.98 /s | 49.4 s |
| 120 | 15.12 /s | 42.4 s |

Double these values in combat, where regeneration is halved. On the side of
increases, 2,120 points of free magicka were refused.

#### 4.7.1 Enchanted items, first measured case

Events 9 and 10 are the first measurement on an **enchanted item**, a case listed as
untested until now. The ring is worth `Δmax = ±20 (= 2 × ±10 + 0)`, consistent with §3.4.

Removed with the pool full: overflow, no write, correct. Put back on with the pool nearly empty:
`Δd = 0`, free magicka, correction of −20 applied, current preserved at 23.5872 exactly.
The mechanism is therefore **the same as for a spell**: the engine moves the maximum and never
rebases the deficit, whatever the source of the change.

#### 4.7.2 The deliberate loss in event 12

Event 12 takes the player from 543.3721 to 320.0000, i.e. 223.37 points lost. This
is not a regression, it is the glass-of-water model applied literally: 543 does not
fit into a glass of 320, and `min(d − Δ, 0)` caps.

The uncorrected engine would have given 243.6843. **The mod therefore gives the player 76.32 more points
than the base game**, while appearing to take some away. It is the only case where the mod's output
reads badly without the frame of reference, and it is worth knowing before receiving a bug
report about it.

#### 4.7.3 The spell cost changed mid-session

The deficit shift on cast is exactly `270.0000` at events 1, 3 and 7,
then exactly `268.0000` at events 11, 13 and 15. The magnitudes are identical
(`+300 = 2 × +100 + 100`), so it is not a spell of a different magnitude.

Both readings are exact to four decimal places because Remastered imposes a
2-second delay before regeneration resumes after a cast
([UESP, Willpower](https://en.uesp.net/wiki/Oblivion:Willpower)): at the moment of the cast,
regeneration is stopped, so the deficit shift is the pure cost, without
contamination. This is what makes the 270 / 268 comparison usable.

**The vanilla formula does not apply on this installation.** The test rig loads
[Oblivion Damage Formula And Magicka Cost Skills Scaling Uncapper](https://www.nexusmods.com/oblivionremastered/mods/1846)
by Baestus (§10.7), whose source code is available. It does not merely lift a
cap: it **replaces the cost function** via a MinHook hook and **patches the function
`luck_skill_modifier`** by overwriting the instruction `cmovbe rax, rcx` (`48 0F 46 C1`) with
`mov rax, rcx ; nop` (`48 89 C8 90`), which removes the cap at 100 for *all* callers
of this function, permanently, with no restoration on unload.

The formula actually executed is therefore, verbatim from `hkMagickaCostFormula`:

```c
float ModifiedSkill = g_luck_calculator(Skill, Luck);  // cap at 100 removed
if (ModifiedSkill > 100.0f)
    cost = (Base_Cost * fMagicCasterSkillCostBase) * (100.0f / ModifiedSkill);
else
    cost = ((1.0f - ModifiedSkill / 100.0f) * fMagicCasterSkillCostMult
            + fMagicCasterSkillCostBase) * Base_Cost;
```

The `<= 100` branch reproduces vanilla exactly: with `fMagicCasterSkillCostBase = 0.2`
and `fMagicCasterSkillCostMult = 1.2`, it equals `1.4 − 0.012 × S`. The `> 100` branch is
**hyperbolic**: `0.2 × Base × 100 / S`. It decreases toward zero without ever reaching it and
cannot become negative.

Restoration 100 with Luck 59 gives `S = 100 + 0.4 × (59 − 50) = 103.6`, hence the
hyperbolic branch and a factor of **0.19305** instead of the vanilla floor of 0.2.

**Resolution.** On this branch the cost is proportional to `1/S`. Going from 270 to 268
therefore requires `S` to rise by `270/268 − 1 = 0.746%`, i.e. **+0.773 points** of effective
skill. Luck contributes exactly 0.4 point per point, so **+2 Luck contributes
+0.8** — the only available integer quantity that falls within the window:

| Luck change | S | Compatible base cost | Window width |
|---|---:|---|---:|
| 59 → 60 | 103.6 → 104.0 | [1396.01, 1396.20) | 0.19 |
| 59 → **61** | 103.6 → **104.4** | **[1396.35, 1401.19)** | **4.84** |
| 59 → 62 and beyond | ≥ 104.8 | none | — |

With a base cost of 1398.8, the installed formula gives 270.033 then 267.964, which
round to **270** then **268**. The +1 Luck solution is not strictly
ruled out but exists only in a window 25 times narrower.

`[DEDUCTION]`: the character's Luck went from 59 to 61 between 21:01 and 22:11, through
level-up, altar blessing, potion or an item fortifying Luck. It is not
a change of spell. The spell's base cost is about 1399.

One constraint narrows the sources: in Remastered, Luck rises by at most 1 point per
level-up and costs 4 virtues
([UESP, Remastered Leveling](https://en.uesp.net/wiki/Oblivion:Remastered_Leveling)). So
+2 in 70 minutes needs two level-ups, or a level-up and another source, or a source other
than levelling. Not yet confirmed by the player: the current Luck has to be read in game.

**Three earlier claims in this document are withdrawn:**

| Withdrawn claim | Why it was wrong |
|---|---|
| "two distinct spells, deduced from the variability of the durations 55 / 86 / 80 / 124 / 74 s" | a recast during the buff emits no event; all these gaps are compatible with a single 55 s spell being recast |
| "effective skill is capped at 100, the cost is at its floor, so only a different base cost remains possible" | the cap is indeed documented for vanilla ([UESP, Spell Making](https://en.uesp.net/wiki/Oblivion:Spell_Making)) but mod 1846 removes it with a binary patch. The base costs 1350 and 1340 that followed from it are wrong |
| "at effective skill ≥ 116.67 spells would become free, which is not observed, so the cap is active" | doubly invalid reasoning: the cap is not active, and the mod's `> 100` branch is hyperbolic, so no zero or negative cost is reachable whatever the skill. The mod deliberately works around this problem |

**Correction, 21 October 2026: the effective skill above ignored a fortify.** The
resolution assumes `S = 100 + 0.4 × (Luck − 50)`, that is, Restoration 100 with no
modifier. But the field dump of §3.4 shows `+5.0` on indices 21 to 25, Restoration
included, and the session of 28 August (§4.8) cannot be fitted without that `+5`. The
ratio argument still gives +2 Luck between the two groups of casts, so the Luck deduction
stands. The base cost does not: it is **about 1460–1470**, not 1399. Fitting all four
costs measured so far (270, 268, 266, 278) with the installed formula, and allowing the
Restoration modifier to be 0 or +5, gives a single pattern:

- Restoration +5 during every cast of 6 August and the first casts of 28 August, gone
  after 21:29 on 28 August;
- Luck 59 → 61 (or 62) → 63 (or 64);
- base cost 1460.7 to 1471.5, depending on the Luck path and on whether the game rounds,
  floors or ceils the cost.

With the modifier at 0 throughout, as the resolution above assumed, no base cost fits
all four. `[DEDUCTION]` resting on three assumptions: the same spell every time
(`Δmax = +300` each time, except one cast at 95 % magnitude, §4.8); base Restoration 100;
and the skill passed to the cost formula including Fortify Restoration. The character
block of the 1.0.0-dev log (§10.8) shows both the Luck and the Restoration modifier, so
the first session with it can check this.

**Checked 21 October 2026, partly.** The character block (§10.8) shows base Luck **63**
and a Restoration modifier of **+0**, and the same spell cost **278** (§4.9). Both agree
with the fit: with `S = 100 + 0.4 × (63 − 50) = 105.2`, a cost of 278 gives a base cost of
about **1462** (1459.7 to 1464.9 if the game rounds), inside 1460.7 to 1471.5.

One question is open. The ring of §3.3 adds **+10 Luck**, and it was worn for that cast.
With the Luck the character sheet shows, 73, `S = 109.2` and the base cost would be about
1518, outside the fit. Either the cost formula takes Luck **without** its Fortify, or the
fit's anchor (Luck 59 on 6 August) was itself read with the ring on, which would shift
every Luck in the fit by 10. The second would need base Luck 53 on 28 August and 63 by
21 October, ten points in four levels at most one per level (UESP, Remastered Leveling),
so it is unlikely. `[DEDUCTION]`, to be settled by one cast with the ring off: same cost
means Luck's Fortify does not count.

The Restoration +5 of 6 August and early 28 August has a likely source: the character
owns a "Boucle pentamagique" (French game text, "pentamagic"), and the dump of §3.4 had
`+5.0` on exactly five actor values, 21 to 25, Conjuration to Restoration, the five
schools that fit the name. `[DEDUCTION]` from the name alone; its item screen has not been
seen.

This does not affect the correction, which never reads the spell cost.

#### 4.7.4 Stability

938,783 reads and 14 writes into a live game object, over 4 h 37 min, without a crash.
56.5 polls per second for 62.5 requested, the gap being the granularity of `Sleep`.
One instance loss at 23:07:40 on a save load, recovered by a scan.
Three re-primings on float bursts, two of them at 21:01. `lost = 0`, `rescans = 4`,
960 log lines against a cap of 200,000.

This is not proof of the absence of memory bugs; it is a large sample without
incident.

### 4.8 Session of 28 August 2026 (0.9.1, analysed 21 October 2026)

1 h 37 min, 21:02 to 22:39. The log stayed on the test machine until 21 October and was
analysed then; the player's own notes from that session were not kept, so every cause in
the table below is read from the numbers alone. 17 transitions, 10 writes, **0 write
failures, 0 error lines, no crash**. The 7 transitions without a write are all
`OVERFLOW SPILLED`: maximum fell while the pool was full, and doing nothing is right.

The Intelligence modifier total sits at **12** between buffs, not 10 as on 6 August:
something besides the +10 ring adds 2, and from 22:08 on it is 11.

| # | time | Int modifier | Magicka modifier | Δmax | deficit before | engine left (Δd) | written | verdict | cause `[DEDUCTION]` |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 21:13:16 | 12 → 112 | 0 → 100 | +300 | 0 | −266 (−266) | −566 | PARTIAL | the +300 spell, cost 266 |
| 2 | 21:14:15 | 112 → 12 | 100 → 0 | −300 | 0 | 0 | — | SPILLED | its expiry, pool full |
| 3 | 21:15:16 | 12 → 112 | 0 → 100 | +300 | 0 | −266 (−266) | −566 | PARTIAL | cost 266 |
| 4 | 21:16:14 | 112 → 12 | 100 → 0 | −300 | 0 | 0 | — | SPILLED | expiry, pool full |
| 5 | 21:29:09 | 12 → 2 | 0 | −20 | 0 | 0 | — | SPILLED | an Int +10 item removed, pool full |
| 6 | 21:29:10 | 2 → 12 | 0 | +20 | 0 | 0 (0) | −20 | FREE MAGICKA | an Int +10 item equipped 0.7 s later |
| 7 | 21:33:54 | 12 → 112 | 0 → 100 | +300 | 0 | −278 (−278) | −578 | PARTIAL | cost 278 |
| 8 | 21:34:59 | 112 → 12 | 100 → 0 | −300 | 0 | 0 | — | SPILLED | expiry, pool full |
| 9 | 21:35:00 | 12 → 112 | 0 → 100 | +300 | 0 | −278 (−278) | −578 | PARTIAL | recast 1.7 s after expiry, cost 278 |
| 10 | 21:36:17 | 112 → 12 | 100 → 0 | −300 | 0 | 0 | — | SPILLED | expiry, pool full |
| 11 | 21:37:40 | 12 → 107 | 0 → 95 | +285 | −6.06 | −284.06 (−278) | −569.06 | PARTIAL | **95 % magnitude**: armor worn (§10.4), cost unchanged at 278 |
| 12 | 21:38:43 | 107 → 12 | 95 → 0 | −285 | 0 | 0 | — | SPILLED | expiry, pool full |
| 13 | 22:08:54 | 12 → 11 | 0 | −2 | −261.75 | −261.61 (+0.14) | −259.61 | PARTIAL | Intelligence −1, lasting to the end of the log |
| 14 | 22:33:50 | 11 → 111 | 0 → 100 | +300 | −7.28 | −285.28 (−278) | −585.28 | PARTIAL | cost 278, pool not full |
| 15 | 22:34:24 | 111 | 100 → 78 | −22 | −356.92 | −356.90 (+0.02) | −334.90 | **MAGICKA STOLEN** | a negative Magicka effect of 22 starts |
| 16 | 22:34:45 | 111 → 11 | 78 → −22 | −300 | 0 | 0 | — | SPILLED | the +300 spell expires, the −22 remains |
| 17 | 22:35:13 | 11 | −22 → 0 | +22 | −129.32 | −129.06 (+0.26) | −151.06 | FREE MAGICKA | the −22 effect ends, 49 s after it began |

What this session adds:

- **The cost moved from 266 to 278** across the item swap of events 5 and 6. Fitted in
  §4.7.3: the item removed carried the Restoration +5 that the cost formula needs, and the
  one put back on did not. `[DEDUCTION]`
- **Armor, measured.** Event 11 is the same spell at 95 % magnitude (`+285 = 2 × 95 +
  95`), exactly the effectiveness cap with any armor (§10.4). Its cost is unchanged at
  278, consistent with effectiveness scaling the magnitude and not the cost.
- **A drain, measured.** Events 15 and 17 are a negative Magicka modifier of 22 that
  lasted 49 s while the Fortify Magicka 100 was active. `[DEDUCTION]`: a Drain Magicka 22
  cast on the player. The regeneration between events 14 and 15, 228 points in 34 s or
  6.7 /s, is about half the 12.98 /s of §3.3, which fits being in combat (§3.3: halved).
  The engine moved the maximum and left the deficit alone, as for every other change: the
  vanilla game took 22 points of current magicka at the start and gave them back at the
  end. The mod cancelled both, so **with the fix this drain cost the player nothing**.
  Whether that is right is a design question, open in §10.5.
- **An Intelligence −1 that does not end** (event 13). `[DEDUCTION]`: a Damage
  Intelligence, which lasts until restored
  ([UESP, Magical Effects](https://en.uesp.net/wiki/Oblivion:Magical_Effects#Drain_vs._Damage_vs._Absorb) contrasts it with Drain). The
  `PARTIAL` verdict comes from regeneration within the same 16 ms poll (+0.14) exceeding
  the tolerance of a 2-point change (§5.4); the decision does not depend on the verdict
  and current magicka was preserved.
- **Calibration**: events 15 and 17 are cost-free changes and were counted as evidence for
  "the engine ignores", reaching 3 for and 0 against, consistent with the pinned default.
- Unexplained: between events 15 and 16 the pool went from −334.90 to full in 21.7 s,
  faster than regeneration at the measured rate. A restore effect is the obvious candidate;
  nothing in the log shows it.

Not covered by this session, despite the 17 transitions: every expiry happened with the
pool full, and the item swap also happened with the pool full. Scenarios 5 and 6 of
chapter 8 remain open.

Read again on 21 October 2026, with the character's equipment seen in game (§4.9):

- The **+2 Intelligence** besides the ring is the Scales of Pitiless Justice, carried:
  Strength, Agility and Intelligence +2, Personality −2, applied by script while they are
  in the inventory ([UESP, Scales of Pitiless Justice](https://en.uesp.net/wiki/Oblivion:Scales_of_Pitiless_Justice)).
- Events 5 and 6 fit a ring swap with both ring slots full, `[DEDUCTION]`: putting on the
  Eidolon ring (Blade and Block +15, no Intelligence) pushed out the ring of perfection
  (event 5); putting that one back pushed out the "Boucle pentamagique", taking the
  Restoration +5 with it, which no transition shows because Restoration does not move the
  maximum. That is the 266 → 278 of the cost.

### 4.9 Sessions of 21 October 2026 (1.0.0-dev)

Two short sessions on the test bench, now with OblivionRemasteredPerformanceFix (§10.7).
First line both times: `ACTIVE`, PE `0xF19077A4 (validated build)`, `conf found`, no
setting away from its default. Player found 41 s and 11 s after start. Both logs end on
"player instance ... no longer valid" when the player quit; the game closed before the
next heartbeat, so nothing followed. That message named only a save load or a moved
object; it now also names a quit and the game closing.

Session 1, 7 minutes, three transitions, all with the pool full:

| # | time | Int modifier | Magicka modifier | Δmax | engine left (Δd) | written | verdict | cause |
|---|---|---|---|---|---|---|---|---|
| 1 | 20:51:01 | 11 → 1 | 0 | −20 | 0 | — | SPILLED | ring of perfection removed |
| 2 | 20:51:02 | 1 → 11 | 0 | +20 | 0 (0) | −20 | FREE MAGICKA | put back 1.25 s later |
| 3 | 20:53:51 | 11 → 111 | 0 → 100 | +300 | −278 (−278) | −578 | PARTIAL | the +300 spell, cost 278 |

All three decisions are the rule of chapter 5. The ring round trip at a full pool cost
20 points, the water above the new rim (§5): intended, and regenerated in seconds.

Session 2 loaded the quick save of session 1 with the spell still running, and saw no
transition. Its character block, taken with that save, was compared field by field with
15 screenshots of the character sheet, the Active Effects list and the item screens:
§10.8.

The spell is Fortify Intelligence, Willpower and Magicka 100: the Willpower modifier goes
from +59 to +159 with it, and Active Effects lists all three.

## 5. The correction rule

Let `d` be the deficit, `m` the max, `m'` the new max, `Δ = m' − m`.

```
d' = min(d − Δ, 0)
```

Three cases, which cover the table in chapter 2:

| Situation | Result | Effect |
|---|---|---|
| `Δ > 0` | `d' = d − Δ` | The current does not move. No more free magicka. |
| `Δ < 0`, no excess | `d' = d − Δ` | The current does not move. No more confiscation. |
| `Δ < 0`, excess | `d − Δ > 0`, clamped to 0 | Pool full at the new max. Excess lost. |

### 5.1 Why this form is the only implementable one

The formulation is **relative**: it uses only `Δ` and `d`, never `m` or `m'`.
This is forced by §3.2, where the absolute max turned out to be inaccessible.

An **absolute** formulation — remembering the current `c` from one tick to the next, then setting
`d' = min(c, m') − m'` — would be more robust in principle, but it is inapplicable on two
counts:

1. it requires `m'`, which is not readable;
2. at a 16 ms sampling period, paying for a spell and the change of max fall in the
   same tick. Rebuilding the deficit from a remembered current would erase the spell cost
   and refund it.

The relative form, on the contrary, preserves any concurrent expenditure, since it starts
from the deficit that has already been debited.

Clamping to 0 can be expressed without knowing `m'`: `d − Δ > 0` means exactly
"the current would exceed the new rim". That is what makes the third case
implementable.

### 5.2 The floor at zero, and why it is almost always useless

A floor `d' ≥ −m'` would guarantee a positive current, but requires `m'`. It is in fact
**redundant**: if the current was positive before, `d ≥ −m`, so

```
d − Δ = d − m' + m ≥ −m − m' + m = −m'
```

The correction therefore cannot itself produce a negative current. The floor would only
bite on an **already corrupted** state, that is, a save loaded with a current already
below zero because of the uncorrected bug.

Decision: the floor stays out of the initial scope. Repairing an already corrupted save is
a separate feature, which requires `m'` and therefore the unblocking of chapter 10.2. The
corresponding parameter is documented as inoperative as long as this condition is not
lifted, rather than removed.

### 5.3 Calibrating the engine policy

The relative form has a weak point: if the engine already compensated on its own side,
the mod's subtraction would add to the engine's and would steal magicka. The mod therefore
has to know which of the two policies the engine applies.

The measurements in chapter 4 establish that this engine does not compensate. That remains
a property observed on one version, not a contractual guarantee: an official patch can
change it. The mod must determine it by itself at run time.

#### 5.3.1 Why the transition to be corrected cannot decide by itself

The obvious test — looking at how the deficit moved during that transition — fails on the
most frequent case. A fortification cast **on oneself** is debited in the very frame that
raises the max, so the observed movement is

    Δd = −(spell cost) + (possible compensation)

that is, one equation with two unknowns. The measured case shows it: `Δd = −270` with
`Δ = +300`. That number is consistent with "no compensation, spell costing 270" and,
arithmetically, with other splits.

A guard that required `|Δd| ≤ tolerance` before acting would therefore refuse to correct
precisely symptom A, which is the mod's reason for existing.

#### 5.3.2 Calibrating on unambiguous transitions

The engine policy is a property of the game version, not of the moment. It therefore does
not have to be decided from the transition being corrected. Some transitions are
unambiguous, because nothing is debited at the moment they occur:

- the **expiry** of a fortification costs nothing;
- **equipping or unequipping** an enchanted item costs nothing;
- **drinking a potion** costs nothing.

On those, `Δd` is pure engine behavior. The mod observes them, deduces the policy from
them once, then applies it to **all** transitions, including ambiguous ones.

Let `c = min(d − Δ, 0)` be the expected correct value (§5.1). For a transition:

- if `|d_after − d_before| ≤ tolerance` **and** `|c − d_before| > tolerance`: the engine
  did nothing when it should have → evidence of `kEngineIgnores`;
- if `|d_after − c| ≤ tolerance` **and** `|c − d_before| > tolerance`: the engine reached
  the right value → evidence of `kEngineHandles`;
- otherwise: uninformative, no evidence counted.

The second condition of each of the first two rules is not decorative. Without it, a
transition where **doing nothing is precisely the correct behavior** would be counted as
evidence of a negligent engine. That is exactly the shape of a decrease on a full pool
(§4.3): `d_before = 0`, `c = min(0 − (−20), 0) = 0`, and a correct engine and a negligent
engine both leave the deficit at zero. This is the main trap of this chapter, and it is
covered by a named unit test.

#### 5.3.3 Per-transition decision

Once the policy is known:

- `kEngineHandles` → never write;
- `kEngineIgnores` → apply rule §5.1 to all transitions;
- `kUnknown` → apply only the unambiguous transitions, that is, those where the deficit
  did not move, and abstain on the others, saying so in the log.

The `kUnknown` case is only reachable with `iEnginePolicy = 0`, the shipped value being
`1` (see §5.3.4).

Contradictory evidence (`kEngineIgnores` and `kEngineHandles` both observed) is treated as
`kUnknown`, not resolved by majority: two transitions that contradict each other mean the
model is wrong somewhere, and the safe answer to a wrong model is to stop correcting.

The evidence is **kept** across save reloads and re-primings. Up to 0.9.2 it was reset
on every reload, on the grounds that a save can in principle run under a different game
version. That was wrong and was withdrawn in 0.9.3: the evidence describes whether **this
executable** rebases the deficit, a property of the binary that loading a save cannot
change, and the reset only delayed the one warning that matters (`FIELD-JOURNAL.md`, long
session of 6 August 2026).

#### 5.3.4 Automatic calibration is not the shipped default

`iEnginePolicy` ships at **1**: assume that the engine ignores the change, and correct from
the first frame.

This is not a shortcut, it is the conclusion of chapter 4. The measurement has been made,
on `OblivionRemastered-Win64-Shipping.exe` with PE timestamp `0xF19077A4`. Making every
player re-derive this measurement has a real and immediate cost: with `iEnginePolicy = 0`,
the mod does **nothing** until the first unambiguous transition, which includes the first
self-cast fortification of every session — that is, precisely the most common way of
running into the defect.

The reasoning adopted: a mod that works right away for everyone is better than a mod that
starts by proving the bug to itself. A mod breaking after a game update is a normal and
accepted event in this community, and chapter 7 gives the advanced player what they need
to diagnose and repair without waiting for a release from us.

#### 5.3.5 Observation continues anyway

Pinning the policy decides what the mod **does**. It does not authorize it to stop
looking.

The observation of §5.3.2 runs whatever the value of `iEnginePolicy`, and every
unambiguous transition is compared with the pinned value. A divergence produces a named
error block in the log.

This is the only scenario in which this mod can **do harm** instead of being merely
redundant: if an official patch rebases the deficit itself, the mod's subtraction adds to
the engine's and steals magicka. Without observation, this would happen silently.

The mod does not stop correcting on its own in this case. A single abnormal transition is
not proof, and stopping silently would be worse than saying so. The log sets out the
finding and the two ways out — `iEnginePolicy = 2` to stop correcting,
`iEnginePolicy = 0` to let the mod decide on its own.

#### 5.3.6 Refuting compensation from an ambiguous transition

On an increase, the ambiguity of §5.3.1 can sometimes be resolved on its own. With `c` the
compensation applied by the engine and `k` the spell cost:

    Δd = −c − k     so     k = −Δd − c

and `k ≥ 0`, because no spell refunds magicka. Displaying `k` under each of the two
candidate values of `c` makes the transition decidable as soon as only one of the two is
positive.

Measurement of 6 August 2026 at 17:58:23: `Δd = −270`, `Δ = +300`. Without compensation,
`k = 270`. With full compensation, `k = −30`, i.e. a spell that refunds: impossible.

Two caveats. This only refutes `c > −Δd`, so not a **partial** rebase, which the binary
model of this chapter does not contemplate. And it assumes `k ≥ 0`, which a Restore
Magicka potion drunk within the same 16 ms frame would violate.

The mod **logs** both implied costs at every event, which documents the reasoning where
the player reads it. It draws no automatic conclusion from them: with the default of
§5.3.4 the question does not arise, and with `iEnginePolicy = 0` the gain would be limited
to a single transition per session.

### 5.4 Tolerance

The comparison cannot be exact. The quantities are single-precision floats, and sampling
introduces an offset of up to one frame.

Tolerance adopted: `ε + 0.02 × |Δ|`, with `ε = 0.01` by default.

The proportional term is necessary: a Fortify Intelligence 110 moves the max by 220
points, and a fixed window of one hundredth would classify any large fortification as
"unexpected behavior", and hence leave the mod permanently inactive. Conversely, the constant term keeps a
2-point variation judged to within a hundredth.

## 6. Architecture

### 6.1 Delivery form: ASI, not OBSE

The mod is an `.asi`, loaded by
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) via
`version.dll`. This is not an aesthetic choice: OBSE-style script extenders turned out to
be unstable on this installation, with frequent crashes. An `.asi` is an ordinary Windows
DLL loaded by the loader, with no dependency on a script runtime.

Point of attention, a frequent source of error: most plugins published for this game have
the `.dll` extension **because they target OBSE**. An ASI plugin has the `.asi`
extension. The two are not interchangeable.

Installation location:

```
…\Oblivion Remastered\OblivionRemastered\Binaries\Win64\plugins\
```

The configuration file and the log go **next to the `.asi`**, in that same folder, and not
in `Documents` or `%APPDATA%`.

### 6.2 No hook

The mod installs **no** virtual table detour and replaces no function. It discovers the
`PlayerCharacter` instance, reads the four fields of chapter 3 in a loop, and writes only
the float `+0x06B8`. Versions before 0.9.1 only observed.

This choice follows from §3.2 and §9.1: the virtual call that would give the max kills the
process. Everything the mod does must fit within raw memory reads and writes from its own
thread.

Consequence to be accepted: the mod observes the effect of the max change **after** the
engine has applied it, with a delay of up to one sampling period. It therefore corrects
after the fact. Over a 16 ms period the player cannot exploit the window, but it exists.

### 6.3 Observation loop

- Dedicated thread, created from an initialization thread and not from `DllMain`.
- Default period 16 ms, i.e. about 60 reads per second.
- Discovery of the player instance by scanning, with rate-limited rescanning.
- Pointer revalidated on every tick; every read goes through a function that fails
  cleanly rather than raising an exception.

### 6.4 Discontinuity detection

A save load rewrites the entire object between two ticks, which is indistinguishable from
a fortification of several hundred points. Three independent safeguards, and **none** of
them may be removed:

1. re-prime when the player pointer is rescanned or invalidated;
2. re-prime when a single tick changes at least 20 watched floats — a spell effect moves a
   handful, a reload dozens;
3. rejection of any `|Δmaximum|` greater than 2000 points (`kMaxPlausibleDMax` in
   `src/ModelWatch.h`), which re-primes instead of correcting. The largest transitions
   measured are ±300 points, from one spell combining Fortify Intelligence 100 and Fortify
   Magicka 100 (§4.1, §4.6). Several effects starting or ending in the same poll add up,
   which is what the margin is for; a rewritten object counts in thousands.

An earlier version of guard 3 gave 200 points, a lone Fortify Intelligence 100, as the
largest legitimate effect. The measured ±300 contradicts it; the limit itself was never
that low.

`[DEDUCTION]`: a legitimate change above 2000 points would need several large effects to
start or end within the same 16 ms poll. It would be re-primed rather than corrected,
that is, left to vanilla behaviour once.

A second limit, `fMaxDeltaGuard` (§7.2), is checked by the correction itself. At its
default of 100000 it never triggers, because guard 3 rejects anything above 2000 first;
it only matters if set below 2000.

A re-prime triggers no correction and does not count as an event.

### 6.5 Populated-object criterion

The `PlayerCharacter` instance exists well before a save is loaded, and its block of
attribute values is then entirely zero. A correction must never be attempted on this empty
object.

The criterion is the **number of non-zero, plausible floats** in the window
`0x400..0xB88`. An empty object has 5 to 10 out of 482; a populated object has 25 to
60. Threshold at 18.

Trap not to repeat: "the deficit has stopped being zero" is a **wrong** criterion,
because a full pool has a legitimately zero deficit. It delays activation until the
player's first spell, or even indefinitely.

### 6.6 Logging

- File next to the `.asi`, truncated at startup. **No rotation**: an ordinary player will
  not come and clean up rotation files, and cluttering their installation folder is
  unacceptable.
- The log opens with a **one-line summary**:
  - plugin version, `ACTIVE` or `DRY RUN`, build time;
  - the game executable and its PE timestamp, and whether that is the validated build
    (`0xF19077A4`); if it is not and the plugin would write, an error line follows;
  - preset, engine policy, and whether the `.conf` was found or created.
- At the default level, the summary is followed by:
  - one line listing the loaders and mods (every module outside the Windows folder),
    each with the PE timestamp of its own file;
  - one line of the settings that differ from the shipped defaults;
  - once a save is loaded, the character block of §10.8.

  The rest of the startup detail (configuration dump, game sections, the full module
  list, discovery, the probe's memory dumps) is written at `iLevel = 3`. A bug report
  then carries, in its first lines, everything needed to read the rest.
- Paths are written with the user's profile folder replaced by `%USERPROFILE%`, so a log
  posted publicly does not carry the Windows account name. Paths inside the game folder
  are written relative to it.
- A periodic heartbeat on **a single line**, every 60 seconds by default, which is a
  floor and not an indicative value.
- Every max transition produces a readable block: the two modifier totals before and
  after, the predicted `Δmaximum` with its calculation, the deficit before and after,
  the value the deficit would have under preservation, the fill state before, and the
  verdict.
- The "deficit under preservation" line is essential for diagnosis: it is what separates
  a spell's cost from compensation, as in §4.2.
- Since 1.0.0-dev each transition also says which source moved, as the change of the
  Intelligence and Magicka modifiers in the magic, script and game arrays. A spell or an
  enchantment lands in the magic array, a script in the script array, the engine's own
  adjustments in the game array.
- A decrease on which the engine raised the deficit by more than regeneration explains
  (2 points plus the tolerance of §5.4) produces an error line, `ENGINE MOVED THE DEFICIT`,
  and raises the detail level for 200 lines. It is the signature of a floor at zero
  applied by the engine (§10.5), under which the correction overshoots.
- Automatic, temporary raising of the detail level after an abnormal event, then return
  to the configured level.
- Line cap, so that a pathological loop does not fill the disk.

### 6.7 Language

The code, identifiers, comments, log messages and configuration file are **in English**,
since they are shipped to the player and read by third parties.
This specification and the field journal are in English too. Both were written in French
and translated for publication in version 0.9.7; the translation changes no claim.

## 7. Configuration

`.conf` file in INI format, next to the `.asi`, written on first run and **never
overwritten** afterwards.

The file is self-documenting: each option is preceded by an explanation of what it does
and of the reason for its default value. The probe options and memory offsets are placed
**after a clearly visible `A D V A N C E D` banner**, so that an ordinary player
understands they do not need to scroll further down.

### 7.1 Presets

A single option governs behavior, so that the common case requires no reading:

| Value | Name | Behavior |
|---|---|---|
| 0 | Off | Observation only, no writes. |
| 1 | Vanilla+ | Corrects only going negative. Keeps the free magicka. |
| 2 | **Glass** | Chapter 2 model in full. **Default.** |
| 3 | Custom | The fine-grained options of the dedicated section are read. |

The fine-grained options are read **only** if the preset is 3. A player who changes a
fine-grained option while leaving the preset at 2 must not get a silent hybrid behavior.

Preset 1 is an explicitly inferior compromise, exposed because correcting symptom C alone
is what most players ask for. Chapter 4.4 explains why it turns a conservative exploit
into a net leak; the `.conf` must say so.

### 7.2 Fine-grained options

| Key | Default | Role |
|---|---|---|
| `bPreserveOnIncrease` | 1 | Keep the current when the max increases. |
| `bPreserveOnDecrease` | 1 | Keep the current when the max decreases. |
| `bSpillOverflow` | 1 | Clamp to 0 when the current would exceed the new max. |
| `bFloorAtZero` | 1 | Forbid a negative current. Inoperative: chapter 5.2. |
| `fEpsilon` | 0.01 | Constant term of the tolerance. |
| `fMaxDeltaGuard` | 100000.0 | Beyond this, refuse to correct. Inert at the default: guard 3 of §6.4 rejects anything above 2000 first. |

### 7.3 Runtime safety

- `bDryRun` defaults to **0** since 0.9.2, once the write path had been validated in game
  (§4.6). With `bDryRun = 1` the mod computes and logs every correction without writing
  it, which is the first thing to set after a game update (§9.4). Earlier versions
  defaulted to 1, and the player switched to 0 after checking the log against what they
  had done in game.
- No default key may conflict with the game's destructive shortcuts. `F5`, `F8` and `F9`
  are reserved for quicksave and quickload and **must never** be offered. The default
  memory dump key is `F10` (`0x79`); `F2`, `F4` and `F11` are also free.

## 8. Acceptance criteria

The mod is deemed correct when, on the same character, the following six scenarios behave
as indicated. Each one is verifiable in the log and on screen, without the console.

| # | Scenario | Expected |
|---|---|---|
| 1 | Full pool, activate a fortification | Current unchanged in absolute value; the bar visually empties. |
| 2 | Half-full pool, activate a fortification | Current unchanged, minus the spell cost. |
| 3 | Full pool, expiry | Pool full at the new max. No change in current beyond the excess. |
| 4 | Low pool, expiry | Current unchanged. **Never negative.** |
| 5 | Equip then unequip a fortifying item, half-full pool | Current identical at start and end. No gain, no loss. |
| 6 | Activation/expiry loop repeated ten times | Current strictly decreasing by the spell costs. No positive drift. |

Scenario 6 is the anti-exploit test: it would fail on any correction that handles only one
of the two halves of the defect.

Scenario 5 is the neutrality test: it would detect a double application of the correction,
the case handled in §5.3. It is also an unambiguous transition, so it calibrates the
engine policy in the sense of §5.3.2.

In addition, the host assertion suite must stay green. It checks the rule of chapter 5,
the tolerance bounds of §5.4, and the calibration and decision of §5.3 on values taken
from the observations of chapter 4, and it compiles and runs outside Windows. The three
measured transitions of chapter 4 are replayed in it end to end, so that a regression is
detected against the field data and not against invented numbers.

## 9. Risks and constraints

### 9.1 The virtual call that kills the process

`GetActorFloatValue`, index `0x0B1` of the `PlayerCharacter` virtual table, would give the
maximum magicka and would make several things simpler. It is **unusable**, and this is
not a theoretical precaution:

- called from the observation thread, it once caused a write access violation at the
  null address, inside the game's code, at module offset `0x458F57C`;
- on another run of the same executable, the same call simply hung without ever
  returning.

`[DEDUCTION]`: this non-deterministic behavior, sometimes a hang and sometimes a fault, is
that of a function that dereferences a context that is valid only on the game thread.

A timeout guard survives a hang. **Nothing survives an access violation**: it belongs to
the entire process, so the game dies with it. The corresponding option must stay disabled
by default, and the demonstration above must appear in the `.conf` so that nobody
re-enables it out of curiosity.

Corollary: the virtual table was indeed located and validated — 201 valid entries,
`Update(float)` at index `0x099` — but this knowledge only serves to document the dead
end.

### 9.2 Concurrent write

The mod writes `+0x06B8` from its own thread, without synchronization with the game
thread. A write of a 4-byte-aligned float is atomic on x86-64, so there is no value
tearing. The residual risk is a lost write if the engine writes the same field in the same
window.

Mitigation: never write without having observed the transition that motivates it, never
rewrite the same correction twice, and check the read-back value on the next tick. A
correction that does not "take" must be logged, not repeated in a loop.

### 9.3 Exiting the process without hanging it

`ExitProcess` kills **all** threads except the caller, at an arbitrary instruction, and
only **then** runs the `DLL_PROCESS_DETACH` handlers, under the loader lock.

The observation thread wrote to the log via `std::fprintf`. A CRT `FILE` holds a lock. If
the thread died inside it, it took that lock to the grave — a critical section held by a
dead thread is never released. The logger's static destructor then called `fflush` and
then `fclose`: permanent deadlock, under the loader lock. Visible result: window gone,
process still present in Task Manager, forced termination required.

Second effect of the same `fopen`: a stdio handle does not grant `FILE_SHARE_DELETE`. As
long as the process was alive, the log was **undeletable**, including when the process was
precisely in the state where one wants to read and then delete that log.

Fixed in 0.9.1 by removing stdio entirely from `Log.h`:

- `CreateFileA` with `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`: the player
  can delete or rename the log at any time, with the game running or stuck;
- a single `WriteFile` per line, unbuffered. `WriteFile` takes no user-mode lock, so the
  entire class of deadlock disappears;
- `Shutdown()` **never** waits. It tries `TryEnterCriticalSection` and gives up on
  failure. Abandoning a handle at process exit costs nothing, the kernel closes it;
  hanging the process costs the player their session;
- CRLF line endings, so that Notepad can read it.

The mod has no clean shutdown mechanism beyond that, and does not need one: this version
never has anything to undo, and `DLL_PROCESS_DETACH` merely raises the stop flag and
returns.

### 9.4 Dependency on offsets

The four offsets of chapter 3 are deduced from a community source and validated
empirically on a single PE timestamp. A game update can invalidate them.

Mitigation: the offsets can be overridden in the `.conf`, the executable's PE timestamp is
logged at every launch, and the mod refuses to write if its reads do not pass a
plausibility check — strictly positive deficit, aberrant modifier totals, unpopulated
object.

### 9.5 Coexistence with other mods

The reference installation hosts at least an attribute scaling modifier, a skill scaling
modifier, a spell removal plugin, a tag cleaner, as well as
[UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) via `dwmapi.dll` and, since 21 October 2026,
[OblivionRemasteredPerformanceFix](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix)
as `version.dll`, with the ASI loader renamed to `winmm.dll`. The latter produces the
memory dump files during a crash; it is not their cause.

A mod that modifies attributes therefore also modifies the maximum magicka. The rule of
chapter 5 is indifferent to this, since it works only on variations, but such a mod can
produce large and frequent variations. The guard of §6.4 must stay calibrated to tolerate
them without mistaking them for a reload.

### 9.6 Already corrupted saves

A save made while the current was negative keeps that deficit. The mod does not repair it,
for the reason given in §5.2. It must, however, **detect** a manifestly inconsistent
deficit on load and report it in the log, rather than letting the player conclude that the
mod does not work.

## 10. What remains unknown

### 10.1 The max increase without a confounder

Symptom A has never been measured unmixed with a spell's cost. The reasoning of §4.2 is
solid but remains a deduction.

Test that would settle it, five minutes in game: bring the pool well below its max, then
**equip** an item with Fortify Intelligence or Fortify Magicka. The max rises without any
magicka being spent, so any movement of the deficit is pure compensation. Expected verdict
if the analysis is right: the deficit does not move.

The symmetric case, removing the same item with a **full** pool, has already been measured
(§4.3); it is removing the item with a **partially empty** pool that would complete the
table.

Part of the need has, however, fallen away, twice. §4.5 captured an increase on a non-full
pool, which eliminates the explanation "it was full, it just filled up". And event 5 of
§4.6 is an increase **with no cost in the same poll**: `Δd = 0` exactly. The confounder
has therefore been removed by measurement, not by reasoning. What remains open is no
longer the mechanism.

This test is not blocking, for two reasons. The rule of chapter 5 does not depend on its
result. And above all, an increase through equipping is precisely an **unambiguous**
transition in the sense of §5.3.2: if the engine behaved there other than expected,
calibration would detect it and the mod would abstain on its own.

### 10.2 The absolute max

Not located in `0x400..0xB88`. It may reside outside this window, in a base class below
`0x400` or in a separate object, or exist only as the result of a computation.

Unlocking it would enable the floor of §5.2, the repair of saves of §9.6, and log messages
expressed in magicka points rather than as a deficit. None of these three items is
necessary for the fix itself.

### 10.3 Blind spot: permanent attribute changes

The watcher monitors the **modifier** fields. A permanent increase in Intelligence —
level-up, attribute restoration, permanent effect — changes the base value and not a
modifier: the predicted `Δmax` is then 0 and **no event is emitted**. The max rises, the
deficit is not rebased, and the player receives the difference for free, without the mod
seeing it.

Not corrected, and not measured.

**History of the justifications, two of them false.** This case was first dismissed with
the argument "the test character is at Intelligence 100, therefore capped". A later
version declared this argument dead on the grounds that the installation includes
uncapping mods. That was false in turn: these mods do not lift the **progression** cap,
only the **usage** cap in certain formulas, and their Nexus pages say so explicitly
(§10.7).

Current position:

- **Unreachable at base Intelligence 100**, which is the test character's case. Base
  Intelligence cannot be **trained** past 100 (§10.7), so any Intelligence above 100 comes
  from a Fortify effect, hence from a **modifier**, which the watcher sees and corrects
  normally.
- **Abilities are the exception**, `[DEDUCTION]` from §10.8: racial, birthsign and
  vampirism bonuses sit in the base values, not in a modifier. An ability that fortifies
  Intelligence or Magicka, gained or lost during play, would move the maximum with no
  event. None is known on the test character. The log reports any change of base
  Intelligence or base spell points (§10.8).
- **Still reachable below 100**, through a level-up that raises Intelligence. Remastered
  allows up to 5 points on one attribute per level-up
  ([UESP, Remastered Leveling](https://en.uesp.net/wiki/Oblivion:Remastered_Leveling)).
  `[DEDUCTION]` from that and §3.4: a level-up moves the maximum by at most 10 points
  without an event.
- **Not known**: whether restoring damaged Intelligence or a permanent blessing changes the
  base value or a modifier. Neither has been measured.

Whether the game refills magicka on a level-up, which would make the case harmless, is not
documented. UESP's Leveling and Remastered Leveling pages say nothing about it. In the
original game a level-up only takes effect after resting in a bed
([UESP, Leveling](https://en.uesp.net/wiki/Oblivion:Leveling)); whether Remastered keeps
that requirement is not stated on either page. This **must be checked at the first
level-up**: a non-zero deficit in the log right after it means the game did not refill.
Since 1.0.0-dev the log writes a `LEVEL UP` line with the deficit before and after
(§10.8), so the first level-up with that build answers it.

A neighboring case has, on the other hand, been ruled out: the **change of vampirism
stage**. It modifies Strength, Speed and Willpower, never Intelligence or Magicka
([UESP, Vampirism](https://en.uesp.net/wiki/Oblivion:Vampirism)). The predicted `Δmax` is
therefore 0 and no event is emitted — correct behavior, not a blind spot. (An earlier
version said the watcher sees the stage's floats move in the modifier window. Withdrawn,
21 October 2026: the stage bonus appears to sit in the base values, §10.8.) Feeding or progressing in vampirism has
**no effect on the magicka pool**, only on its refill rate (§3.3).

### 10.4 Armored players: the magnitude is not guaranteed to be round

All measurements show `Δmax = +300` exactly, because spell effectiveness was 100%. As
soon as a piece of armor is worn, this effectiveness is capped at 95% and multiplies the
**magnitude** of the effect
([UESP, Magic Overview](https://en.uesp.net/wiki/Oblivion:Magic_Overview#Spell_Effectiveness)). A
player in heavy armor at skill 50 will therefore see `Δmax = 285` and not 300, with
non-integer values for other combinations.

This requires no adaptation: the correction reads `Δ` from the actual modifiers and has
never assumed a round magnitude. It is noted here because the corollary is that observing
`+300.0000` on all eight casts **proves** that the test character was wearing no armor,
which served to eliminate armor as the cause in §4.7.3. The first cast at 95 % was
logged on 28 August 2026 (§4.8, event 11): `+285`, corrected like any other.

### 10.5 Untested effects

Measured so far: fortifications cast by the player, an enchanted ring, and, on 28
August 2026 (§4.8), what reads as a Drain Magicka 22 and a lasting Intelligence −1. Not
observed: potions, altars and blessings, and the level-up that permanently increases
Intelligence.

**Open design question: drains.** On the drain of §4.8 the engine did what it does for
every change, and the mod did what it does for every change: it preserved current
magicka on the way down and refused the gain on the way back. A Drain Magicka on a pool
that is not full therefore costs the player nothing with the fix, where vanilla takes the
magnitude away for the duration.

The documented behaviour is the vanilla one. For the original game, UESP describes Drain
Magicka as one that "temporarily reduces the current and maximum magicka, however it
cannot reduce the current Magicka below zero"
([UESP, Spell Making](https://en.uesp.net/wiki/Oblivion:Spell_Making#Spell_Chaining)), and
§4.8 shows the Remastered engine taking the 22 points from current magicka. So the fix
changes what a drain does. The glass-of-water model calls that consistent: the glass
shrank, the water stayed below the rim. As a game rule it is a choice: a drain is meant to
take magicka away. Telling a hostile drain apart from the expiry of the player's own
fortify would need the list of active effects, which the mod does not read.

**Open risk: a floor at zero applied by the engine.** The same page says that Fortify
Magicka's expiry and Drain Magicka cannot take current magicka below zero. If the
Remastered engine applies such a floor, it moves the deficit itself on those
transitions, and the relative rule of chapter 5, which assumes the engine left the
deficit alone, would then grant the difference a second time. The measurement of §4.1
rules this out for a combined expiry: Fortify Intelligence 100 and Fortify Magicka 100
ending together took current magicka from 52.23 to −247.77. That is the full 300, and no
order of applying the two with a floor gives that number. `[DEDUCTION]`: Remastered does
not apply the floor. A **Fortify Magicka alone** expiring at a low pool, and a drain on a
nearly empty pool, have not been measured. Since 1.0.0-dev such a case cannot go unnoticed:
the log flags any decrease on which the engine raised the deficit (§6.6).

What the log does not tell, and an investigation of drains would need:

- whether current magicka would actually have gone below zero. The absolute maximum is
  not read (§10.2), but since 21 October 2026 the log prints an **estimate**,
  `2 × (base Intelligence + modifier) + base spell points + Magicka modifier`, and the
  current magicka it implies (estimate + deficit), on every transition. It matched the
  screen once, with a full pool (§10.8); a partial pool has not been compared yet;
- which effect caused a change: that needs the list of active effects, which the mod
  does not read. The per-source breakdown (§6.6) only says spell or enchantment, script, or
  engine.

`[DEDUCTION]`: all of them go through the same modifier arrays, so the rule should apply
as is. A level-up, on the other hand, raises the base attribute and not a modifier, and
therefore escapes detection (§10.3). To be checked at the first level-up.

### 10.6 Scope of the fix

The mod handles only the player. NPCs presumably suffer from the same defect; they are not
in scope, and the `PlayerCharacter` instance is in any case the only one discovered
reliably.

### 10.7 The test bench is a modded installation

All measurements in this document were taken on an installation that includes, in addition
to the ASI loader and UE4SS, two mods by Baestus:

| Mod | What it actually does |
|---|---|
| [Damage Formula And Magicka Cost Skills Scaling Uncapper](https://www.nexusmods.com/oblivionremastered/mods/1846) | removes the cap at 100 on **skills** in the damage **and magicka cost** formulas. Applies to all skills modified by Luck, so everything except Speechcraft, Athletics and Acrobatics, which are already uncapped |
| [Damage Formula Attribute Scaling Uncapper](https://www.nexusmods.com/oblivionremastered/mods/2068) | removes the cap at 100 on **attributes** in the **damage** formula only, plus the three caps specific to Hand to Hand |

Two clarifications from the Nexus pages that matter, verbatim: "**THIS DOES NOT ALLOW YOU TO LEVEL
YOUR SKILLS PAST 100**" and "**THIS DOES NOT ALLOW YOU TO LEVEL YOUR ATTRIBUTES PAST 100**".
These mods therefore do not lift the **progression** cap: they make values that are already
above 100, obtained through Fortify effects, alchemy or enchantment, actually count in the
calculations instead of being clamped to 100.

The test character's base values above 100 (Willpower, Speed, Destruction and Illusion at
110, §10.8) do not contradict this: they are 100 plus the +10 of the vampirism stage,
which sits in the base values. `[DEDUCTION]`

Since 21 October 2026 the installation also runs OblivionRemasteredPerformanceFix 1.1.0
(§9.5). Two sessions with it, three transitions, two writes, no failure: no interference
seen, on little play.

This distinction is decisive for this mod, and it reverses two conclusions:

| Point | Status |
|---|---|
| `Δmax = 2 × ΔInt + ΔMagicka` remains valid above Intelligence 100 | **sourced**, §3.4. Neither mod touches the magicka formula, and vanilla does not saturate this term |
| The blind spot of §10.3 on **permanent** Intelligence increases | **unreachable at base Intelligence 100**, the test character's case: these mods do not allow base Intelligence to go above 100, so any Intelligence above 100 goes through a Fortify effect, hence through a **modifier**, which the watcher sees. **Still reachable below 100**, through a level-up (§10.3). `[CORRECTION]` of an earlier version that declared this blind spot active, and of one that called it inactive without that condition |
| The cap at 100 in the spell cost equation | **lifted**, contrary to what an earlier version claimed. Mod 1846 removes it by binary patch, see §4.7.3 |

Mod 1846 publishes its source
([OBSE64_Skill_Damage_Scaling_Cap_Remover](https://github.com/Baestus/OBSE64_Skill_Damage_Scaling_Cap_Remover)),
which is what §4.7.3 quotes. It patches the game's **executable code** via `VirtualProtect`
then `memcpy`, with no restoration on unload, whereas this mod writes only into a **data**
field of the player instance. The two do not compete for any address.

Regeneration, on the other hand, is quadratic in Willpower (§3.3), so fortifying this
attribute is the most powerful lever on the gameplay cost of preset 2:

| Willpower | Out-of-combat regeneration | In combat |
|---:|---:|---:|
| 100 | 11.00 /s | 5.50 /s |
| 120 | 15.12 /s | 7.56 /s |
| 150 | 22.50 /s | 11.25 /s |
| 200 | 38.00 /s | 19.00 /s |

Formula and halving in combat from
[UESP, Willpower](https://en.uesp.net/wiki/Oblivion:Willpower). Uncapping the magic
skills, conversely, brings almost nothing more on cost: the hyperbolic branch of
mod 1846 gives `0.2 × 100 / S`, i.e. 19.3% of the base cost at `S = 103.6` and still 14.97%
at `S = 133.6`. The return decreases as `1/S`.

### 10.8 Character data in the log (validated 21 October 2026)

Since 1.0.0-dev the log describes the character: level, base attributes, base skills,
and the modifier totals of each. It also writes a line when the level, a base attribute
or a base skill changes during play. Nothing here feeds the correction; it is there so
that a log from another player's machine can be read without asking them for their
character sheet, and so that a level-up documents itself (§10.3).

The values are read from memory, never written, through the same checked reads as the
rest (§9.2):

| Field | Where | Type |
|---|---|---|
| player form ID, must be `0x14` | `PlayerCharacter + 0x10` | u32 |
| base form (`TESNPC`) | `PlayerCharacter + 0x50` | pointer |
| base form ID, must be `0x07` | `TESNPC + 0x10` | u32 |
| base spell points, fatigue, **level** | `TESNPC + 0x54`, `+0x56`, `+0x5A` | u16, u16, i16 |
| base health | `TESNPC + 0xF0` | u32 |
| **base attributes**, Strength to Luck | `TESNPC + 0x100` | u8 ×8 |
| race | `TESNPC + 0x1B0` | form pointer |
| **base skills**, Armorer to Speechcraft | `TESNPC + 0x1B8` | u8 ×21 |
| class | `TESNPC + 0x1D0` | form pointer |
| ready to level up | `PlayerCharacter + 0x41C` | bool |
| birthsign | `PlayerCharacter + 0x928` | form pointer |

Sources:

- The `PlayerCharacter` offsets come from commonlibob64 (`PlayerCharacter.h`,
  `TESObjectREFR.h`, `OBJ_REFR.h`, `TESForm.h`).
- The `TESNPC` offsets are `[DEDUCTION]`. They are summed from the declared sizes of
  `TESActorBase`'s base components in commonlibob64, assuming MSVC places each one at
  8-byte alignment. One cross-check holds: `TESActorBase` (0x1A8) plus `TESRaceForm`
  (0x10) lands exactly on `NPC_DATA` at 0x1B8, the offset `TESNPC.h` declares.
- The field order and sizes match the `NPC_` record format on UESP: `ACBS` holds flags,
  base spell points, fatigue, barter gold, level, calcMin and calcMax; `DATA` holds the
  21 skills, health and the 8 attributes
  ([UESP, NPC_ record](https://en.uesp.net/wiki/Oblivion_Mod:Mod_File_Format/NPC_)).
- Form IDs `0x14` (player reference) and `0x07` (player base NPC):
  [UESP, Bendu Olo](https://en.uesp.net/wiki/Oblivion:Bendu_Olo) (the default player character) and
  [CS wiki, Player](https://cs.uesp.net/wiki/Player).
- Actor-value order, used for the modifier totals and the skill names:
  [CS wiki, Stats List](https://cs.uesp.net/wiki/Stats_List). It agrees with the field
  dump of §3.4. commonlibob64's `ActorValue.h` puts Magicka at 8 instead of 9; that
  enumeration contradicts the dump and is not used.
- Mastery follows the **base** skill (Novice below 25, Apprentice, Journeyman, Expert,
  Master at 100), and Fortify effects do not unlock perks
  ([UESP, Skills](https://en.uesp.net/wiki/Oblivion:Skills)). That page has no
  Remastered note, so for Remastered this is `[DEDUCTION]`.

Before any field is trusted, the player's form ID, the base form's form ID and the base
form's vtable (`TESNPC`, found through RTTI) are checked. If a check fails, the log says
which one and the character block is skipped.

A change is reported as one game event only if it looks like one. A Remastered level-up
raises the level by one and at most 3 attributes
([UESP, Remastered Leveling](https://en.uesp.net/wiki/Oblivion:Remastered_Leveling)), and
a skill rises a few points at most between two samples 250 ms apart. Anything larger,
or any value going down, is treated as another save being loaded: the block is written
again rather than reported as progress.

**Validated in game on 21 October 2026.** The `PLAYER` block of session 2 (§4.9) was
compared with 15 screenshots taken with the same save:

- **Level** 37: matches.
- **All 8 attributes**: the sheet shows base + modifier exactly, for every one. Strength
  96 + 22 = 118, Intelligence 100 + 111 = 211, Willpower 110 + 159 = 269, Agility 88 + 11
  = 99, Speed 110 + 9 = 119, Endurance 100 + 10 = 110, Personality 86 + 8 = 94, Luck
  63 + 10 = 73.
- **All 21 skills**: the same, for every one (Blade 54 + 15 = 69, Block 85 + 15 = 100,
  the 19 others with a modifier of 0).
- **Maximum magicka**: the sheet shows 622 = `2 × 211 + 100 + 100`, that is
  `2 × (base Intelligence + modifier) + base spell points + Magicka modifier`. This checks
  the factor 2 of §3.4 and the Magicka modifier read, on top of the field offsets.
- Race, class and birthsign: the sheet says Breton, Mage, Mage. The log prints form IDs
  `0x000224FC`, `0x000105D6` and `0x0001FD93`; no source mapping those IDs to names was
  found, so they are not checked.
- Base health 272 and base fatigue 0 do not explain the sheet's 621 and 849; they are not
  used and not investigated.

The TESNPC offsets in the table above are therefore **confirmed** for level, attributes,
skills and spell points, on this game build.

**What the base values hold** `[DEDUCTION]`. The Active Effects screen lists larger
bonuses than the modifier arrays hold, and the gap matches the character's abilities:

| | Active Effects | modifier arrays | gap | ability |
|---|---:|---:|---:|---|
| Magicka | 200 | +100 | 100 | Breton +50, Mage birthsign +50 ([UESP, Breton](https://en.uesp.net/wiki/Oblivion:Breton), [The Mage](https://en.uesp.net/wiki/Oblivion:The_Mage)) |
| Strength | 30 | +22 | 8 | +10 vampirism; the Scales' +2 (below) is in the arrays but not listed |
| Hand to Hand | 10 | +0 | 10 | vampirism skill bonus, 10 pts |
| Block | 18 | +15 | 3 | Gray Prince training, 3 pts |

These ability bonuses are not in the modifier arrays, and the sheet does not add them to
the base the log reads. So they are in that base: base spell points 100 is the Breton and
birthsign bonus, and the four base values above 100 are the vampirism stage (§10.7).
Consequence for the fix: §10.3.

**The source split.** The PLAYER lines print each modifier's split between the magic,
script and game arrays when it is not all magic, written `+22 (m+20 s+2)`. The Scales of
Pitiless Justice (§4.8) explain Strength +2 and Personality −2 entirely, and Intelligence
and Agility +2 each. That leaves **−1 on Intelligence, Agility, Willpower and Speed** with
no source on any screen; the 28 August Intelligence −1 (§4.8, event 13) is probably one of
them. The first log with the split says which array holds them.

Not done, and out of scope until validated: correcting a change of **base**
Intelligence the way modifier changes are corrected, which would close the blind spot
of §10.3. The log only reports it.

## 11. Three method errors not to repeat

The first two points do not concern the game but the tooling; each of them cost a full
cycle of compiling, shipping and in-game testing. The third is a design error in this very
specification.

**A check that is not one.** A packaging assertion checked that a configuration file
contained the substring `bCallVirtuals = 0`. It passed even though the actual setting line
was `1`, because the substring existed elsewhere, in a comment. A configuration file was
shipped with the dangerous option enabled by default.

Rule: never validate a configuration by substring search. Parse the file line by line,
require every non-empty line to be a comment, a section header, or an assignment with a
**single-token value**, then require **exactly one** setting line per key and compare the
extracted value. A sentence that escaped from a comment block shows up as a key whose
value has several tokens, and gets rejected.

**A table indexed by an enumeration.** A test function translated a verdict into text via
a `const char*` array indexed by the enumeration. The enumeration went from three to five
values, the table did not follow, the read went out of bounds and the test binary died
without printing a single line. No compiler warning.

Rule: translate an enumeration with a `switch` listing every case and **without
`default`**, so that an omission becomes a compile error.

**A guard that forbade the case to be corrected.** An earlier version of §5.3 required
`|Δd| ≤ tolerance` before acting, on the grounds that a moving deficit could signal an
engine that had already compensated. This condition is wrong for the self-cast
fortification, where the spell cost moves the deficit in the same frame: the guard would
have refused to correct exactly symptom A, i.e. half of the mod's reason for existing. The
defect was not caught at writing time because the guard had been validated mentally on
the expiry cases, where it works.

Rule: validate every inaction condition against **each** use case the specification
claims to handle, by name and one by one, and not against the case that served to write
it. The fix is the calibration mechanism of §5.3.2, which moves the decision out of the
ambiguous transition instead of asking that transition to make it.

## 12. Sources

UESP documents Oblivion Remastered inside the same `Oblivion:` namespace as the original
game ([UESP, Oblivion Remastered](https://en.uesp.net/wiki/Oblivion:Oblivion_Remastered)):
some pages have an "Oblivion Remastered" section, some carry inline Remastered notes, and
many have neither. A statement from a page without a Remastered note describes the
original game; this document says so where it relies on one, and says what was measured.

- [UESP — Magicka](https://en.uesp.net/wiki/Oblivion:Magicka): `maximum = 2 × Intelligence` plus flat race and birthsign bonuses, under "Original Oblivion". The Remastered section covers regeneration only; the maximum formula is confirmed in Remastered by measurement (§4.3, §4.6). Its Remastered regeneration formula disagrees with the measurement (§3.3).
- [UESP — Willpower](https://en.uesp.net/wiki/Oblivion:Willpower): magicka regeneration, with a Remastered section whose formula the measurement confirms (§3.3).
- [UESP — Remastered Changes](https://en.uesp.net/wiki/Oblivion:Remastered_Changes) and [Remastered Leveling](https://en.uesp.net/wiki/Oblivion:Remastered_Leveling): Remastered only.
- [UESP — Spell Making](https://en.uesp.net/wiki/Oblivion:Spell_Making): "Spell Chaining" (original game): Fortify Magicka and Intelligence add to current and maximum, Drain Magicka removes from both, with a floor at zero for Magicka effects (§2, §10.5). A Remastered note: several Fortify Attribute and Fortify Skill effects may be combined in one spell.
- [UESP — Spell Effects](https://en.uesp.net/wiki/Oblivion:Spell_Effects): spell cost, `B × M × D × A` with `M = magnitude^1.28` and a per-effect factor of `1.4 − 0.012 × skill` (original game; the same constants appear in mod 1846's source, §4.7.3).
- [UESP — Luck](https://en.uesp.net/wiki/Oblivion:Luck): `effective skill = skill + 0.4 × (Luck − 50)` (original game; mod 1846 calls the game's own `luck_skill_modifier`).
- [UESP — Magic Overview](https://en.uesp.net/wiki/Oblivion:Magic_Overview#Spell_Effectiveness): spell effectiveness capped at 95 % in armor (original game; measured in Remastered, §4.8).
- [commonlibob64, `PlayerCharacter.h`](https://github.com/libxse/commonlibob64/blob/main/include/RE/P/PlayerCharacter.h): class layout, origin of the four offsets of chapter 3.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader): `.asi` loader, installed as `version.dll`.
- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS): installed as `dwmapi.dll`, producer of the memory dump files.

The measurements of chapters 3 and 4 come from the logs of 5 and 6 August 2026 on
`OblivionRemastered-Win64-Shipping.exe`, PE timestamp `0xF19077A4`. They are
reproduced in full, with their context and the discarded hypotheses, in
`FIELD-JOURNAL.md`.
