# MagickaPoolFix — Field journal

This document is the raw history of the measurements, the successive hypotheses, the mistakes
made and their corrections, from 5 August 2026 onward. It is **not** the
specification: that one is self-contained and lives in `SPEC.md`.

It is kept for a single reason: every claim in `SPEC.md` must be traceable
back to a dated observation. The sections below also contain
conclusions that have since been **withdrawn**, explicitly flagged as such; do not
read them as the state of the art.

---

ASI plugin for The Elder Scrolls IV: Oblivion Remastered.
Corrects the semantics of the magicka pool when the **maximum** pool changes.

Self-contained document: it contains all the context, the sourced facts, the design decisions,
the offsets, the algorithm and the configuration specs needed to
resume or redo this work without any conversation history.

Document version: 1.2 — 6 August 2026
Status: specification validated, phase 1 (probe) ready to be developed

Changelog:
- 1.2 resolves the "current or maximum" ambiguity with an automatic calibrator (section 8.4 bis),
  implemented and tested, and sets the associated log-level policy.
- 1.1 corrects section 12.3 (the alleged negative multiplier beyond skill 117
  was an erroneous extrapolation on my part) and adds the Luck modification formula
  in section 4.4.

---

## 1. Rigor convention

This document explicitly distinguishes three kinds of claim:

- **[FACT]** — verified against a primary source, URL provided.
- **[DEDUCTION]** — inferred from several sources or from reverse-engineered code. To be validated.
- **[CALCULATION]** — derived from a sourced formula by my own calculation. Reproducible, not verified in game.

Any unlabeled value in the technical sections must be considered to be validated.

---

## 2. Player context

Reference build for which the need arose. Level 33, Breton, Mage birthsign,
Mage class with the 7 schools of magic as major skills.

| Attribute | Value |
|---|---|
| Strength | 76 |
| Intelligence | 100 |
| Willpower | 100 |
| Agility | 76 |
| Speed | 100 |
| Endurance | 100 |
| Personality | 76 |
| Luck | 59 |

Magic skills: Alchemy 90, Alteration 98, Conjuration 100, Destruction 100,
Illusion 81, Mysticism 61, Restoration 100.

**Base magicka pool: 300** = (Intelligence 100 × 2) + 50 (Breton) + 50 (Mage birthsign).

### Self-imposed play constraints (role-play)

They explain the design priorities but **must not** be hard-wired into the plugin:
another player may not follow them.

- **Never design a spell that cannot be cast naked.** Equipment buffs and
  vampirism are bonuses, never prerequisites. Consequence: design is done with
  the naked values, so the ceiling computed below is structural.
- Plays in robes, not armor. The Light Armor level comes from the Arena (armor imposed)
  and from the "dremora warrior" mode (summoning a full armor set and a sword with the pool
  at 600, then playing melee for ~2 min without spells).
- Vampirism postponed until all 7 schools are at 100.

### Spell sequence concerned

1. Fortify Willpower +100 — increases regeneration in anticipation of the enlarged pool.
2. Fortify Intelligence +100 (pool +200, indirect) + Fortify Magicka +100 (pool +100, direct).

Total: max pool from 300 → 600.

---

## 3. The problem

Two behaviors considered inconsistent:

1. **On activation** — the current pool increases together with the maximum. At 100/300,
   activating +300 gives 400/600: 300 points of magicka appear for free.
2. **On expiry** — a `-300` is applied to the current pool as well as to the maximum. At 100/600,
   expiry gives **-200/300**. The player is left without magicka for as long as it takes to climb back to zero.

### Target model: the glass-of-water analogy

- Enlarging the glass does not make water appear in it.
- Shrinking it spills the overflow, but does not remove what still fits in it.

Translated into rules:

- Max goes up → current pool unchanged.
- Max goes down → current pool unchanged, unless it exceeds the new max (overflow spilled).
- Current pool never negative.

Reference examples:

| Situation | Vanilla | Target |
|---|---|---|
| 100/300, activation +300 | 400/600 | 100/600 |
| 100/600, expiry -300 | -200/300 | 100/300 |
| 350/600, expiry -300 | 50/300 | 300/300 |

---

## 4. Verified facts about the game mechanics

### 4.1 Base pool

**[FACT]** `Magicka = (Intelligence × 2) + birthsign bonus + race bonus`.
Mage birthsign: +50. Apprentice birthsign: +100 with 100% Weakness to Magicka.
Atronach birthsign: +150 with Stunted Magicka.
Source: [UESP:Magicka](https://en.uesp.net/wiki/Oblivion:Magicka)

**[FACT]** Intelligence at 100 gives +200 magicka. The ×2 multiplier also applies
beyond 100 and to values modified by Fortify.
Source: [UESP:Magicka](https://en.uesp.net/wiki/Oblivion:Magicka),
[UESP:Spell Making](https://en.uesp.net/wiki/Oblivion:Spell_Making)

### 4.2 Documented behavior of effects on expiry

**[FACT]** Three distinct rules, documented on the same page:

| Effect | Documented behavior on expiry |
|---|---|
| Fortify Magicka | removes the magnitude from current and max; "**It cannot reduce current Magicka below zero.**" |
| Fortify Intelligence | removes 2 × magnitude from current and max; "**The current Magicka can become negative in this way.**" |
| Drain Magicka | reduces current and max; "**It cannot reduce current Magicka below zero**"; when chained, brings a negative pool back up to zero |

Source: [UESP:Spell Making](https://en.uesp.net/wiki/Oblivion:Spell_Making)

**Important analytical consequence.** The observed `-200` is fully explained without invoking
a regression in the remaster: `100 − 200` (Intelligence component, not clamped)
`− 100` (Magicka component) = `−200`. **About two thirds of the problem come from the
Intelligence component**, which has never been clamped, neither in the original nor in the remaster.

**Correction of a mistake made during the analysis.** A first reading, based on UESP talk
pages and Reddit threads, had concluded that the clamp to zero existed in the original
and had been removed by the remaster. **That is wrong as stated.** The clamp exists for Fortify
Magicka and has never existed for Fortify Intelligence. The official page
[UESP:Remastered Changes](https://en.uesp.net/wiki/Oblivion:Remastered_Changes) mentions
no change on this point.

The correct angle is therefore not "remaster bug" but: **a deliberate inconsistency since 2006
between two effects that produce the same result with two opposite rules.**

### 4.3 Magicka regeneration — major change in the remaster

**[FACT]** Formulas:

- Original: `0.01 × (0.75 + 0.02 × Willpower) × MagickaTotal` — **proportional to the max pool**.
- Remastered: `2 × (Willpower/50)² + 1.5 × (Willpower/50)` — **depends only on Willpower**,
  independent of the max pool.
- Remastered: regeneration is **halved in combat**.
- Remastered: **2-second delay** after each cast before regeneration resumes.
- Intelligence has **no** effect on regeneration in the remaster (only on the max).

Sources: [UESP:Willpower](https://en.uesp.net/wiki/Oblivion:Willpower),
[UESP:Remastered Changes](https://en.uesp.net/wiki/Oblivion:Remastered_Changes)

**[CALCULATION]** Derived values:

| Willpower | Out of combat | In combat |
|---|---|---|
| 100 | 11.0 pts/s | 5.5 pts/s |
| 200 (with Fortify +100) | 38.0 pts/s | 19.0 pts/s |

**Consequence for naked play.** The remaster massively favors small pools: at Willpower 200
with a pool of 300, you regenerate 38/s where the original would have given 14.25/s. The role-play rule
"never a spell that cannot be cast naked" is therefore far more playable in the remaster than in the original.

**[FACT]** The Stunted Magicka effect (Atronach birthsign, Astral Vapors disease,
Fork of Horripilation) prevents any natural regeneration.
Source: [UESP:Magicka](https://en.uesp.net/wiki/Oblivion:Magicka)

### 4.4 Cost of effects and of custom spells

**[FACT]** Base costs: Fortify Magicka **0.15**, Fortify Attribute **0.6**,
Restore Magicka **2.5**, Drain Magicka **0.18**, Damage Magicka **2.45**.
Source: [UESP:Spell Effects](https://en.uesp.net/wiki/Oblivion:Spell_Effects)

Fortify Magicka therefore costs about **16 times less** than Restore Magicka. That price only makes sense
because the effect provides magicka immediately: in vanilla, Fortify Magicka is de facto
a disguised Restore Magicka with a deferred bill.

**[FACT]** Cost formula for a custom spell:

```
B = BaseCost / 10
M = Magnitude ^ 1.28
D = Duration
A = Area × 0.15
Cost = B × M × D × A          (M, D, A are at least 1)
× 1.5 if the spell is on Target
then, per effect: × (1.4 − 0.012 × Skill)
```

The skill is first modified by Luck, then constrained between 0 and 100.
The skill adjustment applies **effect by effect**. The gold cost is 3 × the magicka cost.
Wearing armor reduces spell magnitude.
Source: [UESP:Spell Making](https://en.uesp.net/wiki/Oblivion:Spell_Making)

**[FACT]** Skill multiplier: 1.4 (skill 0), 1.1 (25), 0.8 (50), 0.5 (75),
**0.2 (100)**.

**[FACT]** The skill used in this multiplier is first modified by Luck,
**then constrained between 0 and 100**:

```
LuckModifiedSkill = Skill + fActorLuckSkillMult × (Luck + iActorLuckSkillBase)
                  = Skill + 0.4 × (Luck − 50)          [default values]
```

Luck 50 is neutral; each point above 50 adds 0.4 skill levels. Luck
modifies all skills except Acrobatics, Athletics and Speechcraft. The modified value is
not displayed in the menu.
Source: [UESP:Luck](https://en.uesp.net/wiki/Oblivion:Luck)

**Consequence for the reference build**: at Luck 59 and Restoration 100, the effective skill
is **103.6**, so the vanilla cap at 100 is already in effect. The multiplier stays 0.2 and the
calculations in section 5.3 are valid — but see section 12.3 for what changes when this
cap is removed by another plugin.

**Source contradiction to report.** The README of
[OBSE64_Skill_Damage_Scaling_Cap_Remover](https://github.com/Baestus/OBSE64_Skill_Damage_Scaling_Cap_Remover)
gives the remaster's decompiled pseudocode in the form
`(Luck × fActorLuckSkillMult + iActorLuckSkillBase) + Skill`, with the constant **outside**
the multiplication, whereas UESP places it **inside** with the value `-50`. The two forms only
describe the same curve if the constant is `-20` in the decompiled form. Read
literally with `-50`, the decompiled form would make Luck 50 a penalty of −30 skill points, which
contradicts the documented behavior "Luck 50 is neutral". I keep the UESP form and I
flag that the value actually stored in the remaster is **not verified**. Possible empirical
test: compare the observed cost of a known spell against the two predictions.

**[FACT]** In the remaster, several Fortify Attribute or Fortify Skill effects **can** be
combined in the same spell, unlike in the original.
Source: [UESP:Spell Making](https://en.uesp.net/wiki/Oblivion:Spell_Making)

**[CALCULATION]** With Restoration 100 (multiplier 0.2) and magnitude 100 (`M = 363.1`):

| Effect | Cost per second of duration |
|---|---|
| Fortify Attribute 100 (Willpower or Intelligence) | **4.36 magicka/s** |
| Fortify Magicka 100 | **1.09 magicka/s** |

### 4.5 Exploits relying on negative magicka

**[FACT]** Spell chaining: a spell combining Fortify Magicka 100 / Drain Magicka 3 pts /
Restore Health, recast before expiry, never brings magicka below 100 and can
be chained indefinitely, because the spell costs less than the temporary magicka increase.
Advanced variant: Fortify Intelligence 100 + Fortify Magicka 100 + Drain Magicka 3 pts /120 s +
Drain Intelligence 100 pts /1 s, which raises the max beyond 1500 by successively exploiting
the unclamped negative magicka of Drain Intelligence and then the clamp to zero of
Drain Magicka.
Source: [UESP:Spell Making](https://en.uesp.net/wiki/Oblivion:Spell_Making)

These exploits **structurally depend on negative magicka**. Preset 2 neutralizes them.
This is an accepted side effect, to be documented clearly.

---

## 5. Design debate and decisions

### 5.1 Objection raised then withdrawn

Initial objection: removing the free gift would make Fortify Magicka unusable in combat,
since the remaster's regeneration is decoupled from the max pool.

It is **valid for a mage at Willpower 100** (55 s to fill +300 in combat, versus
buff durations of 3 to 13 s) but **invalid for the reference build**: the preceding Fortify
Willpower brings regeneration to 19/s in combat, i.e. ~16 s to fill +300. The casting
order (Willpower first) is the optimal order, not a coincidence. **Objection withdrawn.**

### 5.2 Argument retained in favor of the "glass" model

An outsized strike deserves a preparation phase. The model turns Fortify Magicka from a
"Restore Magicka 16× cheaper" into a real ritual with a time window and an opportunity
cost. It is a **design gain**, not just a consistency fix.

### 5.3 The cost of preset 2, in numbers

**[CALCULATION]**, from the formula in section 4.4. Writing `D` for the duration of spell 2
(Intelligence + Magicka) and giving the Willpower spell a duration of `D + 5`:

```
Preparation cost ≈ 4.36 × (D+5) + 4.36 × D + 1.09 × D = 9.81 × D + 21.8
```

With a base pool of 300 and 19 pts/s of regeneration in combat:

| `D` = 20 s | Vanilla | Preset 2 |
|---|---|---|
| Preparation cost | 218 | 218 |
| Pool after preparation | 82, then +300 given → **382/600** | **82/600** |
| After refilling (18 s × 19/s) | 600/600 (capped) | **~424/600** |
| Actual ceiling of the big strike | **600** | **~424** |

Two consequences:

1. Preset 2 caps the strike at about **70%** of vanilla, for an identical spell budget.
2. **Budget wall**: the preparation costs ~9.8/s of duration while refilling brings in
   19/s, i.e. a net margin of only ~9/s. At `D ≈ 28.4 s`, the preparation consumes
   all 300 points.

Way out of the wall: increase the base pool through enchantment, or raise Restoration beyond 100
(see section 10.3, interaction with another plugin). Under the "playable naked" constraint, the
ceiling of ~424 is **structural**.

Caveats: personal calculation not verified in game; it is not established that the remaster kept
the cost formula identical; wearing armor degrades these figures.

### 5.4 Design decision

Three presets rather than a single fix, because the subject is a gameplay choice and not
an unambiguous bug. Players choose according to their role-play or gameplay preferences.

| Preset | Activation | Expiry | Impact |
|---|---|---|---|
| **0 — Off** | unchanged | unchanged | plugin inert, log and probe remain active |
| **1 — Vanilla+** | gift kept | floor at zero only | no impact on balance, spell chaining preserved |
| **2 — Glass** | no gift | water kept, overflow spilled | target model; neutralizes chaining and the permanent magicka exploit |
| **3 — Custom** | per `[Magicka]` | per `[Magicka]` | advanced players |

**Preset shipped by default: 2**, with the "what it costs" section at the top of the config file.

Scope: **magicka only**. Health and Fatigue exhibit the same pathology by
construction but are out of scope (future work).

---

## 6. Internal engine model

### 6.1 Structure

**[DEDUCTION]** from the reverse-engineered headers of
[commonlibob64](https://github.com/libxse/commonlibob64/blob/main/include/RE/P/PlayerCharacter.h):

| Field | Offset | Type |
|---|---|---|
| `magicActorValueModifiers[72]` | `0x474` | `float[]` — modifiers of magical origin (spells) |
| `scriptActorValueModifiers[72]` | `0x594` | `float[]` |
| `healthModifier` | `0x6B4` | `float` |
| **`magickaModifier`** | **`0x6B8`** | `float` — damage offset, ≤ 0 |
| `fatigueModifier` | `0x6BC` | `float` |
| `gameActorValueModifiers[72]` | `0x6C0` | `float[]` |
| `sizeof(PlayerCharacter)` | `0xB88` | — |

ActorValue indices: Strength 0, **Intelligence 1**, Willpower 2, Agility 3, Speed 4, Endurance 5,
Personality 6, Luck 7, **Magicka 8**.

Primary vtable indices: **`Update(float)` = `0x099`**, `GetActorValue` = `0x0B0`,
`GetActorFloatValue` = `0x0B1`.

Another useful landmark: `Actor::naturalMagickaRegenPeriod` @ `0x2C8`, `ModifierList scriptModifiers`
@ `0x200`.

### 6.2 The central invariant

**[DEDUCTION to be validated in phase 1]** The current pool is not stored as an absolute value.
The maximum is **derived** (recomputed from effective Intelligence, the race and
birthsign bonuses, and the ActorValue modifiers), and current is a **negative damage offset**:

```
current = maximum + magickaModifier        with magickaModifier ≤ 0
```

### 6.3 Why this explains everything

When an effect changes the maximum, the offset **is not adjusted**. The two
criticized behaviors follow from it arithmetically:

- the max goes up, the offset stays → current mechanically follows → free magicka;
- the max goes down, the offset stays → current can drop below zero.

**Nobody wrote "apply −300 to both pools".** It is a consequence of the
representation, inherited from the Gamebryo architecture. The same pathology exists by construction
for Health and Fatigue, which is a strong hint in favor of this reading.

### 6.4 Refinement lead

**[DEDUCTION to be validated]** Since modifiers are stored **per ActorValue**, monitoring
`magicActorValueModifiers[1]` (Intelligence) and `[8]` (Magicka) separately would make it possible:

- to apply different rules to Intelligence and to Magicka — which is defensible since
  vanilla already treats them differently (section 4.2);
- to **naturally exclude level-ups**, since a permanent attribute gain goes through the
  base value and not through these arrays.

Decision: phase 1 instruments both leads; the choice will be made on the data.

---

## 7. Fix algorithm

### 7.1 Principle

On each change of the maximum, compensate the offset by `−delta` to keep current constant,
then clamp current to `[0, max]`.

### 7.2 Pseudocode

```
inputs: lastMax (max of the previous frame), newMax, modifier, config

delta = newMax − lastMax

if delta not finite or |delta| > fMaxDeltaGuard:
    resynchronize lastMax = newMax, correct nothing      # save load
    exit

mod = modifier

if delta >  epsilon and bPreserveOnIncrease: mod = mod − delta
if delta < −epsilon and bPreserveOnDecrease: mod = mod − delta

# clamping current to [0, newMax]  <=>  mod in [−newMax, 0]
if bSpillOverflow and mod > 0        : mod = 0
if bFloorAtZero  and mod < −newMax   : mod = −newMax

apply = |mod − modifier| > epsilon
newCurrent = newMax + mod
lastMax = newMax
```

### 7.3 Algebraic invariant

When `bPreserveOnIncrease` and `bPreserveOnDecrease` are enabled and no clamping
applies: `newMax + newMod == lastMax + modifier`. The current pool is exactly preserved.

### 7.4 Preset → booleans mapping

| Preset | `bPreserveOnIncrease` | `bPreserveOnDecrease` | `bSpillOverflow` | `bFloorAtZero` |
|---|---|---|---|---|
| 0 — Off | — | — | — | — |
| 1 — Vanilla+ | 0 | 0 | 1 | 1 |
| 2 — Glass | 1 | 1 | 1 | 1 |
| 3 — Custom | read from `[Magicka]` | same | same | same |

### 7.5 Test cases

Implemented and **validated outside the game** (20 assertions, g++ 15.2, Linux) in
`src/MagickaMath.h` + `tests/test_math.cpp`. Simulator: `current = max + modifier`,
the engine recomputes the max without touching the modifier.

| # | Scenario | Expected |
|---|---|---|
| 1 | vanilla: 100/300, fortify +300 | 400/600 (bug reproduced) |
| 2 | vanilla: 100/600, expiry | −200/300 (bug reproduced) |
| 3 | preset 2: 100/300, activation +300 | 100/600 |
| 4 | preset 2: 100/600, expiry | 100/300 |
| 5 | preset 2: 350/600, expiry | 300/300 (overflow spilled) |
| 6 | preset 2: 300/600, expiry | 300/300 (nothing to spill) |
| 7 | preset 2: 10/300, Fortify Int +200 then expiry | 10 unchanged, no negative |
| 8 | preset 2: 200/300, Drain −300 then expiry | 0/0 then 0/300, no gift |
| 9 | inherited negative pool (−500/300), no max change | brought back to 0/300 |
| 10 | safeguard: delta of 1e9 | resynchronization, no write |
| 11 | preset 1: 100/600 after spending, expiry | 0/300 instead of −200/300 |

---

## 8. Plugin architecture

### 8.1 ASI constraint

The plugin is an **ASI**, not an OBSE64 plugin. Reason: OBSE64 is unstable on the reference
gaming machine and causes frequent crashes.

Direct consequence: **no address library is available**. The commonlibob64 address
identifiers (`RE/IDs*.h`) are unusable in ASI mode. The vtable has to be discovered at
runtime.

Secondary consequence: commonlibob64 is **not** a build dependency. Its headers serve only as
offset documentation. The plugin depends only on the Win32 API.

### 8.2 Vtable discovery via RTTI

Sequence, entirely at runtime:

1. Get the game module: `GetModuleHandleA("OblivionRemastered-Win64-Shipping.exe")`, then
   `"OblivionRemastered-WinGDK-Shipping.exe"`, with fallback to `GetModuleHandleW(nullptr)`.
   Size via `GetModuleInformation`.
2. Search for the MSVC decorated name **`.?AVPlayerCharacter@@`** in the readable sections of the image.
3. `TypeDescriptor = addressOfTheString − 0x10` (on x64, the `name` field is at offset `0x10`).
4. Compute its RVA, then search for that RVA as an aligned 4-byte value in `.rdata`. Each
   occurrence is a candidate `pTypeDescriptor` of a `_RTTICompleteObjectLocator`:

   ```
   COL = occurrence − 0x0C
   struct _RTTICompleteObjectLocator {  // x64
       uint32_t signature;         // 0x00, equals 1
       uint32_t offset;            // 0x04, equals 0 for the primary vtable
       uint32_t cdOffset;          // 0x08
       uint32_t pTypeDescriptor;   // 0x0C  (RVA)
       uint32_t pClassDescriptor;  // 0x10  (RVA)
       uint32_t pSelf;             // 0x14  (RVA)
   };
   ```

   Validation: `signature == 1`, `offset == 0` (we want the primary vtable),
   and `pSelf == COL − base`. This last check is a strong validator on x64.
5. Search for a QWORD equal to the COL address in `.rdata`. The occurrence is the metadata
   pointer that precedes the vtable: **`vtable = occurrence + 8`**.
6. Consistency check: `vtable[0]` must point into `.text`.

### 8.3 Hook

**Replacement of vtable slot `0x099`** (`PlayerCharacter::Update(float)`) by a plain pointer
write under `VirtualProtect`.

Deliberate choice: **no MinHook, no trampoline, no external dependency.**
Justifications:

- The hook provides the player's `this` pointer without needing to locate the singleton.
- Another ASI plugin in the same environment (see section 10) uses MinHook; not using it
  removes any risk of contention between two MinHook instances in the same process.

Required robustness:

- **Clean chaining**: call the original pointer read at hook installation time, never a
  hard-coded address. A plugin installed before us remains functional.
- **Periodic integrity check**: at each heartbeat, re-read the vtable slot. If it no longer
  contains our pointer, log it explicitly — another plugin has overwritten it and the fix no
  longer does anything.

Calling conventions:

```cpp
using Update_t = void  (__fastcall*)(void* self, float delta);
using GetAVF_t = float (__fastcall*)(void* self, std::int32_t actorValueIndex);
```

On x64, `__fastcall` is the default convention and is identical between MSVC and GCC.

### 8.4 Hook loop

```
call original(self, delta)

v    = GetActorFloatValue(self, Magicka)          # vtable[0x0B1] — current OR max, cf. 8.4 bis
mod  = *(float*)((char*)self + 0x6B8)

calibrator.Feed(v, mod)
if not calibrator.Resolve(v, mod, cur, max) :
    return                                        # NO write until decided

self-test (first N frames after the verdict) :
    require mod ≤ +epsilon, max > 0, max < 100000, cur and mod finite
    if failure → invariantOk = false, plain-text log, NO memory write, ever

if first frame :  lastMax = max ; return
apply the algorithm of section 7.2
if write needed :
    if bDryRun or !invariantOk → log what WOULD have been written
    else → *(float*)(self + 0x6B8) = newModifier
```

Note: the maximum is invariant under our write. Under hypothesis A, `max = v − mod` and our
write changes `v` and `mod` by the same amount; under hypothesis B, `max = v` and we do not
write `v`. No feedback loop in either case.

### 8.4 bis — Automatic calibration of the read

**The problem.** `GetActorFloatValue(Magicka)` may return **current** magicka or **maximum**
magicka; the commonlibob64 headers expose two neighbouring accessors (`GetActorValue` at
`0x0B0`, `GetActorFloatValue` at `0x0B1`) without documenting these semantics. Both readings
are self-consistent, so static inspection cannot decide:

- hypothesis A: `v` = current → `max = v − mod`
- hypothesis B: `v` = max → `current = v + mod`

Getting it wrong inverts the direction of the entire algorithm.

**The solution: a stability discriminant.** It costs nothing and requires no action from the
player. The maximum is **piecewise constant** — it only moves on a Fortify, a Drain or a
level-up. The modifier, on the other hand, moves constantly: every cast pushes it down,
natural regeneration pushes it back up (11 pts/s out of combat at Willpower 100, section 4.3).

We therefore maintain two candidates for the maximum:

```
candA = v − mod       (the true maximum if hypothesis A is correct)
candB = v             (the true maximum if hypothesis B is correct)
```

On every frame where the modifier changes while the real maximum does not, **the right
candidate stays constant and the other moves by exactly −delta**. We keep score; the stable
candidate wins.

The method is **well-posed**: if `mod` has changed, the two candidates cannot both stay stable
at the same time, since `candA = v − mod` — if `v` is stable then `candA` changes by `−delta`,
and vice versa. An event where **neither** is stable means that `v` and the maximum moved on
the same frame: the event is **ambiguous and is not counted**, rather than misattributed.

Parameters: 8 unambiguous events required, decisive margin of 4, give up after 3600 frames.

**Properties verified by the tests** (`src/HypothesisCalibrator.h`, `tests/test_calibration.cpp`,
28 assertions):

| Property | Result |
|---|---|
| Identifies an engine that exposes current | correct verdict, score 8–0 |
| Identifies an engine that exposes maximum | correct verdict, score 0–8 |
| Works from regeneration alone, with no player action | yes |
| Works from casts alone | yes |
| Speed | **9 frames**, i.e. ~0.15 s at 60 fps |
| A Fortify during calibration | `mod` unchanged → no event, zero pollution |
| Fortify **and** regeneration on the same frame | event detected but not counted |
| Full magicka (nothing moves) | stays `UNKNOWN`, `Resolve()` refuses to answer |
| `NaN` / infinite values | frame ignored with no side effect |
| Verdict forced by the config | not overwritten by calibration |

**Safety guard.** As long as the verdict has not been reached, `Resolve()` returns false and
the hook exits immediately: **no memory write is possible before calibration**. A calibration
failure therefore degrades the plugin to inaction, never to an erroneous write.

**Manual escape hatch.** `iForceHypothesis` in `[Offsets]` (0 = auto, 1 = current, 2 = max)
makes it possible to bypass calibration if a pathological case appears after an update.

### 8.5 Lifecycle

`DllMain(DLL_PROCESS_ATTACH)`: `DisableThreadLibraryCalls`, then `CreateThread` to an
initialization thread. No heavy work in `DllMain` (loader lock). This is the pattern of
Baestus's reference plugin.

Init thread: initialize the log, read or generate the config, inventory the modules, discover
the vtable with bounded retries, install the hook.

### 8.6 Deployment

```
OblivionRemastered\Binaries\Win64\plugins\MagickaPoolFix.asi
or
OblivionRemastered\Binaries\WinGDK\plugins\MagickaPoolFix.asi
```

Requires an ASI loader (Ultimate ASI Loader). The `.conf` file and the `.log` are **next to
the `.asi`**.

**Extension convention**: ASI plugins use `.asi`, OBSE plugins use `.dll`.
The module inventory must look for both but report the extension.

---

## 9. Configuration file

`MagickaPoolFix.conf`, **written in English**, placed next to the `.asi`,
**auto-generated if missing or invalid**. Convention taken from
[Delete Spells - OBSE - ASI](https://www.nexusmods.com/oblivionremastered/mods/4934), which uses
a `.conf` with Hungarian prefixes (`b`, `i`, `f`) and automatic regeneration of defaults.

**The file serves as the readme.** Every option is documented, with its balancing
implications and the source URLs. It is the main channel of explanation to the player,
since the ASI offers no console.

### 9.1 Preset semantics — settled decision

- `iPreset` = 0 / 1 / 2 / 3 (see section 5.4).
- The `[Magicka]` section **is only read if `iPreset = 3`**. A preset 1 or 2 with custom
  values would be incomprehensible to a non-technical player.
- **No `-1` sentinel value.** In preset 3 there is no reference point: "default relative to
  what" has no answer. The `[Magicka]` section is therefore **pre-filled with explicit 0/1
  values matching the behaviour of preset 2**, with a comment stating that they are only read
  in preset 3 and serve as a starting point.
- At startup the log writes the **resolved effective values**, plus an explicit warning if
  `[Magicka]` differs from preset 2 while `iPreset ≠ 3`.

### 9.2 Sections and keys

```ini
[General]
iPreset = 2
bDryRun = 0          ; 1 = compute and log without writing to memory (phase 1)

[Magicka]            ; read only if iPreset = 3
bPreserveOnIncrease = 1
bPreserveOnDecrease = 1
bSpillOverflow      = 1
bFloorAtZero        = 1
fEpsilon            = 0.01
fMaxDeltaGuard      = 100000.0

[Log]
iLevel            = 2          ; 0 Off, 1 Error, 2 Info, 3 Debug, 4 Trace
sFile             = MagickaPoolFix.log
iHeartbeatSeconds = 60         ; 60 minimum
iMaxLines         = 200000     ; 0 = unlimited

[Probe]
bEnabled        = 0
bDumpOnKeyPress = 0
iDumpKey        = 0x79         ; F10, Windows Virtual-Key code (F8 = System menu,
                              ; F9 = Quick Load: both are taken by the game)
iDumpFrom       = 0x400
iDumpTo         = 0xB88

[Offsets]            ; -1 = compiled default value
iMagickaModifier    = -1       ; 0x6B8
iMagicAVModifiers   = -1       ; 0x474
iScriptAVModifiers  = -1       ; 0x594
iGameAVModifiers    = -1       ; 0x6C0
iUpdateVtableIndex  = -1       ; 0x099
iGetActorFloatValue = -1       ; 0x0B1
iForceHypothesis    = 0        ; 0 = automatic calibration (recommended)
                               ; 1 = GetActorFloatValue returns CURRENT magicka
                               ; 2 = GetActorFloatValue returns MAXIMUM magicka
```

The `-1` sentinel **remains legitimate in `[Offsets]`**: the reference point there is explicit
and documented in a comment (the compiled value). It is only removed in `[Magicka]`.

### 9.3 Mandatory documentation content of the file

Header explaining: the engine's internal model, the two corrected behaviours with the
figures, the glass analogy, and a "what it costs" section before `iPreset = 2` restating
the regeneration formulas, the values 11 / 5.5 / 38 / 19 pts/s, the effect on the hit
ceiling and the neutralization of spell chaining. Source URLs included.

---

## 10. Logging

### 10.1 Settled decisions

- **Single file**, next to the `.asi`. **Truncated at every startup, no rotation.** If the
  player has a problem, they check the log before relaunching. Do not clutter the folder with
  files nobody cleans up.
- **Flush after every line.** Non-negotiable: without it, a crash takes away precisely the
  last lines, the ones that matter.
- Five levels, 0 Off to 4 Trace. **The Info level only logs changes**, never per frame — at
  60 fps a state log would be unreadable. Level 4 writes one line per frame and is reserved
  for short diagnostics, protected by `iMaxLines`.
- **Calibration policy per level** (section 8.4 bis). Calibration **always** runs, but it is
  purely in memory: it costs no log lines at the default level.
  - level 2 (Info, default): **a single line**, the verdict — retained hypothesis, scores,
    number of frames, resolved maximum. After that, nothing more on the subject.
  - level 3 (Debug): one line **per calibration event** (about a dozen at most), with the
    two candidates side by side and the attribution of the point. This is where the data for
    both hypotheses lives, and nowhere else.
  - level 4 (Trace): raw per frame, `v`, `mod`, both candidates.
- **Auto-escalation on failure.** If calibration does not conclude within the allotted
  window, the plugin switches **on its own** to level 3 detail for a bounded number of lines
  (200 by default), then falls back to the configured level. The diagnostic is thus captured
  on the first occurrence, without asking the player to modify their config and replay the
  scene. This is the only case where the plugin raises its own log level, and it announces
  this in the log.
- **Heartbeat every 60 s minimum**, configurable, **summarized on a single line**:
  frames, deltas detected, corrections applied, hook integrity. This is what proves that the
  plugin is running in the absence of a console — the main need on the ASI side.
- Summary emitted at each heartbeat rather than at DLL detach, `DllMain(PROCESS_DETACH)`
  not being reliable when the game is closed abruptly.

### 10.2 Startup content

1. Plugin name and version, timestamp.
2. Game module detected, base address, image size.
3. **Resolved effective configuration values**, plus consistency warnings.
4. Discovered addresses: TypeDescriptor, COL, vtable, original slot pointer.
5. **Inventory of loaded modules**, highlighting those coming from the `plugins` folder
   (`.asi` and `.dll`). Makes any potential conflict visible right from the log.
6. Result of the invariant self-test.

### 10.3 In-game content

One line per detected change, containing: timestamp, max before/after, delta, modifier
before/after, current before/after, decision taken, and the values of entries 1 (Intelligence)
and 8 (Magicka) of the three modifier arrays — it is this last part that will make it possible
to decide on differentiated handling.

---

## 11. Probe and re-discovery method

The `[Probe]` section is **disabled by default but kept permanently** in the final version,
not only in phase 1. Goal: make restoring the offsets after a game update trivial.

**Hex dump** of the player structure, configurable range (`0x400` → `0xB88` by default),
triggered by a key. Format: four DWORDs per line with their interpretation as `float`, to
make the diff readable.

**Re-discovery method for `magickaModifier`**:

1. `bEnabled = 1`, `bDumpOnKeyPress = 1`.
2. In game, at full magicka: press the dump key.
3. Cast a spell of known cost, press again.
4. Diff the two dumps. The `float` that dropped by exactly the spell cost **is**
   `magickaModifier`, whatever its new offset. Enter it in `[Offsets]`.

**Method for the per-attribute arrays**: same procedure while activating then deactivating a
Fortify, observing which array entries change.

This replaces a reverse-engineering session with a few minutes of diffing.

---

## 12. Conflict analysis with the other ASI plugins

Reference environment: two other ASI plugins by Baestus are installed.

### 12.1 What each one does, verified at source-code level

**[FACT]** [Mod 2068 — Attribute Scaling Uncapper](https://www.nexusmods.com/oblivionremastered/mods/2068),
source: [OBSE64_Attribute_Scaling_Cap_Remover](https://github.com/Baestus/OBSE64_Attribute_Scaling_Cap_Remover/blob/main/src/AttributesScalarUncapper-ASI.cpp)

Hooks nothing. Scans two AOB patterns and replaces two `CMOVG` instructions (`0F 4F D8`) with
`NOP`s, in the melee and weapon damage formula, once at startup, via
`VirtualProtect` + `memcpy`. Also removes the third damage cap specific to hand-to-hand.

**[FACT]** [Mod 1846 — Skill / Magicka Cost Uncapper](https://www.nexusmods.com/oblivionremastered/mods/1846),
source: [OBSE64_Skill_Damage_Scaling_Cap_Remover](https://github.com/Baestus/OBSE64_Skill_Damage_Scaling_Cap_Remover/blob/main/src/SkillScalarUncapper-ASI.cpp)

Three actions:

- detours `MagickaCostFormula(float BaseCost, int Skill, int Luck)` via **MinHook**
  (`MH_CreateHook`);
- patches `luck_skill_modifier`: `CMOVBE` (`48 0F 46 C1`) → `48 89 C8 90` (`mov rax,rcx ; nop`),
  removing the capping of the skill at 100;
- reads the globals `fMagicCasterSkillCostMult` and `fMagicCasterSkillCostBase`.

Its target is the **price of a spell**, not the pool.

### 12.2 Verdict

**No conflict.** Three arguments:

1. **No address overlap.** They patch instruction bytes in static functions. We replace a
   vtable entry and write a `float` in the player instance. Disjoint locations, no
   overlapping trampoline.
2. **No mechanism overlap.** 1846 uses MinHook. Our direct vtable pointer write, without
   MinHook, eliminates any risk of contention.
3. **No state overlap.** Their scope is "how much a spell costs" and "how much damage a hit
   does". Ours is "how the pool reacts when its maximum changes". They compose in series:
   their formula determines the amount spent, that amount appears in `magickaModifier`, and
   we only touch `magickaModifier` when the **maximum** changes.

### 12.3 Favourable semantic interaction

**Correction of an error in version 1.0 of this document.** There I flagged a risk of a
negative multiplier beyond skill ≈ 117. **This risk does not exist in mod 1846**, and reading
the source code proves it.

The origin of my error: if one simply removed the cap while leaving the linear law
`1.4 − 0.012 × skill` running, it would reach zero at `1.4 / 0.012 = 116.67` and then
become negative — that is, spells that would **give back** magicka.

**[FACT]** That is not what the mod does. Its hook replaces the law beyond 100 with a
hyperbola, source:
[SkillScalarUncapper-ASI.cpp](https://github.com/Baestus/OBSE64_Skill_Damage_Scaling_Cap_Remover/blob/main/src/SkillScalarUncapper-ASI.cpp)

```cpp
if (ModifiedSkill > 100.0f)
    calculatedCost = (Base_Cost * fMagicCasterSkillCostBase) * (100.0f / ModifiedSkill);
else
    calculatedCost = ((1.0f - ModifiedSkill / 100.0f) * fMagicCasterSkillCostMult
                      + fMagicCasterSkillCostBase) * Base_Cost;
```

**[DEDUCTION]** The `else` branch is algebraically identical to the UESP formula
`1.4 − 0.012 × skill` if and only if `fMagicCasterSkillCostMult = 1.2` and
`fMagicCasterSkillCostBase = 0.2`: `(1 − s/100) × 1.2 + 0.2 = 1.4 − 0.012 s`. Verified
numerically, zero difference at skills 0 / 25 / 50 / 75 / 100. These two default values are
therefore deduced from the agreement between the two sources, not read from memory.

Properties of the `> 100` branch (multiplier `0.2 × 100 / skill`):

- **continuous** at the boundary: both branches equal exactly 0.2 at skill 100;
- **strictly positive and decreasing**, asymptotic to zero, never negative or zero;
- **hyperbolic, non-linear** decrease: skill 200 is needed to halve the cost, 1000 to divide
  it by ten. The uncapping therefore has strongly diminishing returns, which is a prudent
  design choice.

**[CALCULATION]** Effect on the budget wall of section 5.3:

| Effective Restoration | Multiplier | Preparation cost | Wall |
|---|---|---|---|
| 100 | 0.200 | 9.81 × D + 21.8 | D = 28.4 s |
| 103.6 (Luck 59, cap removed) | 0.193 | 9.47 × D + 21.0 | D = 29.5 s |
| 117 | 0.171 | 8.38 × D + 18.6 | D = 33.6 s |
| 150 | 0.133 | 6.54 × D + 14.5 | D = 43.6 s |
| 200 | 0.100 | 4.91 × D + 10.9 | D = 58.9 s |

**Concrete point for the reference build.** Since Luck applies **before** the cap
(section 4.4), Restoration 100 + Luck 59 gives an effective skill of 103.6. Mod 1846
therefore **already** brings a gain, a modest one (~3.5% cost reduction), without any skill
needing to exceed 100 on display. Fortifying Luck becomes an indirect lever on the budget
wall.

### 12.3 bis — Real risk identified instead: partial initialization state

**[FACT]** Mod 1846 proceeds in this order: (1) AOB search for `MagickaCostFormula`,
(2) derivation of `luck_skill_modifier`, (3) **write of the byte patch that removes the cap**,
(4) and (5) AOB search for the two cost globals, (6) `MH_CreateHook` then `MH_EnableHook`.

All failure branches of steps 4 to 6 do `MH_Uninitialize(); return false;`
**without reverting the byte patch from step 3**. The author documents this himself at the
end of the file: "the patch to luck_skill_modifier is not automatically reverted here".

There is therefore a partial failure state in which **the cap is removed but the hook that
implements the hyperbolic branch is absent**: the vanilla linear formula then receives a
skill above 100. This is exactly the negative-multiplier scenario.

**[CALCULATION]** Threshold for reaching it: an effective skill > 116.67 is needed, i.e. — at
displayed skill 100 — a **Luck above 92**. At Luck 59, the effective skill of 103.6 would
remain in the positive domain (multiplier 0.076 instead of 0.193: very cheap spells but no
magicka given back). The risk is therefore **out of reach for the current build**, but
becomes real with a substantial Fortify Luck or Luck enchantment.

Consequences for our plugin, all already covered by the design:

- the **module inventory at startup** (section 10.2) makes the presence of 1846 visible;
- this failure mode belongs to 1846, not to us — it is an additional argument in favour of
  the **separation of failure domains** (section 12.4): absorbing its code would make us
  responsible for this state;
- if abnormally increasing magicka ever appears in the logs without a change in the maximum,
  this lead must be examined before suspecting our fix.

**Minor source error to report.** The 1846 README labels the patched instruction
`CMOVG EBX, EAX` while giving the bytes `48 0F 46 C1`, which decode to `CMOVBE RAX, RCX` —
and `48 0F 46 C1` is indeed what the code searches for. The `CMOVG` label and the bytes
`0F 4F D8` are those of mod 2068: very probably a copy-paste between the two READMEs. The
source code takes precedence over the README.

### 12.4 Decision: do not absorb these mods

Five reasons, in order of importance:

1. **Separation of failure domains.** Their AOB patterns break with every game update;
   their READMEs say so explicitly. Absorbing their code would make the magicka fix hostage
   to an obsolete byte pattern. Decisive argument.
2. **License.** None of the three Baestus repositories
   (`OBSE64_Attribute_Scaling_Cap_Remover`, `OBSE64_Skill_Damage_Scaling_Cap_Remover`,
   `OBSE64_Hand_To_Hand_Strength_Cap_Remover`) contains a `LICENSE` file. Without an explicit
   license, redistribution is not clean, even with attribution.
3. **Maintenance.** Baestus maintains his own; an absorbed copy rots.
4. **Scope consistency.** A catch-all mod is harder to document and to get adopted.
5. **Design goal.** A plugin that does one thing and documents it well is better at getting a
   new gameplay rule adopted than a pack.

### 12.5 Residual risk and its coverage

The remaining risk is that **another** plugin, present or future, also hooks
`PlayerCharacter::Update` and overwrites our vtable pointer. Neither 2068 nor 1846 does so.
Coverage: module inventory at startup (section 10.2), periodic hook integrity check
(section 8.3), clean chaining of the original pointer.

Honest caveat: the certainty concerns the mechanism and the address space, established by
reading the source code. It does not extend to untested runtime behaviour — which is
precisely what the inventory and the integrity check make observable.

---

## 13. Build toolchain

### 13.1 Cross-compilation from Linux

The gaming machine has no development environment. The binary is produced by
cross-compilation with **mingw-w64**, validated:

```
PE32+ executable for MS Windows (DLL), x86-64
Imported DLLs: KERNEL32.dll, msvcrt.dll     -> 100% system, no VCRedist required
```

Reference command:

```bash
x86_64-w64-mingw32-g++-posix -std=c++20 -O2 -s -shared \
  -o MagickaPoolFix.asi src/*.cpp \
  -static -static-libgcc -static-libstdc++ \
  -lpsapi -Wl,--exclude-all-symbols
```

Toolchain installation: `apt-get install g++-mingw-w64-x86-64`.

**Why this is possible**: the plugin requires **no game header or asset**.
Unlike an OBSE64 plugin that compiles against commonlibob64, everything is discovered at
runtime. The only build dependencies are the Win32 API provided by mingw.

### 13.2 GCC instead of MSVC: why it is risk-free here

No C++ object crosses the boundary with the game. We read its RTTI data as raw bytes and call
its virtual functions via raw pointers using the x64 convention, identical between MSVC and
GCC on Windows. The MSVC C++ ABI is never consumed.

An MSVC-compatible `CMakeLists.txt` is provided alongside for those who prefer it.
Static runtime in both cases, to avoid any redistributable dependency.

### 13.3 Delivery

Full sources, `CMakeLists.txt`, mingw build script, compiled `.asi`, and its **SHA-256
checksum** so that the binary is reproducible rather than taken on trust.

---

## 14. Phases

**Phase 1 — probe.** RTTI discovery, `Update` hook, invariant self-test, module inventory,
logging of changes and of the modifier arrays, memory dump on key press.
`bDryRun = 1`: the plugin computes and logs what it **would** have done, **without any memory
write**. The player plays for a few minutes with their three-spell sequence and provides the log.

Validation goals:

- confirm or refute `current = max + magickaModifier`;
- confirm offset `0x6B8` and vtable index `0x099`;
- **record the calibrator's verdict** — the "current or maximum" question is no longer a goal
  of manual investigation, it is decided automatically in a fraction of a second
  (section 8.4 bis); the log gives the result and the scores in one line;
- determine whether the modifier arrays make it possible to distinguish Intelligence /
  Magicka sources and to exclude level-ups.

**Phase 2 — fix.** Lifting of the dry run, decision on differentiated handling based on the
data, finalization of the documentary `.conf`.

---

## 15. Limitations and risks

- The offsets come from community reverse-engineered headers, not from official
  documentation. Writing to `0x6B8` on a misidentified structure corrupts the player's memory.
  Phase 1 in dry run is not an excessive precaution.
- The binary is compiled but **cannot be run or tested** on the development side (no
  Windows available). The first launch happens on the player's machine. **Save before.**
- The hook on `Update` implies a one-frame latency on the correction. Invisible in practice,
  but real.
- An unsigned DLL injecting itself into a game process may be flagged by Windows Defender.
- Scope limited to magicka. Health and Fatigue exhibit the same pathology.
- Values marked **[CALCULATION]** are not verified in game, and it is not established that
  the remaster kept the spell cost formula unchanged.

---

## 16. Progress status

**Done and validated**, with no Windows dependency, hence testable outside the game:

| Component | Tests | Status |
|---|---|---|
| `src/MagickaMath.h` — core of the fix | `tests/test_math.cpp`, **20 assertions** | passing |
| `src/HypothesisCalibrator.h` — calibration of the read | `tests/test_calibration.cpp`, **28 assertions** | passing |

Under g++ 15.2 / Linux, with `-Wall -Wextra -Wpedantic`, no warnings.
Coverage: reproduction of the vanilla bug, the four booleans, the eleven scenarios of
section 7.5, and the ten calibrator properties of section 8.4 bis.

**Done**: mingw-w64 cross-compilation toolchain installed and validated on a test DLL.

**To do**: the phase 1 ASI wrapper — log, config, RTTI discovery, hook, self-test,
module inventory, probe and dump.

---

## 17. Validated decisions

| Subject | Decision |
|---|---|
| Plugin type | ASI, not OBSE64 (observed instability of OBSE64) |
| Presets | 4 presets, shipped by default with `iPreset = 2` |
| `[Magicka]` | read **only** if `iPreset = 3`, pre-filled with the values of preset 2 |
| `-1` sentinel | **removed** in `[Magicka]`, **kept** in `[Offsets]` |
| Language of the `.conf` | English, and it serves as the readme |
| Language of discussions | French |
| Log | single file next to the `.asi`, truncated at startup, **no rotation** |
| Flush | after every line |
| Heartbeat | 60 s minimum, summary on **a single line** |
| Invariant self-test | yes, blocks any write on failure |
| Probe | kept permanently in the plugin, inactive by default |
| Other ASI plugins | not absorbed; detection and logging instead |
| Differentiated Int / Magicka handling | instrumented in phase 1, decided on the data |
| Current / maximum ambiguity | **automatic calibration**, no logging of both hypotheses at the default level |
| Data for both hypotheses | level 3 only, with bounded auto-escalation if calibration fails |
| Build | mingw-w64 cross-compilation on the development side, MSVC `CMakeLists.txt` also provided |
| Extension | `.asi` for ASI, `.dll` being the OBSE convention |

---

## 18. References

Game mechanics:

- [UESP: Magicka](https://en.uesp.net/wiki/Oblivion:Magicka)
- [UESP: Willpower](https://en.uesp.net/wiki/Oblivion:Willpower)
- [UESP: Spell Making](https://en.uesp.net/wiki/Oblivion:Spell_Making)
- [UESP: Spell Effects](https://en.uesp.net/wiki/Oblivion:Spell_Effects)
- [UESP: Remastered Changes](https://en.uesp.net/wiki/Oblivion:Remastered_Changes)
- [UESP: Drain Magicka](https://en.uesp.net/wiki/Oblivion:Drain_Magicka)
- [UESP: Luck](https://en.uesp.net/wiki/Oblivion:Luck)

Structures and offsets:

- [commonlibob64 — RE/P/PlayerCharacter.h](https://github.com/libxse/commonlibob64/blob/main/include/RE/P/PlayerCharacter.h)
- [commonlibob64 — repository](https://github.com/libxse/commonlibob64)

Reference plugins and neighbouring plugins:

- [OBSE64_Attribute_Scaling_Cap_Remover](https://github.com/Baestus/OBSE64_Attribute_Scaling_Cap_Remover)
- [OBSE64_Skill_Damage_Scaling_Cap_Remover](https://github.com/Baestus/OBSE64_Skill_Damage_Scaling_Cap_Remover)
- [OBSE64_Hand_To_Hand_Strength_Cap_Remover](https://github.com/Baestus/OBSE64_Hand_To_Hand_Strength_Cap_Remover)
- [Nexus 2068 — Attribute Scaling Uncapper](https://www.nexusmods.com/oblivionremastered/mods/2068)
- [Nexus 1846 — Skill / Magicka Cost Uncapper](https://www.nexusmods.com/oblivionremastered/mods/1846)
- [Nexus 4934 — Delete Spells (model for the .conf file)](https://www.nexusmods.com/oblivionremastered/mods/4934)

## Field journal — first real run (0.1.1, 6 August 2026)

First in-game test on the Steam version. The plugin loaded, produced a complete
log up to the observation loop, and then the process raised a fatal exception
that was intercepted by UE4SS's crash handler
(`Binaries\Win64\ue4ss\crash_2026_08_06_13_18_35.dmp`). The game nevertheless
started afterwards.

### What the log validates [FACT]

RTTI discovery works in full on `OblivionRemastered-Win64-Shipping.exe`,
PE timestamp `0xF19077A4`:

| Step | Result |
|---|---|
| `.?AVPlayerCharacter@@` | 1 occurrence, across 7 data sections examined |
| RTTICompleteObjectLocator | 5 candidates, 1 kept after consistency check |
| vtable | RVA `0x861CBA0`, **201 valid leading entries** |
| slot `0x099` `Update(float)` | RVA `0x65FD5B0` |
| slot `0x0B1` `GetActorFloatValue` | RVA `0x66060C0` |

The 201 valid entries exceed the required minimum (`0x0B1 + 8 = 185`), which
establishes that the vtable is long enough for both target indices. This validates
the discovery chain structurally, not the semantics of the slots: the latter
still has to be confirmed by the calibrator (`bCallVirtuals = 1`).

The module inventory shows 128 modules, including `UE4SS.dll`,
`AttributesScalerUncapper.asi` (mod 2068) and `SkillsScalerUncapper.asi`
(mod 1846) — the two mods whose conflict analysis is documented above —
as well as `DeleteSpells.asi` and `NLTagRemover.asi`.

**Correction of an erroneous claim.** From reading this inventory alone,
I concluded that the ASI loader was UE4SS. That was wrong, and the methodological
fault is mine: the inventory only logged module *names*, and I inferred from the
presence of `UE4SS.dll` a causality that was not there. The full paths extracted
from the memory dump show **two independent loaders**, each with its own proxy:

| Proxy in `Binaries\Win64\` | Size | Product | Loads |
|---|---|---|---|
| `version.dll` | 1192 KiB | Ultimate ASI Loader | the 5 `.asi` files in `plugins\` |
| `dwmapi.dll` | 144 KiB | UE4SS | `ue4ss\UE4SS.dll` (23.9 MiB) |

The system copies of `version.dll` (44 KiB) and `dwmapi.dll` (188 KiB) are
also loaded from `C:\Windows\System32`, which is the normal behaviour of a proxy
that forwards exports to the real library.

Our `.asi` is therefore indeed loaded by Ultimate ASI Loader, as intended. UE4SS
serves other mods and does not concern us — except that it installs a global
crash handler, which explains the dialog box and the `.dmp`: it was UE4SS that
intercepted and wrote out our fault, not UE4SS that caused it.

**Tooling fix [FACT]**: `LogModuleInventory` now logs the full path of every
module outside the Windows directory. A loader installed as a proxy carries, by
construction, the name of the system library it replaces, so the name alone
cannot identify it — only the directory can. The error above was made possible
by this gap.

### Cause of the crash [DEDUCTION]

The log stops exactly at the `---- observation ----` banner at 2046 ms, and the
file ends cleanly (output is flushed after every line). The memory dump is
timestamped 13:18:35 UTC, i.e. 15:18:35 Paris time, so less than one second after
that last line. The only code reachable in that interval is the first call to
`FindPlayerInstance`, and it is the only massive unprotected read in the program:
a walk through 668 MB of private read-write memory by direct dereferencing.

A region reported as *committed* by `VirtualQuery` can be freed by the game a few
microseconds later; reading it then raises an access violation. The MinGW-w64
build offers no SEH, so this fault is unrecoverable. Walking 668 MB of live heap
from a foreign thread made the race nearly certain rather than merely possible.

The code comment claimed to re-check each region "just before touching it".
That was false: the check queried the snapshot the region came from, so it was a
tautology with no protective effect.

### Confirmation from the memory dump [FACT]

The deduction above is now an established fact. Analysis of the minidump
(`crash_2026_08_06_13_18_35.dmp`, 32 MB, 129 modules):

| Field | Value |
|---|---|
| Exception code | `0xC0000005` `EXCEPTION_ACCESS_VIOLATION` |
| Operation | **read** |
| Instruction address | `0x00007FFC718A3CF8` |
| Owning module | `plugins\MagickaPoolFix.asi`, base `0x7FFC718A0000`, **RVA `0x3CF8`** |
| Faulting address | `0x00000230D5BAC000` — in no module |
| Thread | `0xAF30`, distinct from the main thread |

The faulting address is page-aligned (`...C000`) and located in the heap:
it is a page that stopped being valid, not a corrupted pointer.

Rebuilt 0.1.1 from the repository history, with the same compiler options, then
resolved the RVA: `0x3CF8` falls in
`(anonymous namespace)::InitThread(void*)`, `FindPlayerInstance` having been
inlined by `-O2`. Disassembly at that exact address:

```
254b33cf8:  48 3b 38        cmp    (%rax),%rdi     <-- faulting instruction
254b33cfb:  75 0a           jne    254b33d07
254b33cfd:  83 c3 01        add    $0x1,%ebx        candidatesOut++
254b33d00:  4d 85 c9        test   %r9,%r9
254b33d03:  4c 0f 44 c8     cmove  %rax,%r9         if (!first) first = p
254b33d07:  48 83 c0 08     add    $0x8,%rax        ++p
254b33d0b:  4c 39 c0        cmp    %r8,%rax
254b33d0e:  72 e8           jb     254b33cf8        loop
```

`cmp (%rax),%rdi` is the literal translation of `if (*p != g_vtable)`, with
`%rax = p` and `%rdi = g_vtable`. The fault is therefore exactly the dereference
in the inner loop of the heap scan, on a page freed between the `VirtualQuery`
snapshot and its read. Diagnosis confirmed down to the instruction.

### 0.2.0 fixes [FACT]

1. **`mpf::ReadMem`** — every read of game memory goes through
   `ReadProcessMemory` on our own process handle. The kernel probes the source
   range, so an unmapped page makes the call fail instead of faulting in our
   thread. No SEH needed, which is precisely the constraint of a MinGW build.
   Cost: one system call per read, negligible at our rate.
2. **Primary search in the static sections** — since the player is a singleton,
   a pointer to it resides in a static slot in the executable's writable
   sections. We search there for a word pointing to an object whose first field
   is the `PlayerCharacter` vtable. About 8 MB backed by the image, never
   decommitted, so no race is possible. Additional benefit: we obtain the address
   of the slot itself, fixed for a given build, which would allow a later version
   to reach the player without any scan at all.
3. **`bHeapScan = 0`** — the old heap scan remains as a fallback, disabled by
   default, now split into chunks and backed by `ReadProcessMemory`: a page that
   disappears costs one skipped chunk, no longer the process.

The `MemRegions` snapshot is kept, but its role is explicitly downgraded to that
of a free pre-filter used to avoid needless system calls. It never makes a read
safe, and the code comments now say so.

The RTTI discovery pass remains the only direct reader, rightly so: it reads only
the mapped sections of the game module, committed for the whole lifetime of the
process, and each access is bounded to the section it comes from.

## Field journal — second real run (0.2.0-probe, 6 August 2026, 15:35)

First run **without a crash**. 435-line log, `bCallVirtuals = 0`, one press of
F10 at 58,735 ms, instance lost at 63,079 ms (save game load).

### Measured facts

| Item | Recorded value |
|---|---|
| Player instance | `0x1E66BAC73C0`, found at 8,235 ms |
| Static slot | `0x7FF6B7EEF708`, RVA `0x941F708` |
| Scan | 7.3 MB, 70,421 pointers tested, **only 1** object with this vtable, 0 unreadable blocks |
| Samples | 3,059 in 58 s (~52/s for `iPollIntervalMs = 16`) |
| Rescans / losses | 3 / 0 |
| `magickaModifier` at `+0x6B8`, t = 8,250 ms | `0.0000` |
| `magickaModifier` at `+0x6B8`, t = 58,735 ms | `-529.6940` |
| `magickaModifier` at `+0x6B8`, t = 62,047 ms | `-450.45` |

The 0.2.0 RPM fix works: zero unreadable blocks, zero crashes, and the search
through static sections finds a single candidate in 7.3 MB instead of 668 MB of
heap.

### Central result: `+0x6B8` is indeed a magicka deficit

This is the first **positive** evidence for the invariant `current = maximum + modifier`,
`modifier ≤ 0`:

- before the save game is loaded: exactly `0.0`, like the rest of the object;
- after casting spells: `-529.69`, therefore **negative**, as the model predicts;
- 3.3 s later: `-450.45`, i.e. `+79.24` — the value **climbs back towards zero** while
  magicka regenerates.

[DEDUCTION] A field that is zero at full magicka, becomes negative after spells
and climbs back towards zero at the regeneration rate is a deficit, not an absolute
value. The sign and the direction of change are consistent with the spec's hypothesis.

**Unresolved point**: the measured rate is `79.24 / 3.312 = 23.9` points/s.
The Remastered formula for Willpower 100 gives `2×(100/50)² + 1.5×(100/50) = 11`
points/s (https://en.uesp.net/wiki/Oblivion:Willpower), i.e. 5.5 in combat. The gap
is a factor of ~2.2 and is not explained. Two measurements taken 3.3 s apart
with an active Willpower modifier are not enough to settle it; this is precisely
what the 0.3.0 two-press protocol makes it possible to measure cleanly.

A `-529.69` also implies a maximum magicka above 529, so well beyond
the base 300 (Int 100×2 + 50 Breton + 50 Mage). Consistent with the modifiers
recorded below, but not verifiable without `bCallVirtuals = 1`.

### Strong validation of the `magicActorValueModifiers` table at `+0x474`

Indexing `+0x474 + 4×index` with Oblivion's ActorValue enumeration
(0 Strength … 7 Luck, 8 Health, 9 Magicka, 10 Fatigue, 11 Encumbrance, 12 Armorer … 32 Speechcraft):

| Offset | Index | ActorValue | Value |
|---|---|---|---|
| `0x474`–`0x480` | 0–3 | Strength, Intelligence, Willpower, Agility | `+110` each |
| `0x484`–`0x490` | 4–7 | Speed, Endurance, Personality, Luck | `+10` each |
| `0x494` | 8 | Health | `0` |
| `0x498` | 9 | **Magicka** | `+100` |
| `0x49C` | 10 | Fatigue | `+100` |
| `0x4A0` | 11 | Encumbrance | `-99.55` |
| `0x4C8`–`0x4D8` | 21–25 | **Conjuration, Destruction, Illusion, Mysticism, Restoration** | `+5` each |

[DEDUCTION] This table is confirmed, and the decisive argument is the last row:
five consecutive floats equal to exactly `5.0` fall on the five indices that
correspond to the character's schools of magic, and `Encumbrance = -99.55` is the
signature of a Feather effect. The probability that this alignment is coincidental
is negligible. This simultaneously validates the `0x474` offset deduced from
commonlibob64 and the fact that the object found is indeed the player's
`PlayerCharacter`.

### Unidentified block at `+0x0B14`–`+0x0B64`

21 consecutive floats, non-integer, between 15.0 and 181.2. The count matches
exactly the 21 skills of Oblivion, and three groups of exact duplicates to
four decimal places appear (`108.3921` twice, `88.4500` twice, `116.4728`
three times), which rules out noise or independent counters.

But the mapping **is not** a function of the skill value alone:
the 4th entry is `127.22` while the 10th is `116.47`, yet the skill expected
in 4th position is lower than the one expected in 10th. No conclusion is
therefore drawn here. Two skill descaling mods are loaded in the process
(`SkillsScalerUncapper.asi`, `AttributesScalerUncapper.asi`) and could write
to this area; that is a lead, not a finding.

### Two probe defects, fixed in 0.3.0

1. **The manual float watch could not display anything.** `Sample()` consumes each
   delta at the moment it reports it. The periodic poll runs every 16 ms
   at `Debug` level, and the F10 press is handled in the *same* iteration,
   right after. At the default log level (`iLevel = 2`, Info) the poll's changes
   are therefore invisible, and the manual dump arrives on a baseline that has
   already been updated: structurally, nothing was left to display. That is exactly
   what is observed in the log — not a single `FLOATWATCH` line between the
   `manual dump` separator and the hexdump. 0.3.0 adds `SampleManual()` with its
   **own** baseline, refreshed only on a manual dump: a press now answers
   "what has moved since the previous press".
2. **The initial snapshot captured an empty object.** The instance exists from
   8.2 s, well before the save game has finished loading: the initial hexdump is
   almost entirely zero, `+0x6B8` included. 0.3.0 triggers an additional
   automatic `object populated` dump the first time `magickaModifier` stops being
   zero.

Added along the way: `LogPlausibleFloats()` lists, after each dump, only the
offsets carrying a non-zero, plausible value — the useful information from a dump
fits on one screen instead of 120 lines of mostly-zero hexadecimal.

`bCallVirtuals` now defaults to `1`: step 2 of the protocol has been passed on
the Steam executable `OblivionRemastered-Win64-Shipping.exe` with PE timestamp
`0xF19077A4`. The `.conf` comment states that it must be set back to `0` on any
other build.

### Protocol for the next run

1. Load the save game, wait until the log shows `---- object populated ----`.
2. **First F10 press** at full magicka — sets the comparison baseline.
3. Cast an expensive spell, then **second F10 press** immediately.
4. Wait for full regeneration, **third F10 press**.

The diffs between presses 1→2 and 2→3 isolate the offsets that track magicka,
and the calibration verdict (`bCallVirtuals = 1`) settles the
current/maximum question for `GetActorFloatValue`.

## Numeric reconciliation of the 0.2.0 run (spell scenario as reported)

Spells cast during the run: Fortify Willpower, then Fortify Intelligence +
Fortify Magicka, then "a series of ordinary spells". The 60 s buff had
**not** expired before the F10 press.

Direct arithmetic consequences, from the modifiers recorded at `+0x474`:

| Term | Calculation | Value |
|---|---|---|
| Base maximum | Int 100×2 + 50 Breton + 50 Mage | 300 |
| Contribution of Fortify Intelligence `+110` | 110 × 2 | +220 |
| Contribution of Fortify Magicka `+100` | direct | +100 |
| **Maximum during the run** | | **620** |
| Modifier recorded at F10 | | −529.69 |
| **Deduced current** | 620 − 529.69 | **90.31** |

[DEDUCTION] This answers the legitimate surprise "my magicka was never
negative": it is not current magicka that is negative, it is the
**modifier**, which measures the gap to the maximum. Current stayed at ~90 out of 620,
so positive. Nothing abnormal.

And above all, the numbers add up: as soon as the two fortifications are
applied, the maximum rises by 320 without a single drop being added to
current, which mechanically deepens the modifier by −320 even before the cost of the
spells. The remaining 210 points (300 full → 90.31, minus regeneration) are the
cumulative cost of the three fortifications and the series of ordinary spells. The
order of magnitude is consistent, which constitutes a second independent validation of
the invariant `current = maximum + modifier`.

### The regeneration anomaly is resolved

The ~2.2 factor noted above came from a mistake on my part: I had used
**base** Willpower (100) while Fortify Willpower `+110` was active. With an
effective Willpower of 210 and the Remastered formula
(https://en.uesp.net/wiki/Oblivion:Willpower):

- out of combat: `2×(210/50)² + 1.5×(210/50)` = **41.58** pts/s
- in combat (half): **20.79** pts/s

The measured value, 23.9 pts/s, lies just above the in-combat value instead
of being 2.2 times the base value. The residual gap is not settled for lack of
knowing the combat state at that moment, but the factor-2 anomaly no longer exists.

### The critical moment was not captured

The event this mod exists to correct is the **expiry** of the
fortification: the maximum drops back from 620 to 300 and the question is whether the
game spills only the surplus or overwrites current. Since the run stopped
before the 60 s were up, that moment is not in the log. It is the most
important measurement still to be made.

## Field journal — third run (0.3.1-probe, 6 August 2026, 16:04)

`iLevel = 3`, `bCallVirtuals = 1`. 320-line log stopping at 8,250 ms. The player
then played for several minutes and performed the complete cycle 1: **the game did not
crash, but no line was written after 8,250 ms.**

### Exact location of the thread's death

Last line written: `FLOATWATCH primed: 482 floats in 0x400..0xB88 (poll)`.
In the order of the 0.3.1 loop body, the next unconditional statement
is the call `(*getFn)(g_player, kAV_Magicka)` — the very first call to a virtual
function of the game. No heartbeat at 60 s, no manual dump, nothing.

This is not a buffering artefact: `Logger::Write` calls `fflush` unconditionally
after **every** line. The last line written is therefore indeed the last line
reached.

Also absent: `---- object populated ----`. At 8,250 ms, `LogPlausibleFloats`
reports **7 non-zero offsets out of 482** and `magickaModifier = 0`: the player object
was still empty, the save game had not been loaded. 0.3.1 therefore called a
game method on a half-initialised object, 8 seconds after the process was
launched, from a thread whose existence the game is unaware of.

### Two possible failure modes, not discriminated

1. **The call faults and something swallows the fault.** The process survived, so if
   an access violation occurred, an exception filter intercepted it — UE4SS
   installs one — and our thread was unwound or abandoned.
2. **The call blocks indefinitely**, because a game method entered from a
   foreign thread waits on a lock held by the game thread.

The log does not allow a decision: in both cases the trace stops at the same
place, with no dump and no dialog. [DEDUCTION] The complete absence of a crash on the
game side makes mode 2 more likely, but this is not established.

**Practical consequence**: vtable index `0x0B1` is not validated. The 201
valid entries counted during RTTI discovery prove that the vtable is indeed that
of `PlayerCharacter`; they do not prove that slot 0x0B1 is
`GetActorFloatValue` on this build.

### 0.4.0 fixes — `VirtualCallGuard.h`

A timeout does not detect mode 1 and an exception handler does not detect
mode 2. The two are therefore handled separately:

1. **Gate 1 — populated object.** No virtual call before `magickaModifier` has
   stopped being exactly zero. This condition alone would have been enough to avoid
   the incident.
2. **Gate 2 — settle delay.** 3,000 ms after population, to let the
   save game load finish.
3. **Expendable thread + timeout.** The first call is made on a throwaway thread,
   waited on for 2,000 ms. If it blocks, the observation loop keeps
   sampling and logging: a hang costs one thread, nothing more. Only one
   attempt per session; on failure, virtual calls are abandoned for
   the session.
4. **On-disk sentinel.** `MagickaPoolFix.vcall` is written with
   `FILE_FLAG_WRITE_THROUGH` then `FlushFileBuffers` **before** the attempt, and
   deleted after it returns. If it survives until the next launch, the previous
   attempt never returned and `bCallVirtuals` is forced to `0` for this build,
   with an explicit error message. Deleting the file re-arms the attempt.
   The synchronous write is essential: without it the sentinel would be lost
   exactly as a log line would have been.

The result of the call is only read if the worker has set its `done` flag via
`InterlockedExchange`: a call that returns after the timeout has expired cannot
feed the calibration.

**Deliberate note**: no `TerminateThread` on a blocked thread. If it holds or
is waiting on a game lock, killing it can leave that lock held permanently and bring
down the game. Leaking a blocked thread is the lesser evil.

### What the incident does not call into question

Memory reading is not affected: 3,059 samples in the previous run, zero
unreadable blocks, zero faults. All of the project's usable data — the deficit at
`+0x6B8`, the modifier table at `+0x474` — comes from memory observation,
which does not depend on `bCallVirtuals`.

---

## Field journal — 0.4.0-probe, 6 August 2026, 16:15

Decisive run. **Simple game launch, no test protocol executed**: loading
a save game, about thirty seconds of passive play, then a "Fatal Error" box
from UE4SS. No spell cast during the measured interval. This point is important: the
regeneration measurement below is therefore purely passive.

### 1. The safeguard worked, and it produced the first complete diagnosis

| Timestamp | Event |
|---|---|
| 8,172 ms | player instance `0x1F46CF56700` via the static slot |
| 25,500 ms | `---- object populated ----`, 26 of the 482 floats non-zero |
| 28,500 ms | guarded call attempt, budget 2,000 ms |
| 28,500 ms | **UE4SS writes `crash_2026_08_06_14_15_49.dmp`** (14:15:49 UTC = 16:15:49 Paris) |
| 30,500 ms | budget expired, virtual calls abandoned for the session |
| 55,281 ms | last line of the log |

The to-the-second match between the moment of the call and the writing of the minidump
establishes the causal chain. Minidump verdict (`tools/minidump-triage.py`):

- thread `44228 (0xACC4)`, `0xC0000005 EXCEPTION_ACCESS_VIOLATION`, operation **write**
- faulting address **`0x0000000000000000`**
- exception address `0x00007FF6B305F57C`, module base `0x7FF6AEAD0000` → **RVA `0x458F57C`**
  in `OblivionRemastered-Win64-Shipping.exe`, PE timestamp `0xF19077A4`
- **not** in `MagickaPoolFix.asi`

The faulting RVA `0x458F57C` **does not match** the RVA `0x66060C0` expected for slot
`0x0B1` (see the RTTI table above). Without a usable stack unwind, it is not possible to
distinguish "the slot is not `GetActorFloatValue` on this build" from "`0x66060C0`
called a helper function that faulted". [DEDUCTION] In both cases the operational
conclusion is the same: the function dereferences a global context that is non-null
only on the game thread.

**Design consequence, final:** a guard timeout survives a hang, nothing
survives a fault. An access violation belongs to the whole process. `bCallVirtuals`
therefore becomes **0 by default** in 0.5.0, with the demonstration cited in the `.conf`.

The positive point: the observation loop survived 25 seconds after the fault and
kept logging normally. That is exactly the behaviour targeted in 0.4.0.

### 2. Exact validation of the regeneration formula, and of `+0x06B8`

Two floats, and only two, move during passive regeneration:

- **`+0x06B8`** rises from `-29.0000` towards zero
- **`+0x08E0`** counts down from `3.0000` towards zero

The ratio of the two deltas, measured over **103 pairs** of samples matched to the same
millisecond, is constant: **12.979 to 12.982**, i.e. 102 pairs out of 103 within a
window of ±0.005. [DEDUCTION] `+0x08E0` is therefore a **countdown expressed in
seconds**, which makes it a directly usable game clock, and the ratio is
a **rate in magicka points per second: 12.98 pts/s**.

Now, the magic modifier table gives, in this run, `+0x0474` to `+0x0490` all at
`+10.0000` — i.e. a passive bonus of +10 on all eight attributes, hence **effective
Willpower 110**. The Remastered formula `2×(Wp/50)² + 1.5×(Wp/50)`
([UESP](https://en.uesp.net/wiki/Oblivion:Willpower)) gives for Wp = 110:

\[ 2 \times 2.2^2 + 1.5 \times 2.2 = 9.68 + 3.30 = \mathbf{12.98} \]

**Exact agreement to four significant figures.** This simultaneously validates three
independent things: the regeneration formula as documented does apply to the
Remastered, it uses **effective** Willpower including modifiers, and `+0x06B8` is
indeed the magicka deficit expressed in points.

This corrects the earlier measurement of 23.9 pts/s in the 0.2.0 journal, obtained from two
heartbeats 3.3 s apart during the expiry of a buff: it is abandoned in
favour of this one, measured frame by frame on a stable state.

### 3. Decisive negative result: current magicka is not stored

Over the 24 seconds of regeneration, in the **entire** `0x400..0xB88` window, only
`0x06B8`, `0x08E0` and `0x0804` (14 times) changed. If current magicka were stored
as a float in this window, it would necessarily have moved on every frame, since
the pool was visibly filling.

[DEDUCTION] **Current magicka is not a stored field.** The engine keeps only the
**deficit** — `magickaModifier` at `+0x06B8`, always ≤ 0 — and computes
`current = maximum + deficit` on demand, the maximum being derived from Intelligence.
This is Oblivion's classic attribute damage model, and it is the only explanation
compatible with a pool that fills while moving only a single float.

The planned `InvariantHunter` — a search for a pair of offsets `(max, current)` satisfying
the relation — is therefore **removed**: it was looking for a field that does not exist.

### 4. The model that replaces everything, and it is simpler

The mod never needs to locate "current" or "maximum". It needs the
**change** in the maximum, and that is fully readable:

\[ \Delta\text{max} = 2 \times \Delta(\text{Intelligence modifiers}) + \Delta(\text{Magicka modifiers}) \]

The factor 2 is the derivative of `max = 2×Int + race + birthsign`
([UESP](https://en.uesp.net/wiki/Oblivion:Magicka)); flat bonuses cancel out in
any difference. Each term on the right is the sum of the three modifier
tables — magic `+0x474`, script `+0x594`, game `+0x6C0` — at the relevant ActorValue
index. Compensation then reduces to adjusting **a single field**, `+0x06B8`, based
on neighbouring fields.

### 5. Correction of an error in the code

`kAV_Magicka` was **8** up to 0.4.0: that is **Health**. Magicka is **9**. The correct
alignment is established by the dump: index 11 carried `-99.55` (unmistakable signature
of a Feather effect on Encumbrance) and indices 21 to 25 carried exactly `+5.0` each,
falling on Conjuration, Destruction, Illusion, Mysticism and Restoration — the five schools
actually fortified. This alignment fixes the whole enumeration. `kAV_Health`,
`kAV_Fatigue` and `kAV_Encumbrance` are added along the way.

### 6. What 0.5.0 measures

`src/ModelWatch.h` watches the two modifier totals and logs, at each
transition, the predicted change in the maximum against the change actually applied
to the deficit. Three possible outcomes, discriminated by `ClassifyMaxChange()` in
`src/MagickaMath.h` (14 dedicated assertions in `tests/test_math.cpp`, 62 in total):

| Observation | Interpretation |
|---|---|
| `Δdeficit = −Δmax` | pool **preserved**: nothing to correct, the engine already does the work |
| `Δdeficit = 0` | pool **not preserved**: the whole of `Δmax` arrives as free magicka — this is the bug |
| other | proportional or partial rule: assume nothing, analyse the numbers |

The tolerance is `epsilon + 2% × |Δmax|`: a Fortify Intelligence 110 moves the
maximum by 220 points, and a fixed window to the hundredth would classify every large fortify as
"partial".

**A single Fortify Intelligence spell is enough to settle it.** It is the last unknown of the
project.

### 7. Binary

`MagickaPoolFix.asi` 292,864 bytes,
`e97fbc8aa7917a6398124c96866e590e64137248f2d3a8fde6f54128b095b419`.
PE32+ DLL x86-64, 9 sections, imports `KERNEL32.dll`, `msvcrt.dll`, `USER32.dll`
only, no exports, static CRT, 0 warnings with `-Wall -Wextra -Wpedantic`.
Built but **not executed**: I cannot run a Windows binary here.

---

## Fix 0.5.1 — the `.conf` shipped in 0.5.0 was wrong

Reported by the user: an orphaned sentence in the `.conf`, of the form
`bCallVirtuals = 0 and relaunch. That second run is what lets`.

The cause is a regex-based cut made in 0.5.0. The replacement of the protocol paragraph
stopped at the **first** occurrence of the string `bCallVirtuals = 1`, which sat in the
middle of a sentence of the numbered protocol ("3. Only then set
bCallVirtuals = 1 and relaunch.") and not on the setting line. Consequences:

1. a sentence fragment was left in the file, and
2. **the actual setting line was still `bCallVirtuals = 1`** — that is, 0.5.0 shipped
   by default exactly the option that the 6 August 2026 journal shows makes the game
   fault.

The packaging assertion `re.search(r'bCallVirtuals\s*=\s*0', conf)` was satisfied by
the orphaned fragment. It tested for the presence of a substring, not the value of the
setting.

**Assertion replaced by a structural check.** Every line of the `.conf` that is neither
empty, nor a `;` comment, nor a `[Section]` header must be an assignment with a
**single-token value**, optionally followed by a `;` comment:

```
^[A-Za-z_]\w*\s*=\s*[^\s;]+\s*(;.*)?$
```

Prose that has escaped from a comment block shows up as a key whose value is a
sentence, hence multi-token: the check rejects it. The sensitive values are then read
by an accessor that requires **exactly one** setting line per key and compares the
extracted value: `bCallVirtuals = 0`, `iDumpKey = 0x79`, `bHeapScan = 0`,
`iPreset = 2`.

The documentation paragraph is rewritten to state the truth established in the
field — hang in 0.3.1, write fault at the null address in 0.4.0, hang again in 0.5.0
on the same build — and to conclude explicitly that the option must stay at 0. The
earlier wording "leaving this at 1 is safe to try" was false and has been removed.

Binary: `MagickaPoolFix.asi` 292,864 bytes,
`ac4a3245b5f7f9158055fd9734bf90ab039477e365de773785b40e546469afa5`. 62 assertions,
0 failures, 0 warnings. Built but not run.

---

## Field journal — 0.5.1-probe, 6 August 2026, 16:46 → 16:49

Earlier save reloaded, the 320 magicka found again. The protocol was followed and
**it produced the expected event**, but with two deviations, both of which are defects
in my tooling, not in the game.

### 1. `MODELWATCH primed` did not appear on load — priming gated on the wrong condition

Observed timeline:

| timestamp | line |
|---|---|
| 8,234 ms | `FLOATS initial snapshot: 10 of 482` then `FLOATWATCH primed` |
| 62,031 ms | `alive … modifier=0.00 … watch=597` |
| 122,031 ms | `alive … modifier=0.00 … watch=597` |
| 129,562 ms | `---- object populated ----`, `FLOATS populated: 60 of 482` |
| 129,562 ms | `MODELWATCH primed: Int 110.0000, Magicka 100.0000, deficit -270.0000` |

Priming waited **129.5 seconds**, that is, until the moment the user cast their spell.
The cause is in `Plugin.cpp`: the `dumpedPopulated` gate was `mod != 0.0f`, meaning
"the deficit has stopped being zero". But **a full pool legitimately has a zero
deficit**. The most ordinary case in the game — loading a save with magicka at
maximum — therefore never passed the gate. The counter `samples=6428` shows the loop
was running correctly; it was the criterion that was wrong.

That criterion had worked in the previous runs only because the deficit happened to be
non-zero there by accident.

**Criterion replaced** by the number of non-zero, plausible floats in the window
`0x400..0xB88`, a quantity independent of the fill state. The four field runs give 10
out of 482 for an empty object, and 25, 26 then 60 for a populated object: the threshold
`kPopulatedFloatFloor = 18` sits between the two clusters. The `mod != 0` condition is
kept as an OR alternative; it costs nothing.

### 2. The 0.5.1 verdict was wrong in this specific case

The event, at 187,312 ms, is the expiry of the double spell:

```
Intelligence modifier total :   110.0000 ->    10.0000  (delta -100.0000)
Magicka modifier total      :   100.0000 ->     0.0000  (delta -100.0000)
predicted change in maximum : -300.0000  (= 2 x -100.0000 -100.0000)
deficit at +0x06B8          :     0.0000 ->     0.0000  (delta +0.0000)
```

**The model is validated.** Both modifier totals moved by exactly the magnitude of the
spells cast, and `Δmax = 2 × ΔInt + ΔMagicka` is indeed the right prediction formula —
it is the derivative of `max = 2 × Int + race + birthsign`
([UESP Magicka](https://en.uesp.net/wiki/Oblivion:Magicka)). The change from 110 to 10
for Intelligence also confirms the permanent `+10` from equipment already measured.

**But the printed verdict was wrong.** 0.5.1 concluded "pool NOT preserved … On an
increase this is free Magicka, which is the bug this mod exists to fix". Yet:

- the deficit was **0 before** the transition, so the pool was **full**;
- the maximum **dropped** by 300;
- deficit unchanged at 0 ⇒ `current = max + 0 = max`, the pool is **still full**.

This is exactly the desired "glass-of-water" behavior on a decrease: the water above
the new rim is precisely the water that must be lost. **This case is not the bug, it
is the correct behavior.** My classifier only looked at the deficit's response,
without taking into account the direction of the change or the state of the pool
before.

`ClassifyMaxChange` now takes a fourth argument, the deficit before, and returns
five distinct verdicts instead of three:

| verdict | condition | meaning |
|---|---|---|
| `kPreserved` | `Δdeficit ≈ −Δmax` | current did not move — correct on an increase |
| `kSpilledFromFull` | `Δdeficit ≈ 0`, `Δmax < 0`, deficit before ≈ 0 | overflow spilled — **correct** |
| `kFreeMagicka` | `Δdeficit ≈ 0`, `Δmax > 0` | free magicka — **bug** |
| `kLostMagicka` | `Δdeficit ≈ 0`, `Δmax < 0`, deficit before ≠ 0 | magicka stolen with no overflow — **bug**, and the more harmful half |
| `kPartial` | anything else | conclude nothing |

`kPreserved` is tested first so that `Δmax = 0` with `Δdeficit = 0` is classified as
trivial preservation rather than as a spill of zero points.

### 3. What this run proves and what it does not

Proven: the prediction formula, the three modifier arrays, the reading of the
deficit, and the fact that **a decrease from a full pool behaves correctly**.

Not proven, and this is where the presumed bug still lies:

1. **increase with a partially empty pool** — cast the fortify spell while magicka is
   low. Does the deficit deepen by `−Δmax` (preserved, correct) or does it stay frozen
   (`kFreeMagicka`)?
2. **decrease with a partially empty pool** — let it expire while magicka is well below
   the inflated maximum. A frozen deficit gives `kLostMagicka`.

Today's run is the only one of the four possible cases where a frozen deficit and
correct behavior coincide. It should therefore have been tested last, not first.

### 4. Discontinuity detection

A save load rewrites the whole object between two polls, which is indistinguishable
from a fortify effect of several hundred points. Three guards replace the former
reliance on the deficit:

- `Discontinuity()` is called when the player pointer is rescanned — the log shows
  `rescans=3` then `is no longer valid … Rescanning` at 192,625 ms;
- `Discontinuity()` is called when a single poll changes at least
  `kDiscontinuityFloats = 20` watched offsets; `FloatWatch::Sample` already returned
  this count, it was simply summed and thrown away;
- any `|Δmax|` greater than `kMaxPlausibleDMax = 2000` is rejected and triggers a
  silent re-prime. The largest legitimate effect is a Fortify Intelligence 100 on a
  character at the cap, i.e. 200 points.

A re-prime prints no event and is not counted.

### 5. A defect in my own test harness

When the enumeration went from three to five values, `test_math.cpp` started to
segfault: `CheckClass` translated the verdict to text through an array
`const char* names[] = { "preserved", "not-moved", "partial" }` **indexed by the
enumeration**. The table was not updated, the read went out of bounds and the binary
died before printing a single line (`exit 139`). No compiler warning.

Replaced by a `switch` listing every case with no `default`: `-Wswitch` now turns the
omission into a compile error. It is the same kind of fault as the substring
assertion on the `.conf` fixed in 0.5.1 — a check that looks as if it verifies
something without verifying it.

Suite: 70 assertions, 0 failures, 0 warnings.

---

## Field journal — 0.6.0-probe, 6 August 2026, 17:01 → 17:03 — **THE BUG IS CAPTURED**

Priming at **17,922 ms** instead of 129,562 ms: the new criterion (27 plausible floats
out of 482, versus 5 in the initial snapshot) works. The discontinuity re-prime fired
twice, once on a float burst at 17,969 ms and once on object relocation at
108,078 ms, without producing a false event.

### Event 2 — the destructive half, unambiguous

```
Intelligence modifier total :   110.0000 ->    10.0000  (delta -100.0000)
Magicka modifier total      :   100.0000 ->     0.0000  (delta -100.0000)
predicted change in maximum : -300.0000
deficit at +0x6B8           :  -567.7690 ->  -567.7690  (delta +0.0000)
pool before the change      : NOT full
VERDICT: MAGICKA STOLEN
```

The maximum falls from 620 to 320, the deficit does not move. Therefore:

- before: `current = 620 − 567.769 = 52.23`
- after: `current = 320 − 567.769 = −247.77`

**Current magicka becomes negative.** There was no overflow to spill — the pool was at
52 points out of 620 — and yet the 300 points of the maximum's drop were taken in full
from current. This is the bug, measured, with the numbers.

And this explains the symptom the mod exists to fix: regeneration then has to climb
back from −247.77 before the first point of magicka is usable again. At 12.98
points/s that is **19 seconds of invisible waiting** after the expiry, before the bar
even starts to fill.

### Event 1 — the "free magicka" half, confounded with the spell cost

```
Intelligence modifier total :    10.0000 ->   110.0000  (delta +100.0000)
Magicka modifier total      :     0.0000 ->   100.0000  (delta +100.0000)
predicted change in maximum : +300.0000
deficit at +0x6B8           :     0.0000 ->  -270.0000  (delta -270.0000)
pool before the change      : FULL (deficit zero)
VERDICT: PARTIAL
```

The classifier returns `kPartial` because −270 is neither 0 nor −300. But the −270.0000
is **exactly the displayed cost of the fortify spell**. [DEDUCTION] based on that
cost and on this exact value:

- if a preservation compensation had been applied, the deficit should have been
  `0 − 300 − 270 = −570`;
- to get exactly `−270`, the compensation must be **zero** and the only movement must
  be the payment for the spell;
- the alternative hypothesis — a cost of 570 with a compensation of +300 — is ruled
  out: a character with a maximum of 320 cannot pay 570.

So the actual sequence is: the maximum goes to 620, the deficit stays at 0 so the
pool is **filled for free** to 620, then the 270 for the spell are debited →
`current = 350`. The player had 320 before casting a 270 spell, and has 350
after. **They gained a net 30 points by casting a spell that costs 270.**

This is the structural confound of this test: a fortify spell cast on oneself is paid
for in the same transition as the one that raises the maximum, so the raw movement of
the deficit mixes the two. 0.7.0 now prints on every event the value the deficit
would have under preservation and the gap, which separates the two terms by simple
reading. **The clean test remains to be done**: change the maximum without paying
anything, by equipping or unequipping a Fortify Intelligence or Fortify Magicka item,
or by drinking a potion, with the pool partially empty.

### The unified model: both halves are a single defect

Both events read as a single rule: **the `+0x06B8` field is a raw counter of spent
magicka, which the engine never rebases when the maximum changes, and
`current = max + deficit` is computed with no bound at all.**

A notable consequence: over a complete round trip, the arithmetic is conservative.
Starting from 320/320, gaining 300 for free, spending, then having 300 taken back
leaves a balance consistent with the total spent. The defect is not a leak of points,
it is that **the intermediate state allows an overdraft**: one spends magicka that
exists only because the glass was enlarged, then the glass shrinks and carries the debt
into the negative.

This also confirms the correction rule, and `ComputeCompensation` already implements it
as is. With `d` the deficit, `m` the maximum, `Δ = m' − m`:

```
d' = min(d − Δ, 0)        bounded below by −m'
```

- increase: `d' = d − Δ` preserves current, no more free magicka;
- decrease without overflow: `d' = d − Δ` preserves current, no more theft;
- decrease with overflow: `d − Δ > 0` so clamping at 0 leaves the pool full at the
  new maximum, which is the behavior already observed as correct in 0.5.1.

The floor at `−m'` is in fact redundant: if current was positive before
(`d ≥ −m`), then `d − Δ = d − m' + m ≥ −m'`. It can only bite on an already
corrupted state, that is, a loaded game with an already negative current. It is kept
for that reason.

### Regeneration on the deficit

The deficit is −572.45 at 62,047 ms and −567.769 at 95,782 ms: it **rises** toward zero
between the two, which confirms that regeneration does act on this field and not on a
separate current field. Consistent with the negative result from 0.5.0 that current
magicka is not stored.

---

## 6 August 2026, 18:30–18:50 — 0.9.1, first session with `bDryRun = 0`

Log: 575 lines, `iPreset = 2`, `iLevel = 2`, `iEnginePolicy = 1`, PE `0xF19077A4`.
Three fortify/expiry cycles, six transitions, **six `WRITE OK`, zero
`WRITE FAILED`**, zero error lines in the entire file. `player=found`, `lost=0`,
`rescans=4` stable over 20 minutes and 67,937 polls.

Player's protocol: trial 1, stay above 50% during the fortify and check the return to
100% after expiry. Trial 2, twice, drop to ~25% and check the return to the same
absolute value on the reduced scale.

| # | h | event | max | current before | left by the engine | after write | drift |
|---|---|-----------|-----|---------------|----------------------|----------------|--------|
| 1 | 18:44:16 | cast    | 320→620 | 320.0000 | 350.0000 | 50.0000 | −270.0000 = cost |
| 2 | 18:45:11 | expire  | 620→320 | 323.9062 | 24.1011 | 320.0000 | −3.9062 = cap |
| 3 | 18:45:21 | cast    | 320→620 | 320.0000 | 350.0000 | 50.0000 | −270.0000 = cost |
| 4 | 18:46:16 | expire  | 620→320 | 153.8036 | −145.9885 | 154.0115 | +0.2079 = regen |
| 5 | 18:48:45 | cast    | 320→620 | 25.4453 | 325.4453 | 25.4453 | +0.0000 |
| 6 | 18:49:40 | expire  | 620→320 | 133.6403 | −166.3597 | 133.6403 | −0.0000 |

Percentages seen by the player: 100 → 8.06% during, back to 100% (trial 1);
100 → 8.06%, drop to 24.81%, back to 48.13% (trial 2a); 7.95 → 4.10%, drop to
21.55%, back to 41.76% (trial 2b). The returns to 48.13% and 41.76% are exactly
24.81% and 21.55% multiplied by 620/320 = 1.9375, so the absolute value is preserved.

Event 5: `Δd = 0.0000`, verdict `FREE MAGICKA` instead of `PARTIAL`. The spell cost
had been applied in an earlier poll. The correction produces the same result, which
validates that the rule is invariant to how time is sliced into polls. See SPEC 4.6.

Calibration observed alongside the pinned default: 4 for "ignores", 0 for "handles",
no contradiction reported.

**Defect found in the log itself**: the startup banner announced
"observation build, no memory writes" and "Nothing in your game will behave differently"
even though `bDryRun = 0` and six writes followed. String unchanged since 0.1.0.
Fixed in 0.9.2: the banner is conditional on `bDryRun`.

**Consequence**: `bDryRun` defaults to 0 in 0.9.2. The write path is
validated in game.

Side incident, before this session: on quitting the game, the process remained in
Task Manager with no window and the log could not be deleted. Mechanism
identified in our code and fixed in 0.9.1; cause of the incident not proven. See
SPEC 9.3.

---

## 6 August 2026, 18:30–23:08 — 0.9.1, long session of 4 h 37 min

Same session as the previous one, continued. Log 960 lines, 938,783 polls,
16,623 s. **16 transitions, 14 `WRITE OK`, 0 `WRITE FAILED`, 0 `[ERR ]` lines, no
crash.** The two transitions without a write are `OVERFLOW SPILLED` 8 and 9, where the
correct decision is to do nothing.

A lot of Charm use early in the session, with no effect on the maximum and therefore no
event — consistent.

### Events 7 to 16 (1 to 6 are in the previous entry)

| # | h | Δmax | before | engine alone | after | note |
|---|---|------|-------|-------------|-------|------|
| 7 | 21:01:29 | +300 | 320.0000 | 350.0000 | 52.0000 | cost 270.0000 |
| 8 | 21:02:55 | −300 | 320.0000 | 320.0000 | — | full, spill |
| 9 | 21:26:09 | −20 | 300.0000 | 300.0000 | — | Int +10 ring removed, full |
| 10 | 21:28:18 | +20 | 23.5872 | 43.5872 | 23.5872 | ring put back on, pool empty |
| 11 | 22:11:39 | +300 | 320.0000 | 352.0000 | 52.0000 | cost 268.0000 |
| 12 | 22:12:59 | −300 | 543.3721 | 243.6843 | 320.0000 | capped, +76.32 vs vanilla |
| 13 | 22:13:52 | +300 | 285.7708 | 317.7708 | 17.7709 | cost 268, float rounding 1e-4 |
| 14 | 22:15:56 | −300 | 173.9605 | −126.0192 | 173.9808 | +0.0203 regen |
| 15 | 22:19:11 | +300 | 318.6917 | 350.6917 | 50.6917 | cost 268.0000 |
| 16 | 22:20:25 | −300 | 96.6643 | −202.9829 | 97.0171 | +0.3528 regen |

Session summary: 4 negative pools avoided (down to −202.98, about 58 s of cumulative
invisible waiting removed), 2,120 points of free magicka refused.

### Three findings

**Enchanted items, first measured case** (9 and 10). `Δmax = ±20 = 2 × ±10`, consistent
with the model. Removed with the pool full → spill, no write. Put back on with the pool
at 23.59 → `Δd = 0`, free magicka, corrected, current preserved exactly. **Same
mechanism as a spell**: the source of the change does not matter.

**The "non-informative" guard worked under real conditions.** Events 8
and 9 have `d_before = 0` and `d_after = 0`, hence `c = d_before = 0`: a correct engine
and a careless engine both leave the deficit alone. No `OBSERVED` line was
emitted. This is precisely the trap I had identified as "the subtle bug of the whole
scheme", and it did not trigger.

**The spell cost goes from 270.0000 to 268.0000** between 21:01 and 22:11, exactly,
and three times in a row afterwards. Identical magnitudes (`2 × 100 + 100`). The formula
`cost = base × (1.4 − 0.012 × skill)` requires a change in effective skill
of about 0.14 points for this ratio, which no gain of a whole point produces. The
observed durations also vary (55, 55, 55, 86, 80, 124, 74 s), incompatible with a single
spell of fixed duration. **No sourced explanation.** No effect on the correction, which
never reads the cost.

### Design defect found, fixed in 0.9.3

At 21:01:00 and 21:01:10, two float bursts (probably a save load) triggered a
re-priming. The code then **also** reset the calibration evidence: 4 confirmations for
"the engine ignores" were thrown away, and event 10 starts again from 1.

This is wrong. The model's baseline describes a snapshot of one object and must be
discarded. The policy evidence describes whether **this executable** rebases the
deficit, a property of the binary that cannot change because a save was loaded or the
player object was reallocated. The only effect of the reset was to delay the one
warning that matters. Both calls to `g_policy.Reset()` are removed.

### Documented blind spot (SPEC 10.3)

A **permanent** increase in Intelligence changes the base value, not a modifier:
predicted `Δmax` is 0 and no event is emitted. Not corrected, not measured. Not
applicable here (Intelligence already at 100) and probably harmless since the game
restores magicka to full on level-up, but "probably" is the honest word.

### Stability

56.5 polls/s for 62.5 requested (`Sleep` granularity). One instance loss at
23:07:40 on a save load, recovered by scan. `lost = 0`, `rescans = 4`.
960 lines against a cap of 200,000.

## 0.9.4 → 0.9.6 — the 270 / 268 spell cost explained (documentation only)

Exact dates were not recorded. No code changed in these versions except the `.conf`
comments; the plugin binary behaves as 0.9.3. The reference write-up is `SPEC.md` §4.7.3.

**This entry supersedes the end of the previous one.** "No sourced explanation" and the
estimate of "about 0.14 points" of skill were both computed with the **vanilla** cost
formula, which does not run on this installation.

### Causes ruled out

- **Vampirism.** The stage bonus applies to Strength, Speed and Willpower, and to
  Acrobatics, Athletics, Destruction, Hand to Hand, Illusion, Mysticism and Sneak. It does
  not touch Intelligence, Magicka or Restoration
  ([UESP, Vampirism](https://en.uesp.net/wiki/Oblivion:Vampirism)). See `SPEC.md` §10.3.
- **Armor.** With any armor, spell effectiveness is capped at 95% and scales the
  **magnitude** ([UESP, Spell Effectiveness](https://en.uesp.net/wiki/Oblivion:Spell_Effectiveness)).
  All eight casts show `Δmax = +300.0000` exactly, so no armor was worn. See `SPEC.md` §10.4.

### The installation is modded

Two mods by Baestus are loaded, Nexus
[1846](https://www.nexusmods.com/oblivionremastered/mods/1846) (skills, in the damage
**and magicka cost** formulas) and
[2068](https://www.nexusmods.com/oblivionremastered/mods/2068) (attributes, damage formula
only). Neither lets a **base** skill or attribute exceed 100; both Nexus pages say so in
capitals. See `SPEC.md` §10.7.

### The source of mod 1846

Read in
[`src/SkillScalarUncapper-ASI.cpp`](https://github.com/Baestus/OBSE64_Skill_Damage_Scaling_Cap_Remover).
The mod hooks the cost function with MinHook, and permanently patches the clamp out of
`luck_skill_modifier` (`cmovbe rax,rcx`, `48 0F 46 C1`, becomes `mov rax,rcx; nop`,
`48 89 C8 90`). The cost actually executed is:

- effective skill `S ≤ 100`: `Base × (1.4 − 0.012 × S)`, identical to vanilla;
- `S > 100`: `0.2 × Base × 100 / S`, hyperbolic, never zero, never negative.

Restoration 100 with Luck 59 gives `S = 100 + 0.4 × (59 − 50) = 103.6`, so the
hyperbolic branch applies.

### Resolution

On that branch the cost is proportional to `1/S`. Going from 270 to 268 needs `S` to rise
by 0.746%, i.e. +0.773 points. Luck contributes 0.4 point per point:

| Luck change | Compatible base cost | Window width |
|---|---|---:|
| 59 → 60 | [1396.01, 1396.20) | 0.19 |
| 59 → **61** | **[1396.35, 1401.19)** | **4.84** |
| 59 → 62 and beyond | none | — |

`[DEDUCTION]`: Luck went from 59 to 61 between 21:01 and 22:11, and the spell's base cost is
about 1399. It is one spell, not two. **Not yet confirmed by the player**: the current Luck
value has to be read in game.

### Withdrawn

Three earlier claims are withdrawn in `SPEC.md` §4.7.3:

- "two distinct spells", deduced from the varying durations. A recast during the buff emits
  no event, so all the gaps fit a single 55 s spell recast.
- "effective skill is capped at 100, so only a different base cost remains" (and the base
  costs 1350 / 1340 derived from it). The cap is real in vanilla, but mod 1846 removes it.
- "spells would become free at effective skill 116.67, which is not observed, so the cap is
  active". The cap is not active, and the hyperbolic branch never reaches zero.

The reasoning in `SPEC.md` §10.3 and §10.7 about the uncapper mods was also corrected twice:
these mods lift the cap **in formulas**, not the **progression** cap.

None of this affects the correction, which never reads the spell cost.

## 21 October 2026 — 0.9.7, publication pass

Preparation of the public repository. Changes:

- `SPEC.md`, this journal and `upstream-issue-1846.md` translated from French to English.
  The translation changes no claim. Three wrong cross-references in the French `SPEC.md`
  were corrected on the way: §4.7.1 pointed to "3.1" for the `Δmax` formula, which is in
  §3.4; §10.2 pointed to "§9.5" for save repair, which is §9.6; §5.3.2 described the
  full-pool trap as "an expiry on a full pool (§4.4)", while the measured case is the ring
  removal of §4.3.
- `README.md` rewritten. The old header still described the 0.2.1 observation build ("does
  not change anything in your game"), and its "second run: set `bCallVirtuals = 1`" advice
  pointed at the call that hangs or faults (`SPEC.md` §9.1).
- The `.conf` text no longer says "this build never writes to game memory in any case", a
  leftover from the observation builds. That sentence is inside the binary, hence the
  version bump.
- The header comment of `src/Plugin.cpp` (still "0.1.0-probe, phase 1, observation only")
  and the version in `CMakeLists.txt` (0.1.0) brought up to date.
- New `tools/gen_conf.py`: writes the default `.conf` from `src/Config.h` with CRLF line
  endings and checks its structure (setting syntax, the shipped defaults, ASCII, one banner,
  nothing technical above it, no quick save / quick load key). `build.sh` runs it and also
  fails on any import other than KERNEL32, msvcrt and USER32.
- Build moved to Linux/WSL as the reference environment, with a GitHub Actions workflow.

Found on the way, on the player's machine: the installed `MagickaPoolFix.asi` was still
0.9.1, whose startup banner reads `0.9.1-dryrun - observation build, no memory writes` while
`bDryRun = 0` and the same log shows `WRITE OK`. That banner was the defect fixed in 0.9.2
(see above); the plugin was writing correctly.

## 21 October 2026 — consistency pass on SPEC (documentation only)

The specification was read against the code and the journal. No code changed except one
comment in `src/ModelWatch.h`; the binary and the `.conf` are unchanged.

### Corrected

- **§7.3** said `bDryRun` defaults to 1. It has defaulted to 0 since 0.9.2 (entry of
  6 August 2026, 18:30–18:50), as `src/Config.h` and the README say.
- **§5.3.3** said the calibration evidence is reset on every save reload. That reset was
  removed in 0.9.3 (long-session entry, "Design defect found, fixed in 0.9.3"). The section
  now says the evidence is kept, and why.
- **§6.2** still described the write as future ("in phase 2 will write").
- **§6.4**, guard 3, and the comment on `kMaxPlausibleDMax`, justified the 2000-point limit
  with "the largest legitimate effect is a Fortify Intelligence 100, i.e. 200 points". The
  measured transitions are ±300 (one spell, Fortify Intelligence 100 + Fortify Magicka
  100). The limit was always 2000; only its justification was wrong.
- **§6.4 and §7.2**: `fMaxDeltaGuard` (default 100000) can never trigger at its default,
  because the 2000-point check in `ModelWatch` rejects anything larger first. Now stated.
- **§10.3 and §10.7 contradicted each other.** §10.3 called the level-up blind spot "not
  reachable on this installation" and then listed reachable cases; §10.7 called it
  "inactive". Both now say: unreachable at base Intelligence 100 (the test character),
  still reachable below 100 through a level-up. Restoring damaged Intelligence and permanent
  blessings, which §10.3 listed as reachable without a source, are now listed as unknown.
- **§10.5** spoke of "a permanent drain through level-up", which is not a thing; it means a
  level-up raising a base attribute.
- **§10.7** pointed to `upstream-issue-1846.md` and to "the reference tree", neither of
  which is in the public repository. It now links mod 1846's public source.

### New sourced facts

From [UESP, Remastered Leveling](https://en.uesp.net/wiki/Oblivion:Remastered_Leveling):

- 12 virtues per level-up; at most 5 on one attribute and 3 attributes per level-up.
- Luck costs 4 virtues per point and rises by **at most 1 per level-up**.

Consequences:

- `[DEDUCTION]`: a level-up moves the maximum by at most 10 points without an event, so
  the §10.3 blind spot is small even where it is reachable.
- §4.7.3: Luck 59 → 61 between 21:01 and 22:11 cannot come from a single level-up. It
  needs two level-ups, or a level-up and another source, or a source other than levelling.
  Still to be read in game.

Neither UESP page says whether a level-up refills magicka, or whether Remastered still
requires resting in a bed to level up. Test 5 (the first level-up) settles the first point.

### Found, not fixed (needs a binary change)

- The `.conf` comment on `fMaxDeltaGuard` says the plugin "resynchronises" past the limit.
  It does not: the decision is "no action" (`DecisionReason::kImplausible`), and the
  re-prime belongs to the 2000-point check, not to this setting.
- The startup summary spans about twenty lines, against the rule of a one-line summary.

## 21 October 2026 — 1.0.0-dev: a log that describes itself (built, not yet run in game)

Version scheme from here: `1.0.0-dev` until the 1.0.0 release, with builds told apart by
the build time on the log's first line.

### Why

The plugin is about to be public. From then on, a log from someone else's machine is all
a bug report will carry, so its first lines have to say what the game build is, what is
loaded, what was changed in the `.conf`, and what the character looks like. Until now the
startup was about 500 lines (configuration, game sections, 142 modules, RTTI details, two
hex dumps of the player object), with no level, attributes or skills anywhere.

### What changed in the log

- **Line 1 is a summary**:
  - version, `ACTIVE` or `DRY RUN`, build time;
  - the game executable and its PE timestamp, with "(validated build)" or "(NOT the
    validated build)";
  - preset, engine policy, conf state.

  It is written before the 2 s wait, and the PE header is read through `ReadMem`. If the
  build is not `0xF19077A4` and the plugin would write, an error line says to set
  `bDryRun = 1` first.
- **One line of loaders and mods**: every module outside the Windows folder, relative to
  the game folder when inside it, with its own PE timestamp. That identifies a mod's build
  without `VERSION.dll`, which the import rule forbids and which is the very file an ASI
  loader usually stands in for.
- **One line of non-default settings.**
- **The character block**, once a save is loaded:
  - level, ready-to-level-up flag, race, class and birthsign form IDs;
  - base health, spell points and fatigue;
  - the 8 attributes and 21 skills, base value and modifier total, with the mastery letter
    of each skill.

  Then **`LEVEL UP`**, **`BASE VALUES CHANGED`**, **`SKILL UP`** and **`READY TO LEVEL UP`**
  lines during play. Each carries the magicka deficit before and after, so the first
  level-up answers SPEC §10.3. Offsets and sources: `docs/SPEC.md` §10.8.
- Everything else from startup moved to `iLevel = 3`.
- Paths: the user's profile folder is written as `%USERPROFILE%`.
- Stale log string removed: the offsets report still said "not read from your binary.
  Confirming them is this build's job", from the probe builds.

### Found on the way

- commonlibob64's `ActorValue.h` puts Magicka at 8. The plugin's mapping (Magicka 9) was
  set from the field dump of 6 August and agrees with the CS wiki "Stats List", which is
  now the cited source for the indices in `src/PCH.h`.
- The `.conf` described preset 1 (Vanilla+) as "Zero balance impact". SPEC §7.1 requires
  the opposite: keeping the free magicka and forgiving the debt at expiry is a net gain
  (§4.4). Corrected.
- The `.conf` banner still said "turn the dry run off", and the `fMaxDeltaGuard` comment
  said "resynchronises". Both corrected (see the previous entry for the second).

### Status

Built with MinGW-w64: unit tests 176 OK (41 new, `tests/test_stats.cpp`), 0 FAIL,
0 warnings, imports KERNEL32, msvcrt, USER32. **Not run in game.** Every character value
is `[DEDUCTION]` until a log is compared with the character sheet.

### Pending

The installed `MagickaPoolFix.log` is a 0.9.1 session from 28 August 2026, 21:02–22:39,
not analysed yet. It has 17 events and 10 `WRITE OK`, 0 `WRITE FAILED`, 0 error lines.
Seen at a glance:
- event 1 is a +300 cast from a full pool that leaves the deficit at exactly −266.0000, so a
  cost of 266 against 268 on 6 August;
- the Intelligence modifier baseline is 12 instead of 10;
- event 15 has a `MAGICKA STOLEN` verdict.

The other casts were not from a full pool, so their costs still have to be worked out.

## 21 October 2026 — the 28 August session, analysed

The 0.9.1 log of 28 August 2026 (21:02–22:39) stayed on the test machine and is analysed
here, with no notes from the player about that session. Full table and findings:
`docs/SPEC.md` §4.8. This entry supersedes the "Pending" paragraph of the previous one.

**Correction of my own reading in the previous entry and in conversation**: I first listed
"spell costs of 266, 278, 284, 261 and 285" from a quick extraction that cut the
`DECISION` lines short. Only 266 and 278 are costs, measured on full-pool casts. 284.06 and
285.28 are deficits after casts from a partial pool (both are cost 278 again), and 261.61
is a deficit before a 2-point change.

### Findings

- **No defect in the correction.** 17 transitions, 10 writes, all read back, 0 errors.
  The 7 unwritten ones are full-pool decreases.
- **The cost went from 266 to 278 across an item swap at 21:29** (Intelligence modifier
  12 → 2 → 12 in 0.7 s). A brute-force fit of the four costs measured so far (270, 268,
  266, 278) under mod 1846's formula finds one pattern only:
  - Restoration +5 on 6 August and before the swap, gone after it;
  - Luck 59 → 61 or 62 → 63 or 64;
  - base cost about 1460–1470.

  It requires the skill passed to the cost formula to include Fortify Restoration. The
  `+5.0` on indices 21–25 in the 6 August dump (SPEC §3.4) fits.
- **SPEC §4.7.3 corrected**: its effective skill of 103.6 ignored that `+5`, so its base
  cost of ~1399 is withdrawn. The +2 Luck between the two groups of 6 August stands.
- **Armor in the field**: event 11 is the spell at 95 % magnitude (+285), with the cost
  unchanged. SPEC §10.4 predicted both.
- **A Drain Magicka 22, most likely**, for 49 s during combat; regeneration ran at about
  half rate just before it. The mod cancelled its effect on current magicka, which raises
  a design question now open in SPEC §10.5.
- **A lasting Intelligence −1** from 22:08, probably a Damage Intelligence. Its `PARTIAL`
  verdict comes from in-poll regeneration exceeding the tolerance of a 2-point change; the
  decision was right regardless.
- **The Intelligence baseline is 12, not 10**: something adds +2 besides the ring.

### Small defects found in the log text (not fixed yet)

- The `PARTIAL` verdict text says "On an INCREASE this is the ordinary shape when you cast
  the fortification on yourself", and it is printed for decreases too (event 13).
- On a small change, regeneration within one poll exceeds the tolerance `0.01 + 0.02 ×
  |Δ|`, so the transition is classed `PARTIAL` and gives no calibration evidence. It has
  no effect with the default `iEnginePolicy = 1`.

### Still open after this session

Every expiry and the item swap happened with the pool full, so SPEC chapter 8 scenarios 5
(equip/unequip at a partial pool) and 6 (ten loops with a drift check) are still open.

## 21 October 2026 — UESP pages checked against Remastered

Prompted by the player's question: is `Oblivion:Oblivion`, the page the README credited,
about the original game rather than Remastered? It is. UESP documents Remastered inside
the same `Oblivion:` namespace, through a hub page (`Oblivion:Oblivion_Remastered`),
"Remastered" pages, "Oblivion Remastered" sections and inline notes. Every UESP page cited
in the repository was re-read in raw wikitext and classified; the result is in
`docs/SPEC.md` chapter 12.

### A misquotation, withdrawn

`SPEC.md` §2 and §12 said UESP's Spell Making page describes Fortify Magicka as *not*
adding current magicka, unlike Fortify Intelligence. The page ("Spell Chaining", original
game) says the opposite:

- Fortify Magicka adds its magnitude to both current and maximum;
- Fortify Intelligence adds twice its magnitude to both;
- Drain Magicka removes from both;
- on expiry, the two Magicka effects cannot take current magicka below zero, while
  Fortify Intelligence can.

The page presents the free magicka as an exploit, spell chaining. §2 now states this and
says the mod departs from it on purpose.

### UESP contradicts itself on Remastered regeneration

At effective Willpower 110, out of combat:

| Page | Formula | Rate |
|---|---|---:|
| Willpower | `2 × (W/50)² + 1.5 × (W/50)` | 12.98 /s |
| Magicka | `2 × (0.0003 W² + 0.015 W)` | 10.56 /s |
| Remastered Changes | `0.02 W² + 0.015 W`, before the out-of-combat factor | 243.65 /s |

Only the first matches the measurement of SPEC §3.3. The Magicka page's formula fits no
vampirism stage either. Recorded in §3.3.

### Two consequences for the design

- **Drains**: UESP documents a Drain Magicka as taking current magicka. The fix changes
  that on a pool that is not full. SPEC §10.5 now quotes the page; the decision belongs
  to the player.
- **A floor at zero**: if Remastered floored current magicka at zero on a Fortify Magicka
  expiry or a drain, the engine would move the deficit itself, and the relative rule would
  over-correct. The measurement of §4.1 rules this out for a combined expiry: 52.23 →
  −247.77, which no order of applying a floor can produce. A Fortify Magicka alone
  expiring at a low pool, and a drain on a nearly empty pool, remain to be measured. Added
  to SPEC §10.5.

### Also from this pass

- **Spell Making, Remastered note**: one spell may now combine several Fortify Attribute
  and Fortify Skill effects.
- **Mastery following the base skill** comes from an original-game page with no Remastered
  note: `[DEDUCTION]` for Remastered.
- Three redirect URLs (`Oblivion:Drain`, `Oblivion:Spell_Effectiveness`, `Oblivion:Player`)
  were replaced by their targets.

## 21 October 2026 — one framing, and the source of Willpower 110

### "Fix" or "balance change": one framing

The README called the plugin "a balance change, not only a bug fix" in one place and "the
fix" everywhere else, and the `.conf` said the same. The maintainer's position, now stated
once in `SPEC.md` §2 and used everywhere: the defect is a bug and the plugin is a fix, and
a fix that closes an exploit changes the balance for the players who used it. Whether the
designers meant the side effect is not documented; UESP describes it as an exploit. The
maintainer holds the same view of the two uncapper mods, written in the README as an
opinion.

### `.conf` advice corrected

The `.conf` said the spell-chaining exploits that need a negative pool keep working under
`iPreset = 1`. Preset 1 is the floor at zero, so they do not. The advice for someone who
relies on them is now `iPreset = 0`. `[DEDUCTION]` from the preset's definition. The
`.conf` also called the 1500-point variant permanent; UESP says the magicka "will never
drop" while the loop runs.

### Willpower 110 was not vampirism

`SPEC.md` §3.3 explained the effective Willpower 110 of the regeneration measurement by
the 50 % vampirism stage. The 0.4.0-probe entry of this journal records `+10.0000` on all
eight attributes in the magic modifier array during that same run. Vampirism fortifies
only Strength, Speed and Willpower (UESP). `[DEDUCTION]`: an item or effect giving +10 to
every attribute, and no vampirism bonus active at that moment, since any stage would have
given at least 14.03 /s. §3.3 corrected; the regeneration measurement and its formula are
unaffected.

## 21 October 2026 — `.conf` text review

Every statement of the generated `.conf` was checked against the code, SPEC and this
journal.

### Corrected

- **The virtual-call warning (`bCallVirtuals`)** told its story twice, in two orders, and
  the second telling was wrong. The second said the guarded call faulted and "a later run
  on the same build merely hung"; in fact the hang came first. The order was: 0.3.1
  (16:04), the call never returned and the game carried on; then 0.4.0 (16:15), the
  guarded call faulted and UE4SS showed a "Fatal Error". The block also contained a
  stray "The intended sequence is:". Rewritten once, in order.
- **`[Offsets]`** said to touch them only if "the invariant self-test failed".
  **`iForceHypothesis`** said the plugin calibrates "in a fraction of a second". Both
  only happen with `bCallVirtuals = 1`, which this build forbids. Both now say so.
- **The header** listed two symptoms where SPEC has three. It also called vanilla
  "inconsistent" with "the same observable result" for Fortify Magicka and Fortify
  Intelligence: that is an original-game statement from UESP, and Remastered was only
  measured on a spell carrying both. It now says what UESP documents and what was
  measured.
- **Regeneration** points to the "Oblivion Remastered" section of UESP's Willpower page,
  and says two other UESP pages disagree (SPEC §3.3). "11.2/sec" became 11.25.
- **"Cast Fortify Willpower FIRST"** became "fortify Willpower". It now mentions the
  Remastered rule that one spell can carry several Fortify Attribute effects.
- **`bModuleInventory` and `bEnabled`** describe what 1.0.0-dev logs at which level.
- **"READ THIS BEFORE COMPLAINING"** became "READ THIS FIRST".

### Credited

`src/Config.h` already said its conventions (Hungarian prefixes, regeneration of the
defaults) come from Delete Spells - OBSE - ASI (Nexus 4934). The README's Credits now say
so too.

### Noted, not changed

`LoadConfig` writes the defaults whenever it cannot open the file, so an existing but
unreadable `.conf` (locked, for instance) would be overwritten. The text says "missing or
unreadable", which matches the code; whether to keep that behaviour is open.

## 21 October 2026 — the `.conf` is never overwritten

Until now `LoadConfig` wrote the defaults whenever it could not open the file, so an
existing `.conf` that was locked by another program or unreadable because of its
permissions would have been replaced, losing the player's settings. Decision of the
maintainer: write only when the file is truly missing, and log what was done or could
not be done.

- **Missing** means `GetFileAttributesA` fails with `ERROR_FILE_NOT_FOUND` or
  `ERROR_PATH_NOT_FOUND`. Only then are the defaults written, through `CreateFileA` with
  `CREATE_NEW`, which refuses an existing file by construction. A partial write deletes
  its own half-written file. LF becomes CRLF as it writes, as text mode did, so the file
  stays byte-identical to the output of `tools/gen_conf.py`.
- **Anything else is left untouched**: a file that exists but cannot be read, or one whose
  existence cannot be established. The run uses the compiled defaults.
- **Reported in the log**: the summary line says `conf found`, `was missing, created with
  defaults`, `missing and NOT creatable` or `present but NOT readable, left untouched`.
  Each failure adds an error line with the reason in plain words and the Windows error
  code (access denied, in use by another program, locked, folder not found, ...).
- The old log fallback message claimed the `.conf` could not be created whenever the log
  could not be; it now speaks only of the log.

Built (176 OK, 0 warnings, imports unchanged); not run on Windows. No Windows runtime is
available here, so none of the failure paths has been exercised.

## 21 October 2026 — the log, ready for investigating drains

Question from the maintainer: does the log carry enough to investigate drains (SPEC
§10.5)? Partly. It shows when a negative modifier starts and ends, how big it is, what
the engine did to the deficit, and what the plugin wrote. It does not show:

- whether the pool would have gone below zero (the absolute maximum is not read);
- which source moved;
- which effect caused it.

An engine floor at zero would have gone unnoticed, and under one the correction
overshoots. Two log-only additions, agreed with the maintainer:

- **Per-source breakdown** in every transition: the change of the Intelligence and
  Magicka modifiers in the magic, script and game arrays.
- **`ENGINE MOVED THE DEFICIT`**, an error line on any decrease where the engine raised
  the deficit by more than 2 points plus the §5.4 tolerance. Regeneration cannot explain
  that much: under 2 points per poll even at Willpower 200. The line also raises the
  detail level for 200 lines. The test is a pure function in `Corrector.h`
  (`EngineRaisedDeficitOnDecrease`), tested against every measured decrease, all silent
  (6 August 17:03, 28 August events 13 and 15, full-pool spills), and a synthetic floor,
  reported.

Still not possible from the log: whether the pool would have gone negative, which needs
an estimate of the maximum once the character data is validated, and which effect it
was, which needs the active effects. Both are recorded in SPEC §10.5.

The README's AI line is now the maintainer's choice among three variants: "AI-written,
human-directed. Code and docs by Claude (Anthropic); scope, decisions and every in-game
test by me."

Built: 183 OK (7 new), 0 FAIL, 0 warnings, imports unchanged. Not run in game.

## 21 October 2026 — first sessions with 1.0.0-dev; the character data checked

Two short sessions (SPEC §4.9), the first with OblivionRemasteredPerformanceFix 1.1.0 as
`version.dll` and the ASI loader renamed to `winmm.dll`. Startup clean both times. Session
1: ring off and on at a full pool, then the +300 spell; three transitions, every decision
the rule's, two writes, no failure. Session 2: the quick save loaded with the spell still
running, no transition, and the maintainer took 15 screenshots.

**Character data validated** (SPEC §10.8): level, the 8 attributes and the 21 skills all
equal base + modifier on the sheet; maximum magicka 622 = 2 × 211 + 100 + 100.

Settled from the screenshots:

- the +10 on every attribute (6 August) is the ring of perfection;
- the Intelligence +2 of 28 August is the Scales of Pitiless Justice (UESP: Str, Agi, Int
  +2, Per −2, by script while carried);
- the spell is Fortify Intelligence, Willpower and Magicka 100, cost 278 with Restoration
  +0 and base Luck 63, which the fit of SPEC §4.7.3 predicted.

`[DEDUCTION]`s, written as such in the SPEC:

- ability bonuses (race, birthsign, vampirism stage, Gray Prince) sit in the base values,
  not in the modifier arrays. That explains base spell points 100 and the base values of
  110; it adds a blind spot to §10.3 (an ability changing Intelligence or Magicka);
- the "Boucle pentamagique" is the Restoration +5 item (its name, and the +5 on five
  school actor values in the 6 August dump), and the 28 August swap at 21:29 was a ring
  swap with both slots full.

Open: whether the spell cost uses Luck with or without its Fortify (a cast with the ring
off settles it), and a −1 on Intelligence, Agility, Willpower and Speed with no visible
source.

**Corrected in conversation, not in the docs:** I first told the maintainer that the
ability bonuses were invisible to the plugin. They are in the base values it reads.

The session felt laggy; the maintainer puts it down to a shader rebuild and DLSS variable
rate. The plugin's polling is the same as 0.9.1's (about 3,400 samples a minute in both
logs). To have numbers rather than impressions, this build reports its own time.

Log-only changes in this build (still 1.0.0-dev):

- PLAYER lines: each modifier's magic / script / game split when not all magic,
  `+22 (m+20 s+2)` (`FormatModifierSources`, tested);
- `PLAYER estimated magicka current / max`, and the same estimate on every transition
  and after every write (`EstimatedMaxMagicka`, tested against the sheet's 622), for the
  drain question of SPEC §10.5;
- heartbeat: the plugin's own time per second, and the memory map refresh, average and
  maximum;
- the mods line skips modules with no path (two empty entries in session 1);
- "player instance no longer valid" also names a quit to the menu and the game closing.

Built: 190 OK (7 new), 0 FAIL, 0 warnings, imports unchanged, `.conf` byte-identical. Not
run in game.

## 21 October 2026 — session 3: the script array, the ring's Luck, the plugin's own cost

Third short session (SPEC §4.9), with the build that prints the source split and the
estimated pool. Seven transitions, four writes, no failure.

- **Script array, first time.** Dropping and picking up the Scales of Pitiless Justice
  moved only the script array (`s-2`, then `s+2` on Intelligence). The engine left the
  deficit alone; the fix applied the same rule. The drop happened with the pool not
  full: first clean decrease at a partial pool, current magicka kept at 302.
- **The four −1** (Int, Agi, Wil, Spd) are in the game array. `[DEDUCTION]`: damage.
- **The ring's Luck counts in the spell cost**: 288 with the ring off, 278 with it on.
  Re-fitted all six costs measured since August: base cost about 1517, not 1460–1470.
  SPEC §4.7.3 corrected, and my own paragraph from earlier this evening withdrawn: I had
  called this outcome unlikely, with reasoning that mixed up which Luck the profile showed.
- **The plugin's own time: 62 to 72 ms per second**, almost all of it the memory map
  refresh (106 to 178 ms, every 2 s). Wasted: the snapshot only serves the player search,
  which refreshes it itself. Removed; the static slot found at startup is now re-read
  instead of searching again, and a full search happens at most every 30 s once that slot
  is known. Next log should show the own time near zero.
- **Crash at quit**, in `UE4SS.dll` (access violation reading), while the Lua mod
  MagicSkillLevelingFixed was polling for the departed player. Our thread was parked in
  `ntdll` at that moment. `[DEDUCTION]`: not this plugin. Recorded in SPEC §4.9.

Not done again: the spell expiring with the pool not full (it expired at a full pool,
before the tests), and a level-up.

Built: 190 OK, 0 FAIL, 0 warnings, imports unchanged, `.conf` byte-identical. Not run in
game.

## 21 October 2026 — session 4: expiries below zero, a level-up, and recasts in two steps

1 h 37 min with the session 3 build (SPEC §4.9): 42 transitions, 31 writes, no failure,
no error, no crash. The spell was kept up throughout, with and without armor; one
level-up; two vampirism stage changes.

- **Scenario 4 of chapter 8, measured three times**: the buff expired with the pool at
  234, 219 and 104; vanilla would have left −65.7, −80.6 and −180.4. The engine did not
  move the deficit, so no floor at zero on a fortify expiry (SPEC §10.5).
- **The level-up** came after a night's rest, pool full, Intelligence unchanged: harmless,
  and not the test §10.3 wants.
- **Vampirism** moved exactly the stage bonus in the base values, in two steps: the
  deduction of the previous entry holds. The "another save loaded?" line it produced now
  also names an ability.
- **Security +40** in the base values: probably the Skeleton Key carried. Not confirmed.

**A design gap, found in this log and filled with a judgement.** A recast of an active
spell sometimes reaches the arrays as a fall then a rise: 17 ms, 115 ms and 680 ms apart
here. The fall is treated as an expiry and spills the pool above the unbuffed maximum;
on a full pool that would make a 278 recast cost 578. Losses here were 0.7 and 2 points.
Now the spill is remembered and given back by a rise within a window, never more than
the spill (`SpilledOnDecrease`, `RecastGiveBack` in `Corrector.h`; SPEC §5.5, chapter 8
scenario 7).

The window was first set to 500 ms, from the 17 and 115 ms cases; the maintainer asked
why not 100 ms (115 ms rules it out), then agreed 500 ms. Writing the docs I listed every
fall followed by a rise and found the 680 ms case I had missed, so I reported it before
going further. The maintainer agreed 1000 ms, on condition that it is documented as
guesswork and monitored. It is: every rise within 5 s of a spill writes a
`RECAST WINDOW` line (gap, inside or outside, given back or lost, where the cost was
charged), and the heartbeat carries the totals.

Side effect, accepted: an item taken off and put back on a full pool within a second is
now neutral; the 28 August swap (0.7 s) lost 20 then and would not now.

Built: 223 OK (33 new: give-back, field replays of 21:34, 21:36, 21:46 and 22:45, the
28 August swap, ten recasts at expiry), 0 FAIL, 0 warnings, imports unchanged, `.conf`
byte-identical. Not run in game.

## 21 October 2026 — a log for players, Debug for us

The maintainer found the default level too heavy for a player: session 4 wrote 758
lines, about 600 of them from 42 transitions at roughly 15 lines each. Agreed:

- **Default level**: the startup lines, a shorter character block (level, attributes,
  estimated magicka), **one line per change of maximum** (what changed and from which
  arrays, the pool before, as the game left it and after, the outcome, a recast
  give-back), level-ups and base value changes, the player lost or found, and anything
  that needs attention. The heartbeat says whether the player is found, the deficit, and
  how many changes were seen and corrected.
- **Debug**: the full transition block, calibration evidence, decisions, writes, skill
  lines and skill-ups, the recast window inside the window, the plugin's own time.
- A `RECAST WINDOW` rise outside the window stays at the default level, as an Info line
  rather than an error: it is a request to report a log, not a fault.
- **Test sessions run at `iLevel = 3`** (AGENTS.md, SPEC §6.6). The shipped default stays 2.

Estimated for session 4: about 180 lines instead of 758, most of them the status line.

Also: the PARTIAL verdict text now has a version for decreases (it described only the
self-cast increase), closing that open item, and its `switch` lists every case with no
`default`. The `.conf` text of `iLevel` describes the new split, so the `.conf` changed:
replace it with the `.asi`.

Built: 227 OK, 0 FAIL, 0 warnings, imports unchanged. Not run in game.

## 22 October 2026 — session 5: the log cap, and two predictions confirmed

First session at `iLevel = 3`. The log hit its 200,000-line cap at 00:06:26, 36 minutes
in: the float watch writes every float that moves, every poll, at Debug. I had told the
maintainer that nothing at Debug repeats every frame; that was wrong, I had checked only
the watch's manual (F10) mode. Moved to Trace; the `.conf` text said "Debug" and says
"Trace" now.

Before the cap (SPEC §4.9, session 5): own time 1.3 to 3.8 ms per second (was 53 to 84),
spell cost 277 three times as predicted from base Luck 64, one more expiry at a partial
pool kept from −104, and the first game-array change (Intelligence −1, maximum −2),
corrected. Scenario 7 not reached before the cap.

The one-line summaries read well at Info.

The maintainer asked for an in-game notice when something reportable happens, since the
Damage Intelligence of 23:49 went unnoticed. Text on the screen would mean calling the
game's UI code on its thread, so a hook: ruled out. Agreed instead, for testers only: a
Windows sound, `bBeepOnReport` (advanced, off by default), on a recast outside the
window, an engine move, a failed write, a policy contradiction, a game-array change or a
base Intelligence change, at most one every 5 s. A marker key was proposed and dropped:
the maintainer noticed nothing odd in play, so it would not have helped here.

Built: 227 OK, 0 FAIL, 0 warnings, imports unchanged (MessageBeep is USER32). Not run in
game.

## 22 October 2026 — drains: decided for 1.0

The maintainer called the drain question (SPEC §10.5) the main gap before the release, and
noted that vanilla drains are not broken. Agreed: on a pool large enough, the game takes a
Drain Magicka's magnitude and gives it back, as documented; the fix changes that only
because it sees the maximum, and a drain starting looks like a fortify ending.

I first argued that leaving drains to the game would need state that a save and load
loses. That was wrong: doing nothing on a drain needs no state. Corrected in the
discussion before anything was written.

Decision: 1.0 keeps the uniform rule, written as a limitation in the README (a drain is
softened; on a full pool it costs its magnitude and gives nothing back). After 1.0, leave
drains to the engine by reading the player's active effects on each change of maximum.
That is only safe if the engine does not floor a drain at zero, which the next session
measures with a self-cast Drain Magicka 100 for 20 s at a full pool, a half pool and less
than 100 left. UESP's table says alchemy cannot make Drain Magicka (original game), so a
custom spell is the source.

Docs only; no build.

## 22 October 2026 — sessions 6 and 7: two DLSS crashes, and an empty game array

Two short sessions from the quick save that ended session 5 (SPEC §4.9). The plugin
behaved: scenario 1 and 3 at a full pool, cost 277 for the fourth time, no warning.

Both sessions ended on a crash. The dumps carry the game's own fatal error, "Streamline/
DLSSG present failed ... DXGI_ERROR_INVALID_CALL" (code 0x4000, message read from the
exception parameter). Our thread was asleep. Suggested to the maintainer: turn off Frame
Generation for the test sessions; a guess from the message, not tested.

The game array, which held five −1 on four attributes, was empty. The maintainer
completed Darkness Eternal while already a vampire during the part of session 5 the log
missed; that is the only candidate, recorded as a deduction. No manual save before it, so
it will not be confirmed.

Docs only; no build.

Session 8 (22:46 to 23:07): nothing to correct, a third DLSS Frame Generation crash, and two
new game-array −1 (Strength, Endurance) already in the save. The maintainer suspects enemy
enchanted weapons; UESP points at NPC poisons as another candidate. Not chased: neither
touches magicka.

## 25 October 2026 — session 9: a drain, start to end

A necromancer's Drain Magicka 22, on a full pool (SPEC §4.9, session 9). Start: nothing to
do, as vanilla. End: the game gave 22 back, the fix took them away again. The engine moved
nothing. The drain case decided in SPEC §10.5 now has one measurement at a full pool; the
start of a drain on a nearly empty pool is still unmeasured.

The maintainer heard the exclamation sound twice when switching windows, with no
"(sound played" line in the log, so it was not the plugin. The plugin uses the same
Windows sound as other programs; after 1.0, consider a sound of its own.

## 25 October 2026 — session 10: scenario 7, both shapes

The recast while the buff is active, open since the window went in (SPEC §5.5, chapter 8
scenario 7), happened twice in normal play (SPEC §4.9, session 10). In one step, at 95 %
magnitude after putting armor on: 361.2 → 84.2, the cost exactly. In two steps, 156 ms
apart: 327 → 50, the cost exactly, the window giving back the 3 points the fall spilled.
The 1000 ms judgement covered it with room to spare; the gap goes in the §5.5 table.

README: the recast window is no longer listed as untested.

## 25 October 2026 — session 11: the warning to remove the plugin, wrongly

The maintainer heard the sound at 23:42. The log carried the most alarming block the
plugin has: "ENGINE BEHAVIOUR DISAGREES WITH THE CONFIGURATION", ending on "remove the
plugin". It was wrong. The buff cost 282 and added 285 to the maximum; the deficit moved
by −282, within 2 % of −285, and the calibrator read that as the engine compensating.

The rule had a hole I had not seen: on an increase, landing on the corrected deficit is
exactly what a cast costing the increase does. With a pinned policy every player whose
buff costs about what it adds would get the warning on every cast. SPEC §5.3.6 already
described the refutation (a compensating engine would imply a cost of −3), but the
calibrator never used it. Fixed by counting "handles" only on decreases, where a cost
cannot fake it (SPEC §5.3.2, with a withdrawn note). What it costs: a patched game is no
longer caught on an item equipped, only on the next decrease on a partial pool.

Built: 233 OK (6 new, replaying 23:42:49; the replay fails without the fix), 0 FAIL,
0 warnings, imports unchanged, `.conf` text unchanged. Not run in game.

## 26 October 2026 — a restore of the game array

The sound played at 21:34:06 when the maintainer used an ichor pool at a Sithis shrine:
the game array's Intelligence −2 went back to 0, maximum +4 on a full pool, kept at 320 /
324 by the fix (SPEC §10.5). It was the optional "Restore Intelligence" test left open
since session 5, and the first time the sound flagged an event the maintainer could tie
to an action in game.


## 27 October 2026 — the other shape of a recast in two steps

Session of 26 October, 23:17 to 02:05 (SPEC §4.9). At 23:52:47 a recast came as a fall
then a rise 16 ms apart, with the cost charged on the fall, not the rise as in session
10. The fall spilled 9 points above the unbuffed maximum; the window gave them back on the
rise, and the pool ended at 609 − 276, the spell cost only. Both shapes of §5.5 are now
measured with the window in place. The other eighteen changes of maximum were as expected.

Docs only; no build.
