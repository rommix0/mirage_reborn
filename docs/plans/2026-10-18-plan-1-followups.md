# Plan 1 follow-ups (inputs to Plans 2 and 3)

These came out of the per-task reviews and the final whole-branch review of
Plan 1, the engine core.

## Plan 2 (standalone JUCE app): required tasks

**Disk loading speed** (done, e3b1219)
- FDC sector layout: replace the even k/6 slot spacing in `wd1772.cpp`
  `delayToSlot` with cumulative byte offsets (ID, gaps, sync, data, CRC), so
  sequential sectors don't each cost a revolution.
- Effect: sound loading drops from about 35 s to about 7 s. Document it as a
  model choice in `docs/hardware/fdc.md`.
- No turbo or fast-forward during loads. The owner chose realistic timing
  (2026-10-18).

**Real-time safety of `render()`** (done, 3665050)
- Add `Machine::prepare(maxFrames, hostRate)`, which reserves `pending_`.
- Reserve the resampler's `changes_` up front.
- Replace the `std::deque` MIDI input with a fixed ring buffer that counts
  overflows.
- Add a MIDI queue depth and flush API (for MIDI panic).

**Front panel** (done, 478046d; also a `keyDown` query)
- Display persistence: widen `kPersistCycles` (20 ms) past the OS's longest
  scan pass, or integrate the duty cycle. About 1 in 10 polls currently read
  blank.

**Snapshot and state hardening** (done, e141153; DiskImage move-only in d3fc478)
- Bound `credit_` and `nextMidiCycle_` in `loadState`.
- Move `loadState` temporaries (about 150 KB) to the heap.
- Delete the `StateReader` rvalue overload, which can dangle.
- Make `DiskImage` move-only.

**Disk swapping** (done: `insertDisk` d3fc478/ad7dcd9; the 0.5 s swap gap in the threading model, 0b50281/991d98d/7fc2730)
- `insertDisk` returns the previous disk.
- The app swaps as eject, at least 0.5 s of emulated time, then insert, so the
  OS sees PB6 drop.

**MIDI out** (done, 769a20e; snapshot v2)
- Hold TDRE for 640 cycles per byte.
- Pass a cycle stamp to `onMidiOut`.

**Host-facing API** (done, e141153)
- Const or diagnostic `doc()`/`fdc()` accessors, so the host can't detach the
  audio chain's callbacks.

**Decisions to make by ear** (decided)
- Output headroom or a soft clip (`kOutputGain` is 0.5; one voice peaks at
  about 0.26). Decided: keep the gain and add a soft clip (3665050).
- Whether to mute the filter self-oscillation during boot calibration
  (6.3–7.6 s). Decided: not muted. This is what the emulated hardware emits, and muting
  needs a boot-phase detector, an OS-specific heuristic (spec §3). Flag it at
  the acceptance listen; see `docs/app.md`.

**Threading model (final reviewer's recommendation)** (done, 0b50281/991d98d/7fc2730; ejected disks return to the UI)
- The audio thread owns `Machine`.
- UI and MIDI threads post through a lock-free single-producer/single-consumer
  queue: MIDI with sample offsets, keys, insert/eject/reset, snapshot requests.
- The audio thread publishes the display, drive LED and illegal-opcode count
  through atomics.
- Ejected disks return to the UI thread for freeing.

**Host boundary** (done: SHA-1 warning b57ca42; unsaved changes settled before an insert 5d6f8d8/35f3a68)
- Warn if the boot ROM's SHA-1 isn't `ec6ea5613eeafd21d8f3a7431a35a6ff16eed56d`.
- Settle unsaved disk changes before inserting another disk (spec §6.5).

## Plan 3

- `DiskImage::saveFile` must be atomic (write a temp file, then rename) before
  any Save UI ships. Done in Plan 2 (d3fc478/ad7dcd9).
- Other-OS sweep: WD1772 `$D4` index interrupt and the MO bit with h=1; DOC
  swap/AM/sync edge cases, including MAME's even-one-shot-starts-odd-swap
  partner rule, which nothing so far needs.
- WAV import (spec §6.7) replaces sampling from audio input (owner decision,
  2026-10-18). Engine work: a wave RAM write API on `Machine` that the host
  calls between slices, the WAV-to-8-bit converter (no JUCE dependency), and
  a CLI import option. App work: *File → Import WAV…* and the import dialog.
  The OS's own sampling path through filter 7 (OS `$9702`) stays unmodelled;
  the converter input stays silent.

## Decisions made during Plan 1

Each decision below records what it costs if it turns out wrong.

**Setup**
- No worktree; Plan 1 ran on branch `plan-1-engine-core` in a new repo. The
  default asset-root path (`..`) would break inside a worktree.
- `.gitattributes` makes all files LF.

**Interfaces**
- A shared opcode table (`engine/src/opcodes6809.*`) drives both the CPU and
  the disassembler.
- `runCycles` carries overshoot as credit.
- Reset keeps the disk and its unsaved changes.

**Front panel and drive wiring (the ROM wins over MAME)**
- PA3 enables the left digit and PA4 the right one.
- PB6 is a disk-present level, not a latched change flag.
- PB4 is active low.

**WD1772 floppy controller**
- A command started with no disk stays BUSY, as a real drive does. Eject during
  a command aborts it.

**DOC 5503 sound chip**
- Enabled oscillators = ((OER >> 1) & 0x1F) + 1. The plan's OER/2 was wrong.
- One oscillator slot is 2 E cycles. The plan's 4 was wrong.
- A higher resolution setting halves the rate. The plan said it doubled it.
- Interrupts fire in every mode, including the free-run wrap, but only while IE
  is set. There are no latent interrupts.

**Filters**
- `$E408–$E40F` is resonance and `$E410–$E417` is cutoff. The plan's example
  had them reversed.
- The sample-and-holds latch the previous DAC value. That's why every CV writer
  presets the DAC first.

**Audio and tests**
- Note tests wait for boot to finish instead of pressing panel keys.
- Sustain, release and pitch checks use SND-A04 strings, because SND-A01 is
  drums and percussion.
- Release is measured from 1 s to 4 s after note-off.

**Snapshots**
- Snapshot v1 carries the boot ROM's SHA-1.
- `--disk` together with `--snapshot-in` is a usage error.
