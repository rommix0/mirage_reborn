# Mirage Emulator — Plan 5: The VST3 Plugin

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `Mirage.vst3`, a VST3 instrument that runs the same emulated Mirage as
`Mirage.exe` inside a DAW. It has the replica panel as its editor, disks inside the
plugin, exact recall from the DAW project, sample-accurate MIDI in and out, and the
DAW's audio as the input jack. It also does two things the standalone does not:
the boot calibration is muted, and disk loads are near-instant.

**Architecture:** There are two new behaviours in the shared code, both off by
default, so the app is untouched:
- In the engine, an output gate closes while the CPU runs the boot ROM's
  calibration routine `$F571-$F5E6`.
- In `RealtimeHost`, a fast-forward runs the machine faster than real time, with
  silent output, while the drive motor is on or the gate is closed.

`RealtimeHost` also gains:
- a `process` overload that takes MIDI events with frame offsets, with a backlog
  for long SysEx;
- a snapshot request and machine-swap pair for plugin state, which never
  allocates on the audio thread;
- `pumpWhileIdle()`, for when the DAW has stopped calling `processBlock`.

The new `plugin/` folder is a `juce_add_plugin` VST3 target around those. It
reuses the app's `PanelComponent`, `ImportDialog` and `InputMeter`, plus disk-menu
logic moved out of `MainWindow` into a shared `DiskActions`.

**Tech Stack:** C++20, CMake ≥ 3.20, MSVC 2019 (v142, x64), Catch2 v3.7.1, JUCE
8.0.4 (app and plugin only).

**Spec:** `docs/specs/2026-11-03-vst3-plugin-design.md` (approved 2026-11-03).
It extends `docs/specs/2026-10-18-mirage-emulator-design.md` and
`docs/specs/2026-10-22-audio-input-sampling-design.md` §9. Background:
- `docs/app.md`: threading model, queues and decisions;
- `docs/hardware/boot-rom.md` §9 and §13: `$F571`, the boot timeline;
- `docs/hardware/filters.md` §2: what `$F5DC`/`$F5E7` write.

## Global Constraints

- **Faithful emulation only** (main spec §3). No OS-specific code, no OS patching,
  and no writing OS RAM or OS parameters. The gate and the fast-forward **never
  change emulated state**:
  - A machine run with them must reach the same CPU, RAM, wave RAM, chip and
    disk state as one run without them, cycle for cycle. Only the audio-path
    output and the wall-clock pacing may differ.
  - `host/` and `plugin/` never call `debugRead`, `debugWrite`, `writeWaveRam`
    (other than WAV import, as today) or anything else that writes machine state.
    Reading `cpu()` for the PC is the engine's job, not the host's.
- **The standalone is unchanged.** Both behaviours default to off and the app
  never turns them on. Every existing test passes unchanged; if one changes, the
  default-off invariant is broken. Fix the code, never the test.
- **Snapshot format stays at version 5.** The gate's ramp and hold and every
  host-side option are not saved (spec §3.1). `kSnapshotVersion` must not move.
- **The engine and `mirage_host` stay JUCE-free.** Only `app/` and `plugin/`
  include JUCE.
- **Real time.** After `prepare()`, nothing on the audio thread allocates, frees,
  locks or does I/O. That covers the block-MIDI path, the backlog, fast-forward,
  snapshot requests, machine swaps and MIDI-out parsing. The allocation-counting
  test (`tests/alloc_counter.h`, `AllocationScope`) enforces it. The plugin may
  call `juce::MidiBuffer::ensureSize` once in `prepareToPlay`.
- **ROM and disk images are the user's.** Never copy them into the repo, a DAW
  project or the plugin binary. The project stores only the snapshot, which holds
  the ROM's SHA-1 and never its bytes.
- **Never write the user's images** under `../disks and manuals`. Tests copy to
  temp files.
- **Panel artwork is finished.** `PanelComponent` is reused as it is. The plugin's
  menu bar and status strip go around it.
- **Branch:** `plan-5-vst3` in `emulator/`, with no worktree (the default asset
  root `..` breaks inside one). LF line endings. Commit prefixes
  `feat:`/`fix:`/`docs:`/`test:`/`build:` with an optional scope.
- **Build and test (Git Bash, from `emulator/`):**
  - `cmake --build build --config Debug --parallel`
  - `ctest --test-dir build -C Debug --parallel 8 --output-on-failure`

  Never `-- /m`. Use Release for listening and for the timing assertions marked
  "(Release)". After Task 1, `build/tests/Debug/mirage_tests.exe` must report
  **no skipped tests**. ctest shows a Catch SKIP as Passed, so check the
  executable's summary.
- **The plugin target** is `MiragePlugin`, `PRODUCT_NAME "Mirage"`, `FORMATS VST3`,
  `VERSION 0.3.0`, `PLUGIN_MANUFACTURER_CODE MrEm`, `PLUGIN_CODE Mrg8`,
  `IS_SYNTH TRUE`, `NEEDS_MIDI_INPUT TRUE`, `NEEDS_MIDI_OUTPUT TRUE`,
  `VST3_CATEGORIES Instrument Sampler`, and `COPY_PLUGIN_AFTER_BUILD FALSE`
  (Common Files needs admin). It is built when `MIRAGE_BUILD_APP` is ON, from
  `add_subdirectory(plugin)` after `app`, because JUCE is fetched in `app/`. Use
  `JUCE_VST3_CAN_REPLACE_VST2=0`, `JUCE_WEB_BROWSER=0` and `JUCE_USE_CURL=0`.

### Decisions made for this plan (record each in the doc named)

| Decision | Choice | Why / cost if wrong | Doc |
|---|---|---|---|
| Calibration mute trigger | Output gate while the CPU PC is in `$F571-$F5E6`, checked once per DOC scan. Armed only for the known ROM SHA-1. | The routine is in the boot ROM (hardware) and every OS calls it. Another ROM is heard as in the standalone. | plugin.md, boot-rom.md §9 |
| Gate timing | Closes over 5 ms, holds 20 ms after the PC leaves the range, opens over 10 ms. Applied after the DC blocker and before the output resampler. | The hold bridges interrupts and the OS code between the 8 calls. The DC blocker's state matches an unmuted machine, so the output is bit-identical again once the resampler kernel (32 scans) has passed. Retune by ear. | plugin.md |
| Fast-forward trigger | `driveActive() \|\| outputMuted()` at a block's start | Hardware signals only. Silent stretches are skipped instead of waited out. | plugin.md |
| Fast-forward pacing | Chunks of 2,000 E cycles (1 ms). Real time: until the trigger ends or half the block's wall-clock duration is used. Offline: until the trigger ends, capped at 60 s emulated per block. A block whose trigger ended renders normally with a 10 ms fade-in; otherwise it is silent. | About 12× real time on the owner's PC, adapting to slower PCs. The cap keeps an unformatted disk at "ud" from hanging a bounce. Cost: sound an OS plays during a load is skipped. | plugin.md |
| Input during a fast-forward | The block's MIDI, keys, inserts and wave writes are applied before the chunks. Key releases and swap inserts that fall due are applied between chunks. | Emulated-time timers (key hold, swap gap) still hold. | plugin.md |
| Host MIDI in | `process(..., std::span<const BlockMidiEvent>)` queues each event at its frame, with no added latency. A 256 KB audio-thread backlog feeds the machine's 4096-byte ring at segment ends. Overflow is dropped and counted. | Sample-accurate. Long SysEx dumps from a DAW track survive. | plugin.md |
| MIDI out | Drained on the audio thread after each block, parsed by `MidiByteParser` with a preallocated 64 KB SysEx buffer (a longer SysEx is dropped and counted), placed at the frame its cycle maps to, and clamped. | No extra thread. Cost: a SysEx over 64 KB from the OS is lost. | plugin.md |
| State taking | The state thread posts a buffer reserved to the last snapshot's size + 64 KB. The audio thread fills it with `Machine::saveStateInto`. If no block runs within 200 ms, `pumpWhileIdle()` does it. | No allocation or pause on the audio thread. The idle path covers a stopped DAW. | plugin.md |
| State restoring | A new `Machine` is built off the audio thread and swapped in at a block start, and the old one goes back to be freed off the audio thread. The blob stays "pending" until the swap is confirmed, and `getStateInformation` returns it meanwhile. | Exact recall. A save during the swap returns the right state. | plugin.md |
| Unrestorable state | The blob is kept and returned unchanged. The plugin outputs silence. Reset/Open/New Blank asks before discarding. | No silent loss of the project's Mirage. | plugin.md |
| State format | `juce::ValueTree` "MirageState", `version` 1, GZIP-compressed (`juce::GZIPCompressorOutputStream`) | Already in JUCE. Wave RAM and empty sectors compress well. | plugin.md |
| Buses | Stereo output (mono allowed, same signal on all channels). "Input Jack" auxiliary input, mono or stereo, off by default; channels averaged; inactive means `nullptr`. | Spec §4.1. Hosts show it as a sidechain. | plugin.md |
| Parameters | `volume` (0..1, default 1) and `inputGain` (0..4, default 1, app skew) in an `AudioProcessorValueTreeState`. The volume is smoothed per block (`juce::LinearSmoothedValue`, 20 ms). | As the app's sliders. They can be automated. | plugin.md |
| ROM lookup | Plugin settings → `mirage.bin` beside the `.vst3` bundle → the app's `romPath` (read only) → `MIRAGE_ROM` → chooser. Loaded once per process. | Spec §4.7 | plugin.md |
| Plugin settings | `%APPDATA%\MirageEmulator\MiragePlugin.settings`: `romPath`, `lastDiskDir`, `lastWavDir`. One `juce::ApplicationProperties` shared by instances (`juce::SharedResourcePointer`) with an inter-process lock. | It never touches the app's file. | plugin.md |
| Disk asset root | `MIRAGE_ASSET_ROOT` defaults to `../disks and manuals` | The library was renamed. Asset tests were silently skipping. | AGENTS.md, README |

## Review Focus

1. **The DAW stops calling `processBlock`** (transport stopped in a host that
   suspends plugins, audio engine off, project loading). Then `getStateInformation`,
   `setStateInformation` and *Save Disk* must still complete, within about 1 s, and
   never hang. Test: Task 5 (`snapshot and restore complete with no process() calls`)
   and Task 6 (smoke tool: state taken before `prepareToPlay`).
2. **A sample-rate or block-size change after a restore** (`prepareToPlay` again).
   The swapped-in machine is prepared at the new rate and nothing allocates. Test:
   Task 5 (`a restored machine renders at a new rate after prepare`).
3. **A corrupt, truncated, newer or wrong-ROM project blob.** No crash, silence, a
   message, and the blob comes back byte for byte from `getStateInformation`. Test:
   Task 6 (`PluginState` decode of garbage, a truncated GZIP, `version` 2, a
   snapshot with a different ROM SHA-1).
4. **An unformatted or unreadable disk keeps the drive motor on for ever**, for
   example a blank disk at "ud". The fast-forward must not hang an offline bounce or
   overrun a real-time block. Test: Task 3 (`offline fast-forward stops at the
   cap`, `real-time fast-forward stays within half the block`).
5. **A block bigger than `prepare()` announced, or zero frames, with MIDI events**
   (some hosts change the block size without calling `prepareToPlay`). Events land in
   order, nothing allocates beyond the existing chunking, and zero frames is a no-op
   that still applies the queues. Test: Task 4 (`block MIDI with frames above the
   prepared size`, `zero-frame block`).

---

## File Structure

```
emulator/
  CMakeLists.txt                          asset root default; add_subdirectory(plugin)
  engine/include/mirage/machine.h         setOutputMuteRange/clearOutputMuteRange/outputMuted;
  engine/src/machine.cpp                  saveStateInto
  engine/include/mirage/state_io.h        StateWriter writing into a caller's bounded vector
  engine/src/state_io.cpp
  host/include/mirage/boot_rom.h          kCalibrationRoutineFirst/Last
  host/include/mirage/realtime_host.h     HostOptions, setOffline, fast-forward, BlockMidiEvent
  host/src/realtime_host.cpp              process overload, backlog, snapshot/restore,
                                          pumpWhileIdle, new status fields
  host/include/mirage/midi_byte_parser.h  maxSysex, dropped()
  host/src/midi_byte_parser.cpp
  host/include/mirage/disk_session.h      adoptRestored()
  host/src/disk_session.cpp
  app/src/InputMeter.{h,cpp}              moved out of MainWindow (unchanged)
  app/src/DiskActions.{h,cpp}             File-menu disk actions moved out of MainWindow
  app/src/MainWindow.{h,cpp}              uses DiskActions and InputMeter (behaviour unchanged)
  app/CMakeLists.txt                      the new app sources
  plugin/CMakeLists.txt                   MiragePlugin + plugin_host_smoke
  plugin/src/PluginProcessor.{h,cpp}      buses, params, processBlock, state, ROM, DiskSession
  plugin/src/PluginEditor.{h,cpp}         menu bar + PanelComponent + status strip
  plugin/src/PluginState.{h,cpp}          ValueTree encode/decode (shared with the smoke tool)
  plugin/src/PluginSettings.{h,cpp}       MiragePlugin.settings, ROM lookup
  plugin/tools/plugin_host_smoke.cpp      loads Mirage.vst3 through JUCE's VST3 hosting
  plugin/plugin_smoke.cmake               ctest wrapper (needs the ROM and SND-A01)
  tests/test_machine_map.cpp, test_snapshot.cpp, test_boot.cpp, test_realtime_host.cpp,
  tests/test_midi_byte_parser.cpp, test_disk_session.cpp, test_state_io.cpp
  docs/plugin.md                          new: the plugin guide, decisions, acceptance 9
  docs/app.md, docs/hardware/boot-rom.md, docs/specs/2026-10-18-mirage-emulator-design.md,
  AGENTS.md, README.md                    pointers
```

---

### Task 1: The asset root follows the renamed library

**Files:**
- Modify: `CMakeLists.txt:9`, `AGENTS.md` (Build and test), `README.md` (wherever it says `(xtras)`), and `docs/app.md`, `tests/*.cpp` comments only if they name `(xtras)` as a path to use

**Interfaces:**
- Produces: `MIRAGE_ASSET_ROOT` defaults to `${CMAKE_CURRENT_SOURCE_DIR}/../disks and manuals`.

- [ ] **Step 1: Change the default** and reconfigure: `cmake -S . -B build -DMIRAGE_ASSET_ROOT="$(cygpath -m "$PWD/../disks and manuals")"`. The cache keeps the old value, so pass it explicitly.
- [ ] **Step 2: Run** `cmake --build build --config Debug --parallel` and `build/tests/Debug/mirage_tests.exe`. Expected: the summary says `All tests passed` with **no** `skipped`. Then run `ctest --test-dir build -C Debug --parallel 8`. Expected: every test passes (the count is the baseline for later tasks; record it in the commit message). If an asset test now fails because a file moved inside the library, fix the test's relative path, never its assertions, and list each one in the report.
- [ ] **Step 3: Commit** `build: default MIRAGE_ASSET_ROOT to ../disks and manuals (the renamed library)`.

### Task 2: The calibration gate in the engine

**Files:**
- Modify: `engine/include/mirage/machine.h`, `engine/src/machine.cpp`, `host/include/mirage/boot_rom.h`, `docs/hardware/boot-rom.md` (§9 row `$F571`: the routine's extent `$F571-$F5E6`, from the disassembly)
- Test: `tests/test_boot.cpp`, `tests/test_snapshot.cpp`

**Interfaces:**
- Produces (`Machine`):
  - `void setOutputMuteRange(uint16_t first, uint16_t last);` arms the gate for PCs `first..last` inclusive. `void clearOutputMuteRange();` disarms it and opens the gate at once. Disarmed by default and after construction. `powerOn()` keeps the range and opens the gate.
  - `bool outputMuted() const;` true while the gate is not fully open: closing, closed, holding or opening.
  - Per DOC scan (in the `onFrame` path, so during `runCycles` too): if the PC is in the range, the gate closes (gain falls by `scanCycles / kMuteCloseCycles` per scan to 0) and the hold restarts. Otherwise, after `kMuteHoldCycles` out of range, the gain rises by `scanCycles / kMuteOpenCycles` per scan to 1. Constants: `kMuteCloseCycles = 10'000` (5 ms), `kMuteHoldCycles = 40'000` (20 ms), `kMuteOpenCycles = 20'000` (10 ms).
  - `render` multiplies each scan's DC-blocked, gain-scaled mix by the gate gain before it enters the output resampler.
  - Not saved. After `loadState`, the gate is closed and holding if the restored PC is in an armed range, and open otherwise.
- Produces (`boot_rom.h`): `inline constexpr uint16_t kCalibrationRoutineFirst = 0xF571, kCalibrationRoutineLast = 0xF5E6;`

- [ ] **Step 1: Write the failing tests** (`[assets]` where they boot a disk):
  - `calibration gate: muted for the whole calibration, unchanged elsewhere`. Two machines boot SND-A01 from power-on to 9 s in 256-frame blocks at 48 kHz; B has `setOutputMuteRange(kCalibrationRoutineFirst, kCalibrationRoutineLast)`. A `setTrace` on A records the first cycle the PC enters the range and the last cycle it is in it. Then:
    - B's output is exactly 0.0f from first entry + 6 ms to last exit + 20 ms;
    - A's output peaks above 0.1 in that window (the tones exist);
    - A and B are bit-identical before first entry and from last exit + 60 ms on;
    - `cycles()`, RAM `$8000-$BFFF`, sample RAM and `display()` match at 9 s.
  - `calibration gate: masos and Soundprocess are muted the same way`. Do the same for `EnsoniqOS/masos` and `Soundprocess/Soundprocess OS/Soundprocess OS.img`, checking only the silent window and the peak in A.
  - `calibration gate: disarmed by default and by clear`. With no range, `outputMuted()` is never true over the boot. After `clearOutputMuteRange()` mid-calibration it is false at once.
  - `calibration gate: derived after loadState`. Take a snapshot mid-calibration (PC in range) and load it into an armed machine: `outputMuted()` is true and the next 256 frames are 0.0f. Load it into a disarmed machine: false. The snapshot bytes are identical whether or not the source machine was armed (no version change).
  - `calibration gate: a note after boot is untouched`. On a booted, armed SND-A01 machine, note 60 renders bit-identically to an unarmed one (`$F5E7` is outside the range).
- [ ] **Step 2: Run them; expect FAIL** to compile (no such API).
- [ ] **Step 3: Implement** in `machine.h`/`machine.cpp` per the interface. The PC is `cpu_.regs().pc` at the scan.
- [ ] **Step 4: Run the full suite.** The new tests pass and no existing test changes.
- [ ] **Step 5: Docs.** `boot-rom.md` §9: the extent of `$F571` (it leaves only by `JMP $FC91`) and that the plugin's gate keys on it.
- [ ] **Step 6: Commit** `feat(engine): output gate over the ROM calibration routine (off by default)`.

### Task 3: Fast-forward and gate arming in `RealtimeHost`

**Files:**
- Modify: `host/include/mirage/realtime_host.h`, `host/src/realtime_host.cpp`
- Test: `tests/test_realtime_host.cpp`

**Interfaces:**
- Consumes: Task 2 (`setOutputMuteRange`, `outputMuted`, `kCalibrationRoutine*`, `isKnownBootRom`).
- Produces:
  - `struct HostOptions { bool muteCalibration = false; bool fastDisk = false; };`
  - `explicit RealtimeHost(std::span<const uint8_t> bootRom, HostOptions options = {});` arms the gate only if `options.muteCalibration && isKnownBootRom(romSha1())`.
  - `bool calibrationMuteArmed() const;` (any thread).
  - `void setOffline(bool offline);` any thread (atomic). It selects offline pacing; it only matters with `fastDisk`.
  - `HostStatus` gains `bool outputMuted, fastForwarding;` (the last block was fast-forwarded, entirely or in part) and `uint64_t blocks` (`process()` calls that ran the machine).
  - Constants: `kFastChunkCycles = kCpuClockHz / 1000`, `kFastOfflineCapCycles = 60 * kCpuClockHz`, `kFastBudgetFraction = 0.5`, `kFadeInSeconds = 0.010`.
  - In `process`, after the queues are applied and the block's MIDI has been placed: if `fastDisk` and `driveActive() || outputMuted()`, queue all of the block's MIDI at once, in order, then run chunks with `applyDue()` after each. Stop when the trigger ends, when the budget is used (real time: `std::chrono::steady_clock`, half of `frames / hostRate`), or when the cap is reached (offline). If the trigger ended, render the block normally (no MIDI left to place) with the fade. Otherwise write zeros and set the fade to 0. The fade is a host-side gain from 0 to 1 over `kFadeInSeconds`, applied to rendered output after every block that followed a fast-forward.
  - The app's constructor call is unchanged (default options), so its behaviour is the same.

- [ ] **Step 1: Write the failing tests:**
  - `fast-forward: same machine state as a plain run`. Host H (`fastDisk`, offline) boots SND-A01 in 512-frame blocks at 48 kHz until `status().driveActive` has been false for 2 s of blocks after the lower sound loaded. A bare `Machine` M with the same disk runs `runCycles` until `M.cycles() == H's machine cycles()` (step with `runCycles(1)` at the end). CPU registers, RAM `$8000-$BFFF`, sample RAM, `display()`, `disk()->writeCount()` and the `fdc().sectorsRead()` count are equal.
  - `fast-forward: a factory boot is playable fast (Release)`. In real-time mode, H with `fastDisk` and `muteCalibration` gets `process()` calls paced by wall-clock in 512-frame blocks. The first note-on posted when `display()` reads `DB 06` plays (peak > 0.05) within **3 s** of wall-clock from the insert. Print the measured time and a panel *Load Lower* time for `docs/plugin.md`. `SKIP` in Debug.
  - `fast-forward: offline fast-forward stops at the cap` (Review Focus 4). A blank disk (`DiskImage::blank()`) inserted at power-on (the ROM retries "ud" for ever) gives offline blocks that advance at most `kFastOfflineCapCycles + kFastChunkCycles + frames' cycles` each and return.
  - `fast-forward: real-time fast-forward stays within half the block` (Review Focus 4). The same blank disk in real time: each 512-frame block at 48 kHz returns within the block's own duration, 10.7 ms, of wall-clock (the budget is half of it; Release; `SKIP` in Debug).
  - `fast-forward: off by default`. Default options over a SND-A01 boot give the same output, sample for sample, as today's `process` (compare against a second default host).
  - `fast-forward: input during a fast-forward is kept`. A note posted in a fast-forwarded block is taken (`midiBytesTaken` grows by 3) and reaches the machine's MIDI queue. A key pressed and released in one fast-forwarded block is held at least `kMinKeyHoldCycles`.
  - `fast-forward: fade-in after the trigger ends`. The first rendered block after a fast-forward starts at 0 and reaches the unfaded signal by `kFadeInSeconds`.
  - `gate arming: unknown ROM`. A ROM with the unreached NOP filler byte at `$F5B8` changed to `$00` gives `calibrationMuteArmed() == false`, and the calibration is audible.
  - The allocation test runs fast-forwarded blocks, both real-time and offline.
- [ ] **Step 2: Run them; expect FAIL** to compile.
- [ ] **Step 3: Implement** per the interface.
- [ ] **Step 4: All tests green** (Debug, then Release for the timing tests). Note the two measured times in the report.
- [ ] **Step 5: Commit** `feat(host): fast-forward while the drive runs or the gate is closed; arm the calibration gate`.

### Task 4: Host MIDI with frame offsets, the backlog, and MIDI out without allocation

**Files:**
- Modify: `host/include/mirage/realtime_host.h`, `host/src/realtime_host.cpp`, `host/include/mirage/midi_byte_parser.h`, `host/src/midi_byte_parser.cpp`
- Test: `tests/test_realtime_host.cpp`, `tests/test_midi_byte_parser.cpp`

**Interfaces:**
- Produces:
  - `struct BlockMidiEvent { uint32_t frame; std::span<const uint8_t> bytes; };`
  - `void process(const float* in, float* out, size_t frames, double hostRate, std::span<const BlockMidiEvent> events);` The events are in frame order, as `juce::MidiBuffer` iterates. Each frame is clamped to `[previous frame, frames - 1]`. The block renders in segments split at the frames, as the timestamped path does, and each event's bytes are fed at its frame. It does not touch the timestamped MIDI queue, and `postMidi` stays as it is for the app. Everything else is as in the existing overload: queues, wave writes, an invalid rate (the events are then counted in `midiDropped`), fast-forward (Task 3: all events at the block start) and status.
  - Backlog: `kMidiBacklogBytes = 256 * 1024`, a ring allocated in the constructor. A message goes straight into the machine's ring only if the backlog is empty and `Machine::kMidiQueueCapacity - midiQueueDepth() >= size`. Otherwise it is appended to the backlog (bytes that do not fit are dropped whole per message and added to `midiDropped`). The backlog is moved into the machine's ring as room allows at every segment end and between fast-forward chunks. Panic and Reset clear it.
  - `HostStatus` gains `size_t midiBacklog` (bytes waiting).
  - `MidiByteParser`: `explicit MidiByteParser(size_t maxSysex = 0);` reserves `maxSysex` bytes; with `maxSysex > 0`, a SysEx longer than that is dropped (counted) and `feed` never allocates. `uint64_t dropped() const;` counts the messages dropped that way. `0` keeps today's unbounded behaviour.

- [ ] **Step 1: Write the failing tests:**
  - `block MIDI: each event enters at its frame`. On a booted SND-A01 host, events at frames 0, 100 and 511 of a 512-frame block are queued at the frame they name. A twin `Machine` renders the same block in pieces split at those frames and calls `queueMidi` between the pieces. Its output, `cycles()` and `midiQueueDepth()` are bit-identical to the host's.
  - `block MIDI: frames above the prepared size` (Review Focus 5). `prepare(256, 48000)`, then a 1000-frame block with events at 10, 600 and 999: all three are taken, in order, and nothing allocates.
  - `block MIDI: zero-frame block` (Review Focus 5). Frames 0 with events: the queues are applied and the events are queued at the current cycle. There is no crash and nothing is rendered.
  - `block MIDI: a 100 KB SysEx goes through the backlog`. `midiBacklog` starts near 100 KB minus the free ring space, falls to 0 after about 33 s of emulated time (320 µs/byte), `midiDropped` stays 0, and a note-on sent after it in the same block is delivered after the SysEx's last byte.
  - `block MIDI: a 300 KB SysEx is dropped whole` (`midiDropped += 300 KB`, backlog unchanged).
  - `block MIDI: panic clears the backlog`.
  - `midi parser: a bounded parser never allocates` (`AllocationScope` over feeding a 60 KB SysEx and notes into `MidiByteParser(64 * 1024)` with an emit function made beforehand) and `midi parser: a SysEx past the bound is dropped` (`dropped() == 1`, later messages still parse).
  - The allocation test runs `process` with block events and a backlog.
- [ ] **Step 2: Run them; expect FAIL.**
- [ ] **Step 3: Implement.** Share the segment loop between the two `process` overloads, so the timestamped path is unchanged (its tests must not move).
- [ ] **Step 4: All tests green.**
- [ ] **Step 5: Commit** `feat(host): sample-accurate block MIDI with a SysEx backlog; bounded MIDI parser`.

### Task 5: Snapshots and restores through `RealtimeHost`

**Files:**
- Modify: `engine/include/mirage/state_io.h`, `engine/src/state_io.cpp`, `engine/include/mirage/machine.h`, `engine/src/machine.cpp`, `host/include/mirage/realtime_host.h`, `host/src/realtime_host.cpp`, `host/include/mirage/disk_session.h`, `host/src/disk_session.cpp`
- Test: `tests/test_state_io.cpp`, `tests/test_snapshot.cpp`, `tests/test_realtime_host.cpp`, `tests/test_disk_session.cpp`

**Interfaces:**
- Produces (engine):
  - `StateWriter` gains a bounded mode that writes into a caller's vector (`out.clear()` first) and reports failure instead of growing past `out.capacity()`. The builder picks the form; the unbounded `take()` path is unchanged.
  - `bool Machine::saveStateInto(std::vector<uint8_t>& out) const;` writes the same bytes `saveState()` returns. It returns false, leaving `out`'s contents unspecified, if they would not fit in `out.capacity()`. It never allocates.
- Produces (host), with a new SPSC "state" queue whose single producer is the caller's state thread:
  - `struct ReturnedSnapshot { std::unique_ptr<std::vector<uint8_t>> bytes; bool ok; };`
  - `bool postSnapshot(std::unique_ptr<std::vector<uint8_t>>&& reserved);` and `std::optional<ReturnedSnapshot> takeReturnedSnapshot();` The audio thread fills the snapshot at the next block start, after the UI queue and wave writes (so a just-posted key or insert is in it), and hands it back. `ok == false` means the buffer was too small; the caller retries with `2 × capacity`.
  - `std::unique_ptr<Machine> buildMachine(std::span<const uint8_t> snapshot) const;` (state thread). It builds a Machine from the host's copy of the ROM, calls `loadState` (which throws `StateError` as `Machine::loadState` does) and `prepare` at the last prepared size and rate (if any), and applies the gate range if armed.
  - `bool postRestore(std::unique_ptr<Machine>&& m);` At a block start the audio thread: wires `m`'s `onMidiOut`; swaps it in; clears host per-machine state (held-key releases due, the MIDI backlog and the host's MIDI queue counted as flushed, `pressCycle_`, `ejectCycle_`); hands a pending swap insert back as `Unused`; and returns the old Machine. `std::unique_ptr<Machine> takeReturnedMachine();` (null when none) is the confirmation that the swap happened, and the caller frees the old machine.
  - `bool pumpWhileIdle();` (any non-audio thread). If no `process()` call holds the machine, it claims it (atomic flag), does the block-start work (stalled returns, UI queue, wave writes, state queue), publishes status, releases, and returns true. Otherwise it returns false. A `process()` call that finds the machine claimed writes zeros, advances nothing, and returns.
  - `HostStatus::blocks` (Task 3) lets a caller see that blocks are running.
- Produces (`DiskSession`): `void adoptRestored(const std::filesystem::path& path, DiskFormat format, bool untitled);` sets the identity of a disk that arrived inside a restored machine (no insert posted). `title()` and `startSave()` then behave as if the disk had been opened from `path`. Dirtiness comes from the restored disk.

- [ ] **Step 1: Write the failing tests:**
  - `state_io: bounded writer fails instead of growing` and `machine: saveStateInto equals saveState and never allocates` (`AllocationScope`, with a buffer reserved to the size). `machine: saveStateInto fails on a small buffer`.
  - `host: snapshot taken mid-boot restores and continues identically`. Host A boots SND-A01 for 9 s in 512-frame blocks. `postSnapshot`, one more block, `takeReturnedSnapshot` → bytes. Host B (same ROM): `buildMachine(bytes)`, `postRestore`, one block, `takeReturnedMachine()` is non-null. Then A and B process the same 200 blocks with the same block MIDI (a note at 12 s), and their outputs and `status()` are bit-identical.
  - `host: snapshot and restore complete with no process() calls` (Review Focus 1). With no `process()` call ever made, `postSnapshot` + `pumpWhileIdle()` returns the snapshot. `postRestore` + `pumpWhileIdle()` swaps, and `takeReturnedMachine()` confirms.
  - `host: pumpWhileIdle excludes process`. A thread calls `process()` in a loop while another calls `pumpWhileIdle()` 1,000 times. There are no data races under the existing test pattern (the counts of snapshots returned match those posted), and the blocks that found the machine claimed are silent.
  - `host: a restored machine renders at a new rate after prepare` (Review Focus 2). Restore, `prepare(1024, 96000)`, then `process` at 96 kHz: audio renders and nothing allocates.
  - `host: wrong ROM`. `buildMachine` with a snapshot from a different ROM throws `StateError` "snapshot was taken with a different boot ROM".
  - `disk session: adoptRestored names and saves the restored disk`. After a restore whose disk is dirty, `title()` is `"SND-A01.img (modified)"`, and `startSave()` writes that path in the adopted format.
  - The allocation test covers a snapshot request and a swap.
- [ ] **Step 2: Run them; expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: All tests green.** `kSnapshotVersion` is still 5.
- [ ] **Step 5: Commit** `feat: snapshot requests, machine swaps and an idle pump in RealtimeHost`.

### Task 6: The plugin processor, its state and the VST3 smoke test

**Files:**
- Create: `plugin/CMakeLists.txt`, `plugin/src/PluginProcessor.{h,cpp}`, `plugin/src/PluginState.{h,cpp}`, `plugin/src/PluginSettings.{h,cpp}`, `plugin/tools/plugin_host_smoke.cpp`, `plugin/plugin_smoke.cmake`
- Modify: `CMakeLists.txt` (`add_subdirectory(plugin)` inside `if(MIRAGE_BUILD_APP)`, after `app`)

**Interfaces:**
- Consumes: Tasks 3-5.
- Produces (`PluginState.h`, JUCE, shared with the smoke tool):
  - `struct PluginStateData { juce::MemoryBlock machine; juce::String diskPath; int diskFormat = 0; bool diskUntitled = false; float volume = 1, inputGain = 1; int editorW = 0, editorH = 0; };`
  - `juce::MemoryBlock encodePluginState(const PluginStateData&);` writes the ValueTree "MirageState" (`version` 1) through `juce::GZIPCompressorOutputStream`.
  - `std::optional<PluginStateData> decodePluginState(const void* data, size_t size);` returns nullopt for anything that is not a version-1 "MirageState" (garbage, truncated GZIP, `version` 2).
- Produces (`PluginSettings.h`): `juce::PropertiesFile& pluginSettings();` (process-wide, `SharedResourcePointer`, `MiragePlugin.settings` in folder `MirageEmulator`, `processLock`). `std::optional<juce::MemoryBlock> findBootRom(juce::File& found);` follows the lookup order in the decision table, reads only 4096-byte files, and caches the bytes once per process. `bool setBootRom(const juce::File&);` is for the editor's chooser.
- Produces (`MirageProcessor : juce::AudioProcessor`):
  - Buses: output "Output" stereo (mono allowed), and an aux input "Input Jack", mono or stereo, disabled by default. `isBusesLayoutSupported` accepts output mono or stereo and jack disabled, mono or stereo.
  - The `AudioProcessorValueTreeState` parameters `volume` and `inputGain` (decision table).
  - Its `RealtimeHost` is built with `HostOptions{ .muteCalibration = true, .fastDisk = true }` once a ROM is found. With no ROM, `processBlock` clears the output.
  - `prepareToPlay` calls `host.prepare(maxBlock, rate)` and reserves the jack's mono buffer, the MIDI-out `MidiByteParser(64 * 1024)` and its emit function, and `midiMessages.ensureSize(...)`.
  - `processBlock`:
    1. `setOffline(isNonRealtime())`;
    2. the jack (`nullptr` if the bus is disabled or has no channels, otherwise the average × the `inputGain` param through `setInputGain`);
    3. `BlockMidiEvent`s from the `MidiBuffer`;
    4. `host.process`, into channel 0;
    5. the smoothed volume, then a copy to the other output channels;
    6. MIDI out: clear the buffer, drain `popMidiOut`, parse, and place each message at `clamp((cycle - c0) * frames / (c1 - c0), 0, frames - 1)`, with `c0` and `c1` = `status().cycles` before and after.
  - Owns the `DiskSession` and a 10 Hz `juce::Timer` that runs `DiskSession::poll()`, frees `takeReturnedMachine()`/returned disks, and finishes pending restores.
  - `getStateInformation` (callers serialised by a `std::mutex`, never waiting on the message thread):
    - a pending or unrestorable blob is returned as it is;
    - otherwise `postSnapshot` with a buffer of the last size + 64 KB, waiting up to 200 ms (`takeReturnedSnapshot` polled each 1 ms), then `pumpWhileIdle()` retried for up to 1 s;
    - the result is encoded with the `DiskSession` identity, the parameters and the editor size.
  - `setStateInformation`: decode. On failure, or when the ROM is missing, the ROM SHA-1 differs or `buildMachine` throws: keep the raw blob as unrestorable, run no machine, and set the message the editor shows (`restoreProblem()`). Otherwise `postRestore`, keep the blob pending until `takeReturnedMachine()` confirms, call `pumpWhileIdle()` if no block has run since (`status().blocks` unchanged after 200 ms), then `adoptRestored` and set the parameters.
  - `getTailLengthSeconds()` returns `std::numeric_limits<double>::infinity()`, and the latency is 0.
- Produces (`plugin_host_smoke`): a JUCE console app (`juce_add_console_app`) linking `mirage_engine`, `juce_audio_processors` and `juce_audio_utils`. Usage: `plugin_host_smoke <Mirage.vst3> <rom> <SND-A01.img> [--editor-png out.png]`; exit 0 on success, 1 on a failed check (with the reason on stderr), 2 for usage.

- [ ] **Step 1: Write `plugin_host_smoke`** to fail first (the plugin does not exist yet). Through `juce::VST3PluginFormat` it:
  1. loads the plugin and checks one stereo output, an aux input disabled by default, MIDI in and out, and 2 parameters;
  2. calls `getStateInformation` **before** `prepareToPlay` and checks it returns (Review Focus 1);
  3. boots SND-A01 headless with `mirage_engine` to 16 s, wraps `saveState()` with `encodePluginState`, and calls `setStateInformation`;
  4. prepares at 48 kHz/512, processes 2 s of blocks offline with note 60 on at 0.5 s, and checks the peak is > 0.05;
  5. takes `getStateInformation`, loads it into a second instance, processes the same 2 s with the same notes in both, and checks the outputs are bit-identical;
  6. feeds the garbage, truncated, `version` 2 and wrong-ROM blobs (Review Focus 3), checking each instance outputs silence and returns the same bytes from `getStateInformation`.

  `plugin_smoke.cmake` runs it with `MIRAGE_ROM` and `MIRAGE_ASSET_ROOT`'s SND-A01 and prints `SKIP: missing asset` when either is missing. Register the ctest `plugin_vst3_smoke` with that skip regex.
- [ ] **Step 2: Add unit checks** for `decodePluginState` inside the smoke tool (garbage, truncated, `version` 2) and run the ctest. Expect FAIL (no plugin target).
- [ ] **Step 3: Implement** the CMake target (Global Constraints values), `PluginState`, `PluginSettings` and `MirageProcessor`. `createEditor` returns a `juce::GenericAudioProcessorEditor` until Task 7.
- [ ] **Step 4: Build Debug and Release; run the full ctest.** `plugin_vst3_smoke` passes in both and every earlier test is unchanged.
- [ ] **Step 5: Commit** `feat(plugin): Mirage VST3 processor, state and a VST3 smoke test`.

### Task 7: The editor, shared UI and disks in the plugin

**Files:**
- Create: `app/src/InputMeter.{h,cpp}` (moved from `MainWindow.{h,cpp}`, unchanged), `app/src/DiskActions.{h,cpp}`, `plugin/src/PluginEditor.{h,cpp}`
- Modify: `app/src/MainWindow.{h,cpp}`, `app/CMakeLists.txt`, `plugin/CMakeLists.txt` (it compiles `../app/src/PanelComponent.cpp`, `ImportDialog.cpp`, `InputMeter.cpp` and `DiskActions.cpp` and links `MirageBranding`), `plugin/src/PluginProcessor.{h,cpp}` (`createEditor`), `plugin/tools/plugin_host_smoke.cpp` (`--editor-png`)

**Interfaces:**
- Produces (`DiskActions`, message thread, JUCE). These are the app's File-menu disk actions with their choosers and prompts, moved out of `MainWindow` **with no change in behaviour or text**:
  - `DiskActions(mirage::DiskSession& session, std::function<juce::File()> diskFolder, std::function<void(const juce::File&)> setDiskFolder, std::function<void()> changed);`
  - `void open(); void newBlank(); void save(); void saveAs(); void eject();`
  - `void whenSafeToReplace(const juce::String& verb, std::function<void()> then);` is the "Save changes to X before ...?" prompt.
  - `void showSaveOutcome(const mirage::SaveOutcome&);`
  - `bool dialogOpen() const;` (keyboard shortcuts are off while true).
- Produces (`MirageEditor : juce::AudioProcessorEditor`):
  - a `juce::MenuBarComponent` with *File* (Open Disk Image..., New Blank Disk, Save Disk Image, Save Disk Image As..., Eject Disk, Import WAV...) and *Machine* (Reset, Panic), with the app's greying rules;
  - the app's `PanelComponent` on the host, its volume slider attached to the `volume` parameter;
  - a status strip: the disk title, warnings (`illegal ops: N`, `MIDI dropped: N`, "calibration not muted (unknown ROM)"), the Input slider attached to `inputGain`, and an `InputMeter`;
  - resizable with a fixed aspect (`setResizeLimits`, `getConstrainer()->setFixedAspectRatio`), the size saved through the processor;
  - the panel shortcuts from the app;
  - with no ROM: a "Choose the Mirage boot ROM" button in place of the panel, which calls `setBootRom` and builds the host;
  - with a `restoreProblem()`: the message, a "Choose ROM" button, and Reset/Open/New Blank asking "Discard the Mirage saved in this project?" first.

  Closing the editor never prompts (spec §4.6).

- [ ] **Step 1: Move `InputMeter` and the disk actions out of `MainWindow`.** Build the app, run `app_panel_png_smoke` and the full ctest (unchanged), and check the `--panel-png` output is byte-identical to the one built before the move.
- [ ] **Step 2: Extend `plugin_host_smoke --editor-png out.png`**: it creates the editor of an instance restored as in Task 6, sizes it to 1200 px wide, snapshots it and writes the PNG. Add the ctest `plugin_editor_png_smoke` (PNG signature, exit 0). Run it and expect FAIL.
- [ ] **Step 3: Implement `MirageEditor`** and wire `createEditor`.
- [ ] **Step 4: All ctests green.** Look at the editor PNG: the panel is unchanged, the menu bar is above it and the strip below it. Attach the PNG to the report.
- [ ] **Step 5: Commit** `feat(plugin): replica panel editor with disk menus; DiskActions and InputMeter shared with the app`.

### Task 8: Docs and the owner's acceptance 9

**Files:**
- Create: `docs/plugin.md`
- Modify: `docs/app.md` (a pointer under the boot-calibration and no-turbo decisions: the plugin differs, see `plugin.md`), `docs/specs/2026-10-18-mirage-emulator-design.md` (§3 non-goal "The VST3 plugin" → see the plugin design; §6.9 → done in Plan 5), `AGENTS.md` (layout row for `plugin/`; the VST rule now points at `plugin.md`), `README.md` (build and install the plugin)

- [ ] **Step 1: Write `docs/plugin.md`:**
  - build: the target `MiragePlugin`, where the `.vst3` lands (`build/plugin/MiragePlugin_artefacts/Release/VST3/Mirage.vst3`), and copying it to `C:\Program Files\Common Files\VST3` or adding the folder to the DAW's VST3 paths;
  - the ROM lookup;
  - the buses and parameters;
  - the menus;
  - disks in a plugin (§4.6 of the spec);
  - state and recall;
  - the two VST-only behaviours and why they stay faithful, with Task 3's measured times;
  - the threading table;
  - every row of this plan's decision table;
  - the machine runs free (spec §4.2): transport, tempo and play position are ignored, and emulated time stops while the DAW calls no `processBlock`;
  - known limitations: no MIDI clock from the DAW, held notes after reopening (use Panic), sound during a load is skipped, two instances on one image (the last Save wins), a host that withholds program changes, and an OS SysEx out over 64 KB is dropped.
- [ ] **Step 2: Write the acceptance-9 checklist** in `plugin.md`, by ear, in the owner's DAW, each step with what to expect. Each step is a separate acceptance item:
  1. Insert *Mirage* on an instrument track. Choose the ROM if asked.
  2. *File → Open* `Factory Library/IMG/SND-A01.img`. Expect "2." "1" and playable within about Task 3's measured time, with **no calibration tones**.
  3. Play from a MIDI track and record a clip.
  4. *Load Lower* 2 from the panel. Expect the load to be near-instant.
  5. Edit parameter 36 as in acceptance 2. Save the DAW project, quit the DAW and reopen. Expect it to play at once with no boot, parameter 36 still showing the edit, and the strip saying "(modified)" until *Save Disk Image As...* into a scratch folder.
  6. Bounce the clip offline and compare it with the real-time take.
  7. Run a second instance with `EnsoniqOS/masos` (no calibration tones, both play).
  8. Enable the Input Jack sidechain from an audio track and sample as in acceptance 7.
  9. Confirm `Mirage.exe` still sounds its calibration tones and loads at real speed.
- [ ] **Step 3: Full build (Debug and Release) and ctest.** `mirage_tests.exe` reports no skips.
- [ ] **Step 4: Commit** `docs: the VST3 plugin guide and the acceptance-9 checklist`.
