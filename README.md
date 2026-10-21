# MagickaPoolFix

An ASI plugin for **The Elder Scrolls IV: Oblivion Remastered** that fixes how current
magicka behaves when maximum magicka changes.

This README is the developer's entry point: what the plugin does, how it is built, tested
and released. A player-facing description will live on the mod page. Contributors, human
or AI, should also read [`AGENTS.md`](AGENTS.md).

**Built with an AI.** The code and the docs are written by Claude (Anthropic); the scope,
the decisions, every iteration and every in-game test are mine. See
[How this project is made](#how-this-project-is-made).

> **This plugin writes to game memory by default.** It writes one float, the player's
> magicka deficit, when a change of maximum magicka needs correcting, and reads it back
> to confirm the write took. No function is hooked and no game code is patched. Set
> `bDryRun = 1` in the `.conf` to watch what it would do without letting it act.

Validated in game on the Steam build whose executable has PE timestamp `0xF19077A4`.
A game update may break it; see [Limitations](#limitations).

---

## Why this exists

This plugin was born from an end-game mage build: custom spells that push Intelligence,
Willpower and the magic skills well past 100, recast every time they expire. Two mods by
Baestus are what make such a build worth playing, because the base game clamps values
above 100 in the formulas that matter:

- [**Skill Damage Scaling Cap Remover**](https://github.com/Baestus/OBSE64_Skill_Damage_Scaling_Cap_Remover)
  (Nexus [1846](https://www.nexusmods.com/oblivionremastered/mods/1846)). The game caps
  skills at 100 in `luck_skill_modifier`; the mod patches that cap out and hooks the
  spell cost formula. A fortified skill above 100 keeps lowering spell costs, as
  `0.2 x base x 100 / skill`, instead of stopping at the vanilla floor of 20 % of the base
  cost, and keeps raising damage.
- [**Attribute Scaling Cap Remover**](https://github.com/Baestus/OBSE64_Attribute_Scaling_Cap_Remover)
  (Nexus [2068](https://www.nexusmods.com/oblivionremastered/mods/2068)). Attributes above
  100 count in the damage formula instead of being clamped to 100.

Their source code was also this plugin's starting point; see [Credits](#credits).

Neither mod lets a *base* skill or attribute level past 100. Neither touches maximum
magicka or regeneration either: 1846's source hooks only the cost formula, and 2068's
README describes the damage formula only. Those two did not need uncapping. Intelligence
above 100 already adds 2 magicka per point in the base game
([UESP, Spell Making](https://en.uesp.net/wiki/Oblivion:Spell_Making)), and regeneration,
`2 x (W/50)^2 + 1.5 x (W/50)` per second, was measured following an effective Willpower of
110, not a capped 100 ([`docs/SPEC.md`](docs/SPEC.md) section 3.3). `[DEDUCTION]`: since
neither mod touches regeneration, that is the base game's behaviour. Details and sources:
[`docs/SPEC.md`](docs/SPEC.md) sections 4.7.3 and 10.7.

The result is a magicka pool that swings by hundreds of points every time a buff is cast or
expires. On the test character, one Fortify Intelligence 100 + Fortify Magicka 100 spell
takes maximum magicka from 320 to 620 for 55 seconds, then back, and is recast over and
over. That is where the engine's bookkeeping
defect, harmless on a +10 ring, becomes the main thing happening to the magicka bar:

- the buff refills the pool for free when it starts, so the spell more than pays for itself
  (measured: 320 before casting a spell that costs 270, 350 after);
- when it ends at a low pool, the pool goes deeply negative (measured: 52 becomes -247.77)
  and stays unusable until regeneration climbs back through the overdraft.

So the fix matters most for builds like this one. It does the same thing for any change of
maximum magicka, from a +10 ring to a potion, but the size of the problem scales with the
size of the swing.

It also changes what refills the pool. With the fix there is no free refill on activation,
so regeneration does most of the refilling, alongside potions and other restore effects,
and regeneration grows with the square of Willpower: 11.00 /s at Willpower 100, 22.50 /s at 150, 38.00 /s at 200, halved in combat
([UESP, Willpower](https://en.uesp.net/wiki/Oblivion:Willpower); [`docs/SPEC.md`](docs/SPEC.md)
section 10.7). `[DEDUCTION]`: on a build like this one, fortifying Willpower is the main
lever on the cost of the fix.

## How this project is made

Every line of code and documentation in this repository was written by Claude, Anthropic's
AI model. I do not write or edit the code. That does not make it a prompt-and-hope project:
the AI writes, I direct, and nothing ships on its word alone.

- **Scope is mine.** What the plugin does and refuses to do: the player only, no hooks and
  no code patches, an ASI plugin rather than an OBSE one, which symptoms to fix and which to
  leave out.
- **Decisions are mine.** Design choices come to me with their trade-offs, and I pick: the
  correction model, the shipped defaults, what goes into each version.
- **Iterations are mine.** Each version has one stated purpose, and the next one starts
  from what the previous one showed in game. Nothing is committed, pushed or published
  without my go-ahead.
- **Testing is mine.** Claude can build the plugin but cannot run the game. Every in-game
  result here, including the 4 h 37 min validation of 0.9.1, comes from a session I played
  and a log I sent back.
- **Claims are checked.** Claude has to back every game-mechanics claim with a source (UESP,
  a Nexus page, source code) or a measured log, and tag inferences `[DEDUCTION]`. When a
  claim turned out wrong, it was withdrawn in the open: [`docs/SPEC.md`](docs/SPEC.md) and
  [`docs/FIELD-JOURNAL.md`](docs/FIELD-JOURNAL.md) keep the withdrawn hypotheses next to
  their corrections.

The rules Claude works under are in [`AGENTS.md`](AGENTS.md), the same file any other
contributor reads. Commits written by Claude carry a `Co-Authored-By: Claude` trailer.

## The problem

The engine does not store your current magicka. It stores your maximum and a deficit
(a float that is zero or negative), and computes `current = maximum + deficit` whenever it
needs the value. Nothing adjusts the deficit when the maximum changes, and nothing clamps
the result. Three symptoms follow from that one defect:

- **Free magicka on activation.** Casting Fortify Magicka or Fortify Intelligence raises
  your current magicka by the full amount. At 100/300, a +300 buff gives 400/600.
- **The full magnitude is taken back on expiry**, even if you were nowhere near full.
- **Negative magicka.** When a buff ends while your pool is low, current magicka goes
  below zero: at 100/600, the buff ending leaves you at -200/300, unable to cast until
  regeneration climbs back to zero.

The intended model is a glass of water:

- **enlarging the glass adds no water**: raising the maximum must not grant magicka;
- **shrinking the glass spills only the overflow**: lowering the maximum must only remove
  what no longer fits.

```
100/300 + 300 buff   ->  100/600   (vanilla: 400/600)
100/600, buff ends   ->  100/300   (vanilla: -200/300)
350/600, buff ends   ->  300/300   (vanilla: 50/300)
```

This is a balance change, not only a bug fix: free magicka on activation is gone, and so
are the exploits that rely on negative magicka. The `.conf` explains what that costs the
player, and `iPreset = 1` keeps vanilla behaviour except for the floor at zero.

## How it works

The plugin runs on its own thread and polls the player object. It watches the
attribute-value modifier arrays of the player (magic, script and game modifiers;
Intelligence and Magicka), predicts the change of maximum as
`delta = 2 x delta(Intelligence) + delta(Magicka)`, and on each transition writes
`deficit' = min(deficit - delta, 0)` to `PlayerCharacter + 0x6B8`, then reads it back.

The correction is relative, so it is only right if the game did not already adjust the
deficit itself. That was measured on the build above: it does not. The plugin keeps
checking anyway. Changes that cost nothing at the moment they happen (a buff expiring, an
enchanted item equipped or removed) show the game's own behaviour, and each one is compared
with that assumption. If a game update ever fixes the bug, the log says so in capitals,
because the plugin would then be subtracting the change twice. `iEnginePolicy` controls
this; see [`docs/SPEC.md`](docs/SPEC.md) section 5.

The model itself is `src/MagickaMath.h`; when to apply it is `src/Corrector.h`. Both are
free of any Windows dependency and covered by host unit tests.

Offsets come from the reverse-engineered
[commonlibob64](https://github.com/libxse/commonlibob64) headers
(`include/RE/P/PlayerCharacter.h`) and were confirmed in game on the build above.

### How memory is accessed

Every read of game objects and every write go through `ReadProcessMemory` and
`WriteProcessMemory` on the game's own process (`mpf::ReadMem` / `mpf::WriteMem` in
`src/PeUtil.h`). A page the game releases under us makes the call fail instead of faulting.
That matters because the plugin is built with MinGW-w64, which has no structured exception
handling: an access violation would take the game down with it. That is exactly how 0.1.1
crashed; [`docs/FIELD-JOURNAL.md`](docs/FIELD-JOURNAL.md) has the dump analysis.

The player is a singleton, so a pointer to it lives in a static slot in the executable's
own data sections. The plugin scans those sections (about 8 MB, never decommitted) for a
pointer to an object whose first field is the `PlayerCharacter` vtable, found through RTTI.
A chunked heap sweep remains as an opt-in fallback (`bHeapScan`).

The game's own `GetActorFloatValue` would give the absolute maximum, but calling it from the
plugin's thread hangs or faults inside the game ([`docs/SPEC.md`](docs/SPEC.md) section 9.1),
so `bCallVirtuals` stays 0 and the plugin only ever sees changes of the maximum.

## Status

Validated in game on 6 August 2026 (version 0.9.1, the first version that writes): one
continuous 4 h 37 min session, 938,783 polls, 16 transitions, 14 writes, **no failures, no
error lines, no crash**. The two transitions without a write are the two where the correct
answer is to do nothing.

Current magicka was preserved across every change of maximum, to within one frame of
regeneration. Four of those transitions would otherwise have left the pool below zero, down
to -202.98, for 641.35 points of cumulative overdraft. On the way up, 2,120 points of free
magicka were refused. Enchanted items are covered: an Intelligence ring behaves exactly
like a spell and is handled by the same rule. Reloading a save is handled: the plugin
notices, re-primes, and finds the player object again.

Since 0.9.1 the correction itself has not changed: only the calibration bookkeeping, the
`.conf` text and the log. The current development version, 1.0.0-dev, adds the startup
summary and the character data to the log; it has not been run in game yet. Every number is
in [`docs/SPEC.md`](docs/SPEC.md), sections 4.6 and 4.7.

**Not yet tested in game:** removing a Fortify Intelligence/Magicka item at a partial pool,
ten activation/expiry loops in a row, potions, Drain Intelligence/Magicka, altars and
blessings, and levelling up. [`docs/SPEC.md`](docs/SPEC.md) chapters 8 and 10 list them.

## Repository layout

```
src/
  PCH.h                    Windows includes, version, actor-value indices, default offsets
  Log.h                    5 levels, truncate at start, no rotation, flush per line, line cap
  Config.h                 INI parser + the self-documenting .conf it generates
  MagickaMath.h            the compensation model, no Windows dependency
  Corrector.h              engine-policy calibration + the write decision, no Windows dependency
  HypothesisCalibrator.h   current-versus-maximum resolver, no Windows dependency
  ModelWatch.h             watches the modifier totals, reports each transition
  PeUtil.h                 PE section walk, committed-page snapshot, checked reads and writes
  Rtti.h                   MSVC RTTI walk: mangled name -> locator -> vtable
  Probe.h                  hex dump, float watch, edge-triggered key
  VirtualCallGuard.h       the guard around the (disabled) virtual call
  Plugin.cpp               DllMain, init thread, discovery, observation and correction loop
tests/                     host unit tests (math, calibration, corrector)
tools/
  gen_conf.py              writes the default .conf from Config.h and checks its structure
  package.sh               builds the release zip
  minidump-triage.py       reads a Windows minidump: modules, exception, faulting address
docs/
  SPEC.md                  self-contained specification, measurements and sources
  FIELD-JOURNAL.md         dated measurement history behind the spec, including withdrawn hypotheses
AGENTS.md                  rules and checklists for contributors and coding agents
build.sh                   the reference build: tests, plugin, .conf, import check
CMakeLists.txt, cmake/     alternative build for IDEs and MSVC
.github/workflows/         CI: build.sh, CMake with MinGW-w64 and MSVC, draft releases
```

## Build

Linux or WSL, with MinGW-w64 (POSIX threads variant), a host C++20 compiler and Python 3:

```sh
sudo apt install g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 zip   # Debian/Ubuntu
./build.sh            # unit tests, then build/MagickaPoolFix.asi and build/MagickaPoolFix.conf
./tools/package.sh    # dist/MagickaPoolFix-<version>.zip with a SHA256SUMS
```

`build.sh` fails if any unit test fails, if the generated `.conf` breaks its structural
checks (`tools/gen_conf.py`), or if the plugin imports anything other than `KERNEL32`,
`msvcrt` and `USER32`. The C runtime is linked statically, so no Visual C++ redistributable
is needed. `package.sh` zips the built plugin, its `.conf` and every git-tracked source and
doc file.

### With CMake (IDEs, MSVC)

`build.sh` is the reference: CI runs it and releases come from it. CMake is there for
contributors who prefer an IDE or Visual Studio. With MinGW-w64 it uses the same flags and
produces the same binary layout and the same `.conf`; it also generates the `.conf` when
Python 3 is found.

```sh
# MinGW-w64, from Linux or WSL
cmake -B build-cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake

# MSVC, on Windows
cmake -B build-cmake -A x64
cmake --build build-cmake --config Release

# unit tests, on the host
cmake -B build-tests -DMPF_BUILD_TESTS=ON -DMPF_BUILD_PLUGIN=OFF
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

An MSVC build is a different compiler and runtime from the release binary. Its in-game
behaviour is not covered by the validation above; use the CI artifact or `build.sh` for
anything you report.

### Without a toolchain

Every push and pull request is built by GitHub Actions, including in a fork that enables
Actions. The run's `MagickaPoolFix` artifact is the same zip a release would contain.

Builds are not byte-reproducible: the plugin embeds `__DATE__` and `__TIME__` so every log
states which binary produced it. Each release zip carries a `SHA256SUMS` for its own files.

## Testing

The unit tests replay the transitions measured in game, so a regression shows up against
field data rather than invented numbers. Everything else needs the game: nothing here can
run it, and every in-game claim in [`docs/SPEC.md`](docs/SPEC.md) comes from a log. A test
session changes one thing at a time, starts from backed-up saves, and ends with the log
being read against the scenario. [`AGENTS.md`](AGENTS.md) has the procedure.

## Releasing

1. `MPF_PLUGIN_VERSION` in `src/PCH.h` is the only version number; CMake reads it from
   there. Until 1.0.0 it stays `1.0.0-dev`, and each build is identified by its build time,
   printed on the log's first line. After 1.0.0, bump it whenever the binary changes,
   including any text the plugin writes (the `.conf` and log strings).
2. `./build.sh`, then `./tools/package.sh`. `dist/` only ever holds the current zip.
3. Add a dated entry to [`docs/FIELD-JOURNAL.md`](docs/FIELD-JOURNAL.md).
4. Tag `v<version>`. For `v*` tags, GitHub Actions checks that the tag matches
   `MPF_PLUGIN_VERSION` and attaches the zip to a **draft** release, published by hand.

## Install (for testing)

1. You need an **ASI loader**, usually
   [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (x64). This is
   **not** an OBSE64 plugin and does not need OBSE64.

2. Copy `MagickaPoolFix.asi` into a folder your ASI loader scans. Ultimate ASI Loader reads
   `.asi` files from the folder holding the loader DLL and from its `scripts`, `plugins` and
   `update` subfolders. On Steam, the usual place is
   `OblivionRemastered\Binaries\Win64\plugins\`.

   **Put exactly one copy in exactly one folder.** A copy in two scanned folders is loaded
   twice; a single-instance mutex stops the second one and the log names the duplicate.

3. Launch the game once. `MagickaPoolFix.conf` and `MagickaPoolFix.log` are created **next
   to the .asi**. The plugin never overwrites an existing `.conf`, so delete the old one
   when upgrading if you want the new defaults and text. If that folder is not writable, the
   log falls back to `%TEMP%\MagickaPoolFix.log` and says so.

4. Read the `.conf`. Every option is explained in place, including what the fix costs in
   balance terms. Everything a regular player needs is above the `A D V A N C E D` banner.

**Back up your saves before installing anything that writes to game memory.** The plugin
does not repair a save whose magicka was already corrupted by the vanilla bug.

The plugin finds the game module through `GetModuleHandleW(nullptr)` and the
`PlayerCharacter` vtable through RTTI at runtime, so nothing is tied to the Steam executable
name. But Steam and Game Pass ship separate compilations, and the offsets were only
confirmed on the Steam build. Game Pass is untested.

### Settings you might change

| Setting | Default | Meaning |
|---|---|---|
| `iPreset` | `2` | `0` off (log only), `1` vanilla plus a floor at zero, `2` glass of water, `3` custom |
| `bDryRun` | `0` | `1` computes and logs every decision but writes nothing |
| `iEnginePolicy` | `1` | `1` correct (measured default), `0` work it out from what happens, `2` never correct |
| `bCallVirtuals` | `0` | **Leave at 0.** Calling the game's `GetActorFloatValue` from this thread hangs or crashes the game ([`docs/SPEC.md`](docs/SPEC.md) section 9.1) |
| `iDumpKey` | `0x79` (F10) | Writes a memory dump of the player object to the log. Never bind F5, F8 or F9 (quick save and quick load) |

### The log

`MagickaPoolFix.log`, next to the .asi, truncated at every start and never rotated. It is
opened with `FILE_SHARE_DELETE`, so you can delete or rename it at any time, even while the
game is running.

The first lines are what a bug report needs:

1. a one-line summary: version, `ACTIVE` or `DRY RUN`, build time, the game executable and
   its PE timestamp (and whether it is the validated build), preset, engine policy, and
   whether the `.conf` was found;
2. the loaders and mods in the game process, each with the PE timestamp of its file;
3. the settings that differ from the defaults;
4. once a save is loaded, the character: level, base attributes and skills with their
   modifier totals (`PLAYER` lines, not yet validated in game: [`docs/SPEC.md`](docs/SPEC.md)
   section 10.8).

After that, each change of maximum produces a `DECISION` line, and every write is followed
by `WRITE OK` or `WRITE FAILED`. Level-ups, base attribute changes and skill-ups get a line
each. A status line is written at most once a minute. `iLevel = 3` adds the full startup
detail. Paths are written with the user's profile folder replaced by `%USERPROFILE%`.

## Compatibility

- **The two uncappers by Baestus** (see [Why this exists](#why-this-exists)): loaded
  throughout the in-game validation. They patch the spell cost and damage code; this plugin
  hooks nothing, patches no code, and writes one data field of the player object, so the two
  never touch the same address.
- **UE4SS**: present during the in-game validation.
- **Mods that also ship a `version.dll`**, for example
  [OblivionRemasteredPerformanceFix](https://github.com/LTSharpe/OblivionRemasteredPerformanceFix):
  only one file can be named `version.dll`. Ultimate ASI Loader also works under other
  names; the Steam executable imports `winmm.dll`, so renaming the loader to `winmm.dll`
  frees `version.dll` for the other mod. Not yet tested in game with this plugin.

## Limitations

- **Tied to one build.** Offsets are confirmed for the executable with PE timestamp
  `0xF19077A4`. The log prints the module name and PE timestamp so each log identifies its
  build. After a game update, check the log before trusting the plugin.
- **The player only.** NPC magicka is not touched.
- **Permanent attribute changes are not seen.** A level-up changes the base attribute, not
  a modifier, so the plugin does not observe it. [`docs/SPEC.md`](docs/SPEC.md) section 10.3
  explains when that can matter, and what is still unmeasured.
- **Cross-thread reads can tear.** The plugin's thread is not the game thread. Non-finite
  and implausible values are discarded, the object's vtable pointer is re-verified before
  each sample, and every write is read back.

## Credits

- **[commonlibob64](https://github.com/libxse/commonlibob64)** (libxse), the
  reverse-engineered layout of the game's classes. Every memory offset this plugin uses
  and the actor-value indices come from its headers, and each offset was confirmed in game
  before the plugin relied on it ([`docs/SPEC.md`](docs/SPEC.md) section 3.4). The plugin
  does not build against it, and contains none of its code: only offsets and indices,
  quoted with their source. commonlibob64 is GPL-3.0.
- **Baestus**, author of the two uncappers this plugin was born alongside:
  [Skill Damage Scaling Cap Remover](https://github.com/Baestus/OBSE64_Skill_Damage_Scaling_Cap_Remover)
  (Nexus [1846](https://www.nexusmods.com/oblivionremastered/mods/1846)) and
  [Attribute Scaling Cap Remover](https://github.com/Baestus/OBSE64_Attribute_Scaling_Cap_Remover)
  (Nexus [2068](https://www.nexusmods.com/oblivionremastered/mods/2068)). Their source was the
  starting point: the plugin's skeleton (`DllMain` only starts an init thread, and all work
  happens off the loader lock) follows their ASI plugins, and their reading of
  `luck_skill_modifier` and of the spell cost formula is what explained the measured spell
  costs ([`docs/SPEC.md`](docs/SPEC.md) section 4.7.3). No code is copied: their
  repositories carry no license ([`docs/FIELD-JOURNAL.md`](docs/FIELD-JOURNAL.md),
  section 12.4).
- **[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)** (ThirteenAG),
  which loads the plugin.
- **[UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)**, whose crash dumps were how the early
  crashes were traced.
- **[UESP](https://en.uesp.net/wiki/Oblivion:Oblivion)**, the main source for the game
  mechanics described in these docs.

## License

[MIT](LICENSE).
