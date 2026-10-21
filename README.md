# MagickaPoolFix

An ASI plugin for **The Elder Scrolls IV: Oblivion Remastered** that fixes how your
current magicka behaves when your maximum magicka changes.

> **This plugin writes to game memory by default.** It writes one float, the player's
> magicka deficit, when a change of maximum magicka needs correcting, and reads it back
> to confirm the write took. No function is hooked and no game code is patched. Set
> `bDryRun = 1` in the `.conf` to watch what it would do without letting it act.

Validated in game on the Steam build whose executable has PE timestamp `0xF19077A4`.
A game update may break it; see [Limitations](#limitations).

---

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
are the exploits that rely on negative magicka. The `.conf` explains what that costs you,
and `iPreset = 1` keeps vanilla behaviour except for the floor at zero.

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
this; see [`SPEC.md`](SPEC.md) section 5.

The model itself is `src/MagickaMath.h`; when to apply it is `src/Corrector.h`. Both are
free of any Windows dependency and covered by host unit tests.

Offsets come from the reverse-engineered
[commonlibob64](https://github.com/libxse/commonlibob64) headers
(`include/RE/P/PlayerCharacter.h`) and were confirmed in game on the build above.

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

Since 0.9.1 only the documentation, the calibration bookkeeping and the `.conf` text have
changed. Every number is in [`SPEC.md`](SPEC.md), sections 4.6 and 4.7.

**Not yet tested in game:** equipping or removing a Fortify Intelligence/Magicka item at a
partial pool, potions, Drain Intelligence/Magicka, altars and blessings, and levelling up.

## Install

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
   to the .asi**. If you later move the .asi, move the `.conf` with it. If that folder is not
   writable, the log falls back to `%TEMP%\MagickaPoolFix.log` and says so.

4. Read the `.conf`. It is the real documentation: every option is explained in place,
   including what the fix costs you in balance terms. Everything a regular player needs is
   above the `A D V A N C E D` banner.

**Back up your saves before installing anything that writes to game memory.** The plugin
does not repair a save whose magicka was already corrupted by the vanilla bug.

### Game Pass

The plugin finds the game module through `GetModuleHandleW(nullptr)` and the
`PlayerCharacter` vtable through RTTI at runtime, so nothing is tied to the Steam executable
name. But Steam and Game Pass ship separate compilations, and the offsets were only
confirmed on the Steam build. Game Pass is untested.

## Settings you might change

| Setting | Default | Meaning |
|---|---|---|
| `iPreset` | `2` | `0` off (log only), `1` vanilla plus a floor at zero, `2` glass of water, `3` custom |
| `bDryRun` | `0` | `1` computes and logs every decision but writes nothing |
| `iEnginePolicy` | `1` | `1` correct (measured default), `0` work it out from what happens, `2` never correct |
| `bCallVirtuals` | `0` | **Leave at 0.** Calling the game's `GetActorFloatValue` from this thread hangs or crashes the game ([`SPEC.md`](SPEC.md) section 9.1) |
| `iDumpKey` | `0x79` (F10) | Writes a memory dump of the player object to the log. Never bind F5, F8 or F9 (quick save and quick load) |

## The log

`MagickaPoolFix.log`, next to the .asi, truncated at every start and never rotated. It is
opened with `FILE_SHARE_DELETE`, so you can delete or rename it at any time, even while the
game is running.

The first line says whether the run is `ACTIVE` or `DRY RUN`. After that, each change of
maximum produces a `DECISION` line, and every write is followed by `WRITE OK` or
`WRITE FAILED`. A status line is written at most once a minute.

## Compatibility

- **Skill and attribute uncappers by Baestus** (Nexus
  [1846](https://www.nexusmods.com/oblivionremastered/mods/1846) and
  [2068](https://www.nexusmods.com/oblivionremastered/mods/2068)): used throughout the
  in-game validation. They patch the spell cost and damage formulas; this plugin hooks
  nothing and patches no code.
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
  a modifier, so the plugin does not observe it. [`SPEC.md`](SPEC.md) section 10.3 explains
  why that is probably harmless, and why "probably" is the right word.
- **Cross-thread reads can tear.** The plugin's thread is not the game thread. Non-finite
  and implausible values are discarded, the object's vtable pointer is re-verified before
  each sample, and every write is read back.

## How memory is accessed

Every read of game objects and every write go through `ReadProcessMemory` and
`WriteProcessMemory` on the game's own process (`mpf::ReadMem` / `mpf::WriteMem` in
`src/PeUtil.h`). A page the game releases under us makes the call fail instead of faulting.
That matters because the plugin is built with MinGW-w64, which has no structured exception
handling: an access violation would take the game down with it. That is exactly how 0.1.1
crashed; [`FIELD-JOURNAL.md`](FIELD-JOURNAL.md) has the dump analysis.

The player is a singleton, so a pointer to it lives in a static slot in the executable's
own data sections. The plugin scans those sections (about 8 MB, never decommitted) for a
pointer to an object whose first field is the `PlayerCharacter` vtable, found through RTTI.
A chunked heap sweep remains as an opt-in fallback (`bHeapScan`).

## Build

Linux or WSL, with MinGW-w64 (POSIX threads variant), a host C++20 compiler and Python 3:

```sh
sudo apt install g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64   # Debian/Ubuntu
./build.sh            # unit tests, then build/MagickaPoolFix.asi and build/MagickaPoolFix.conf
./tools/package.sh    # dist/MagickaPoolFix-<version>.zip with a SHA256SUMS
```

`build.sh` fails if any unit test fails, if the generated `.conf` breaks its structural
checks (`tools/gen_conf.py`), or if the plugin imports anything other than `KERNEL32`,
`msvcrt` and `USER32`. The C runtime is linked statically, so no Visual C++ redistributable
is needed.

CMake also works, with MinGW-w64 or MSVC:

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build
cmake -B build -A x64 && cmake --build build --config Release            # MSVC, on Windows
cmake -B build-tests -DMPF_BUILD_TESTS=ON && cmake --build build-tests && ctest --test-dir build-tests
```

Builds are not byte-reproducible: the plugin embeds `__DATE__` and `__TIME__` so every log
states which binary produced it. Each release zip carries a `SHA256SUMS` for its own files.

GitHub Actions runs `build.sh` on every push and attaches the zip to a draft release for
`v*` tags.

## Layout

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
SPEC.md                    self-contained specification, measurements and sources
FIELD-JOURNAL.md           dated measurement history behind the spec, including withdrawn hypotheses
upstream-issue-1846.md     draft bug report for Nexus mod 1846, found while measuring spell costs
```

## License

[MIT](LICENSE).
