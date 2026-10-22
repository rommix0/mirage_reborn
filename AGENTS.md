# AGENTS.md

Ensoniq Mirage Reborn: a cycle-level emulator of the Ensoniq Mirage DSK-8
sampler. It runs the original 4 KB boot ROM and whatever OS is on the disk
image (OS 3.2, MASOS, Soundprocess, resmos32, ...) unmodified. Windows only so
far. Plans 1-4 are done and the owner has passed acceptances 1-8 by ear.

## Layout

| Path | What it is |
|---|---|
| `engine/` | `mirage_engine`: 6809, WD1772, DOC 5503, VIA, ACIA, panel, filters, input jack, `Machine`. JUCE-free, deterministic, snapshot-able. |
| `host/` | `mirage_host`: `RealtimeHost` (audio thread owns `Machine`; lock-free SPSC queues), `DiskSession`, MIDI parser and feeder. JUCE-free. |
| `app/` | JUCE 8 standalone `Mirage.exe` with the replica front panel. |
| `tools/mirage_cli` | Headless runs: `run` (disk, keys, notes, MIDI, `--import`, `--audio-in`, `--wav`, `--save-disk`, snapshots, trace) and `disasm`. |
| `tests/` | Catch2 v3 unit and end-to-end tests, run with `ctest`. |
| `docs/specs/` | The binding designs. Read before changing behaviour. |
| `docs/plans/` | Plans 1-4 and `plan-1-followups.md` (decisions and their costs). |
| `docs/hardware/` | Reverse-engineered hardware facts: `boot-rom.md`, `clocks-and-midi.md`, `fdc.md`, `filters.md`. |
| `docs/app.md` | App guide: menus, threading model, decisions, acceptance checklists, known limitations. |

## Build and test

Run from the folder that contains `emulator/` (Git Bash: use `--parallel`, not `-- /m`):

```
cmake -S emulator -B emulator/build -G "Visual Studio 16 2019" -A x64
cmake --build emulator/build --config Debug --parallel
ctest --test-dir emulator/build -C Debug --parallel 8 --output-on-failure
```

- First configure fetches Catch2 and JUCE 8.0.4. `-DMIRAGE_BUILD_APP=OFF` skips the app.
- Build **Release** for live play; Debug glitches.
- `MIRAGE_ROM` (default `emulator/mirage.bin`) is the boot ROM; `MIRAGE_ASSET_ROOT`
  (default `../(xtras)`) is the disk library. Tests that need a missing asset
  **skip**, so check the skip count, not only the pass count.
- Test and CLI processes route MSVC Debug assertions to stderr
  (`tools/mirage_cli/non_interactive.h`, `tests/non_interactive.cpp`); nothing may pop a dialog.

## Rules

- **Emulate the hardware, never an OS.** No patching OS code, writing OS RAM or
  parameters, or OS-specific shortcuts. Anything that only works with one OS is
  a failure. The one exception is WAV import, which writes wave RAM only.
- **Never add HFE support** (owner decision, permanent).
- **The standalone stays faithful:** realistic disk timing (no turbo), and the
  boot filter-calibration tones are not muted. A future VST3 must instead mute
  the calibration and make floppy access instant.
- **Evidence order for hardware behaviour:** the boot ROM and OS disassembly win
  over MAME. MAME's `enmirage.cpp` and chip cores are references only, never
  copied (keeps the engine GPL-free). Record findings in `docs/hardware/`.
- **Engine and host stay JUCE-free and deterministic.** Every chip's state goes
  in the snapshot; bump `kSnapshotVersion` (`engine/include/mirage/machine.h`) when the
  format changes.
- **The audio thread never allocates or blocks**: talk to `Machine` only through
  `RealtimeHost`'s queues.
- **Never write to the user's library images** under `(xtras)`; tests copy to temp files.
- Fidelity is judged **by ear by the owner** (no real Mirage to compare
  against). Automated tests can't hear; give the owner a checklist in `docs/app.md`.
- C++20, MSVC 2019 x64, CMake. LF line endings (`.gitattributes`). Commit
  messages use `feat:`/`fix:`/`docs:`/`test:`/`build:`/`chore:` prefixes with an
  optional scope, for example `feat(engine): ...`.

## Useful facts

- E clock 2 MHz from an 8 MHz master; DOC output 29,411.76 Hz with 32 oscillators.
- Disk: 80 tracks × (5 × 1024 + 512) bytes = 450,560-byte images; `.edm`
  (raw or Giebler) is detected by content and saved in the format it was read in.
- A factory disk's first sound is playable about 14.6 s of emulated time after
  power-on; schedule CLI notes at 16 s or later.
- Boot ROM SHA-1 `ec6ea5613eeafd21d8f3a7431a35a6ff16eed56d`; others are allowed with a warning.
