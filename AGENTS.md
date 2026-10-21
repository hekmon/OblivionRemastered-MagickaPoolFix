# AGENTS.md

Rules and checklists for anyone working on MagickaPoolFix: contributors, the maintainer
coming back after a break, and coding agents. Read it whole before changing anything.

The code and docs are written by an AI (Claude) under the maintainer's direction: the
maintainer owns scope, decisions, iterations and in-game testing (README, "How this project
is made"). An agent proposes, with the trade-offs, and does not decide alone on anything
that changes what ships, what is public, or the game install.

## Read first

1. [`README.md`](README.md): what the plugin does, build, release.
2. [`docs/SPEC.md`](docs/SPEC.md): the reference. Every number in the code and the docs
   traces back to it.
3. The last three entries of [`docs/FIELD-JOURNAL.md`](docs/FIELD-JOURNAL.md): where the
   work stands.

## Non-negotiable rules

### Claims and sources

- **Never state a guess as fact.** Every claim about game mechanics needs a source: UESP
  (`en.uesp.net/wiki/Oblivion:*`), a Nexus page, source code, or a log measured in game.
- A conclusion drawn from several sources is allowed, tagged `[DEDUCTION]`.
- When sources contradict each other, say so; do not pick one silently.
- When an earlier claim turns out wrong, say so plainly and correct it, in `docs/SPEC.md`
  (with a "withdrawn" note when it mattered) and in a journal entry. Do not defend it.
- Nothing here can run the game. A change can be built and unit-tested, but every in-game
  claim comes from a log the maintainer sends. Say so whenever a binary is handed over.

### Language

- **Everything in the repository is in English**: code, comments, log strings, the `.conf`,
  README, SPEC, the journal. The docs were French until 0.9.7 (21 October 2026); never
  reintroduce French.
- Talk to the maintainer in the language they write in.

### Delivery

- **ASI only, never OBSE.** OBSE crashes on the maintainer's machine. The file is `.asi`,
  not `.dll`; most `.dll` plugins for this game are OBSE plugins and are not
  interchangeable.
- **No hook, no code patch.** The plugin reads game memory and writes one float, the
  deficit at `PlayerCharacter + 0x6B8`. All access goes through `ReadProcessMemory` /
  `WriteProcessMemory` (`src/PeUtil.h`): MinGW-w64 has no SEH, so a raw access violation
  kills the game.
- **`bCallVirtuals` stays 0.** `GetActorFloatValue` called from the plugin's thread hangs
  or faults inside the game (`docs/SPEC.md` section 9.1).
- **Imports**: KERNEL32, msvcrt and USER32 only. `build.sh` enforces it.

### Log

- One file next to the `.asi`, truncated at every start, **never rotated**.
- Startup summary on **one line**. Periodic status no more often than **every 60 s**.

### Keys

- Never bind **F5, F8 or F9** (quick save and quick load). The dump key is **F10**
  (`0x79`); F2, F4 and F11 are also free.

### `.conf`

- Self-documenting, ASCII, CRLF. The plugin writes it only when none exists.
- Everything a regular player does not need sits below the single `A D V A N C E D`
  banner. No version string in the conf text.
- `tools/gen_conf.py` checks all of this by parsing, never by substring search
  (`docs/SPEC.md` chapter 11 explains why).

### Shipped defaults

- `bDryRun = 0`, `iPreset = 2`, `iEnginePolicy = 1`, `iPolicyConfirmations = 1`,
  `bCallVirtuals = 0`.
- The fix works from the first frame. A game update may break it, and that is accepted;
  the log prints the PE timestamp so a broken build is identifiable.

### Code

- Translate an enum with a `switch` that lists every case and has **no `default`**, so a
  new value becomes a compile error.
- The math (`MagickaMath.h`), the decision (`Corrector.h`) and the calibrator
  (`HypothesisCalibrator.h`) stay free of any Windows dependency and covered by host tests.
- Validate every "do nothing" condition against **each** scenario of `docs/SPEC.md`
  chapter 8, by name, not only against the case it was written for.

### Git and GitHub

- Never change git configuration (identity, signing, hooks).
- **Never push, tag, or create or publish anything on GitHub without an explicit
  go-ahead.**

### The game folder

- It belongs to the maintainer. Ask before copying, renaming or deleting anything under the
  game install, and back up whatever is replaced.

## Build and release checklist

After every change:

1. `./build.sh`. It must end with every unit test OK and 0 FAIL, `conf check OK`, and the
   three allowed imports, with 0 compiler warnings.
2. If the binary changes, including any string the plugin writes to the log or the `.conf`,
   bump `MPF_PLUGIN_VERSION` in `src/PCH.h`, the version's only home (CMake reads it from
   there). Comment-only changes do not need a bump.
3. If the build changes, keep `CMakeLists.txt` in step with `build.sh`: same flags, same
   outputs. CI builds both.
4. `./tools/package.sh`. `dist/` holds only the current zip.
5. Every build has a new checksum (`__DATE__` / `__TIME__`). Quote the SHA-256 printed by
   `build.sh` when handing over a binary.
6. Record what changed and why in a dated `docs/FIELD-JOURNAL.md` entry; update
   `docs/SPEC.md` when a claim changes.

## In-game testing

- **One change per session**, so each log has one explanation.
- The maintainer backs up their saves before every session. Every result needs a log.
- First check of every session: the log's first lines show the PE timestamp `0xF19077A4`,
  `ACTIVE`, then the player being found. If the timestamp differs, stop and set
  `bDryRun = 1` before going further.
- The plugin never overwrites an existing `.conf`: replace it along with the `.asi` when the
  conf text changed.
- Role-play rule: never suggest a spell the character could not cast **unarmored and
  unequipped**.
- Open scenarios are in `docs/SPEC.md` chapters 8 and 10. Out of scope unless asked: NPCs
  (10.6), locating the absolute maximum (10.2), repairing already corrupted saves (9.6).

## The test bench

All in-game measurements so far come from one modded installation, described in
`docs/SPEC.md`:

- the game build: chapter 3;
- the loader and plugin folder: section 6.1;
- the other mods: sections 9.5 and 10.7;
- the character: sections 3.3, 4.7.3 and 10.4.

Answers about game mechanics depend on that character and those mods, so read those
sections first. Machine-specific details (install paths, loader versions) are not kept in
the repository.
