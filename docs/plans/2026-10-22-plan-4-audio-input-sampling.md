# Mirage Emulator — Plan 4: Sampling from Audio Input

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The emulated rear-panel input jack carries a signal, so the disk's
own OS samples into wave RAM as on a real Mirage: *Sample Lower/Upper*, level
detect on the display, *Enter*, threshold, rate, input filter and line/mic, all
done by the OS.

**Architecture:** `Machine::render` gains an input buffer. Each block's input is
resampled from the host rate to the DOC scan rate and enters voice channel 7's
filter, the OS's anti-alias filter. The A/D mux reads it on the line path
(mux 1) or, through the existing compressor, on the mic path (mux 0). With
nothing plugged in, every A/D read is bit-identical to today. `RealtimeHost::process`
passes the device's input block straight through (same callback, no queue),
with a host-side input gain and a published input peak. The app opens an
input device and shows an input level slider and meter. The CLI feeds the jack
from a WAV file for tests.

**Tech Stack:** C++20, CMake ≥ 3.22, MSVC 2019 (v142, x64), Catch2 v3.7.1,
JUCE 8.0.4 (app only).

**Spec:** `docs/specs/2026-10-22-audio-input-sampling-design.md` (approved
2026-10-22), which extends `docs/specs/2026-10-18-mirage-emulator-design.md`.
Background: `docs/hardware/filters.md` §3 (the A/D mux, sampling set-up at OS
`$9702`), `docs/app.md` (threading model, audio settings).

## Global Constraints

- **Faithful emulation only.** No OS-specific code paths, no OS patching, no
  writing OS RAM or OS parameters (main spec §3). The host feeds the input jack;
  the OS does the sampling. `host/` and `app/` never call
  `debugRead`/`debugWrite`/`cpu()`.
- **Nothing plugged in changes nothing.** `render(nullptr, ...)` and the old
  `render(out, ...)` behave exactly as before this plan: every boot timing,
  calibration result and existing test stays as it is.
- **WAV files stay an import feature in the app** (owner, 2026-10-22). No WAV
  player feeds the jack in the app; the CLI's `--audio-in` exists for tests and
  reproducible listening.
- **ROM and disk images are user-owned.** Never copied into the repo. Tests use
  `MIRAGE_ASSET_ROOT` and skip when assets are missing; anything they write goes
  to the build tree or a temp folder.
- **The engine and `mirage_host` stay JUCE-free.** Only `app/` includes JUCE.
- **Real time:** after `prepare()`, nothing on the audio thread allocates,
  frees, locks or does I/O. The allocation-counting test enforces it.
- **Determinism:** same inputs (including the jack signal) → bit-identical
  state and audio. Any change to saved state bumps `Machine::kSnapshotVersion`
  (it is 4 now).
- **Panel artwork is finished. Do not change the replica panel's look**; new
  controls go in the status strip.
- **Branch:** `plan-4-audio-input-sampling` in `emulator/`, no worktree (the
  default asset root `..` breaks inside one). LF line endings.
- **Build/test (Git Bash, from `emulator/`):** `cmake --build build --config Debug --parallel`,
  `ctest --test-dir build -C Debug --parallel 8`. Never `-- /m`. Windows Python
  needs `cygpath -m` paths. Release for listening.

### Decisions made for this plan (record each in the doc named)

| Decision | Choice | Why / cost if wrong | Doc |
|---|---|---|---|
| Where the jack enters | Added to voice channel 7's filter input, every DOC scan, only while the mux selects 0 (x `kMicGain`) or 1 (x 1); mux 2 and 3 ground it (amended by Task 1's schematic finding) | The OS sets channel 7's filter for sampling (OS `$9702`); param 74 then works for free | filters.md §3.3 |
| Line path (mux 1) | With a jack connected: `128 + 127·clamp(mix, −1, 1)` rounded, clamped to 1..255, where mix = the whole voice mix (all 8 filters, uncompressed; amended: not channel 7's output alone). With nothing connected: `$80` as today | Hard clip as the guide warns; nothing connected unchanged | filters.md §3.1 |
| Mic path (mux 0) | The jack signal is multiplied by `kMicGain` (11, +20.8 dB, the schematic's resistor ratio; amended from 10) before filter 7 while the mux selects 0; mux 0 still reads `compressedAdcCode(mix)` | The guide: mic "increases gain and also puts a compressor on the signal". A silent jack leaves mux 0 bit-identical, so boot calibration is untouched. Gain may still be tuned by ear. | filters.md §3.1 |
| Input units | ±1.0 at the jack = full scale of the line path | Simple; the host input gain sets the real level | app.md |
| Input timing | Host frames resampled (a second `StreamResampler`, host rate → DOC scan rate) and popped one per DOC scan; a shortfall holds the last value | Same machinery as the output; group delay 35 DOC scans = 1.19 ms at 48 kHz (measured and pinned: the first plug-in pre-rolls zeros), deterministic | filters.md §3.3 |
| Host input gain | 0..4, default 1, applied before the jack; saved in settings | Stands in for the mixer/preamp knob the guide recommends; the Mirage has none | app.md |
| Device input to mono | Average of the active input channels | The Mirage samples mono | app.md |

## Review Focus

1. **A silent or absent jack must not move anything.** Boot timings, the
   calibration, `DB 06` settles, every existing acceptance test. Test: Task 2
   (`render(nullptr)` and `render(zeros)` match the old `render` bit for bit
   over a full SND-A01 boot).
2. **Input device appears, disappears or changes rate mid-run** (USB interface
   unplugged, sample-rate change in Audio Settings). No crash, no stale buffer
   read, the jack goes silent. Test: Task 4 (`process` with `in == nullptr`
   after blocks with input; rate change between blocks).
3. **A hot input** (full-scale square, NaN, ±inf from a misbehaving driver).
   Codes stay in 1..255, no NaN reaches the filters. Test: Task 2.
4. **A long sampling run** (a full 64 KB half at 10 kHz, about 6.5 s, while
   MIDI plays). No dropout, no allocation. Test: Task 3 (end to end) and Task 4
   (allocation test with input).
5. **Mic vs line actually differ.** Test: Task 3 (the same input sampled with
   param 75 off reads louder and more compressed than with it on).

---

## File Structure

```
emulator/
  engine/include/mirage/voice_filter.h   FilterBank: external input on channel 7, output(ch)
  engine/src/voice_filter.cpp
  engine/include/mirage/machine.h        render(in, out, ...), input resampler, snapshot v5
  engine/src/machine.cpp                 adcRead mux 0/1 with the jack
  tools/mirage_cli/run_options.{h,cpp}   --audio-in
  tools/mirage_cli/main.cpp
  host/include/mirage/realtime_host.h    process(in, out, ...), input gain, inputPeak
  host/src/realtime_host.cpp
  app/src/AudioEngine.{h,cpp}            input channels, mono mix, input gain
  app/src/MainWindow.{h,cpp}             input level slider + peak meter in the status strip
  tests/                                 test_voice_filter.cpp, test_machine_map.cpp,
                                         test_snapshot.cpp, test_cli_run.cpp,
                                         test_realtime_host.cpp, test_acceptance.cpp
  docs/hardware/filters.md               §3.1, §3.3 sampling evidence and model
  docs/app.md                            input device, level, acceptance 7-8 checklists
  docs/specs/2026-10-18-mirage-emulator-design.md   §2/§3/§5 pointers to the new design
  README.md                              --audio-in
```

---

### Task 1: Evidence — how OS 3.2 samples

A research task with a documentation deliverable; it settles the model Task 2
builds.

**Files:**
- Modify: `docs/hardware/filters.md` (§3.3, and §3.1 if the mux table changes)

- [ ] **Step 1: Disassemble OS 3.2's sampling path.** With `mirage_cli disasm` on the OS as loaded in RAM (dump RAM from a booted SND-A01 via a scratch program or `--trace` at the *Sample Lower* key), follow OS `$9702-$974D` (set-up) into the level-detect loop and the recording loop. Establish: which zero-page bytes hold params 73-77 (confirm `<$18` = param 74, `<$19` = param 75); which mux each param 75 value selects; how the threshold (param 76) is compared; how the recording loop is paced (VIA timer, DOC IRQ or a counted loop) and how often it reads `$ECE2`; where it writes (bank, `$0000-$7FFF` window, page order); what it does to DOC channel 7's oscillators while sampling; how the level is shown on the display. Run the key sequence (*Sample Lower*, then *Enter*) headlessly and trace it.
- [ ] **Step 2: Check MASOS and Soundprocess briefly**: do they use the same mux bits and channel 7? One line each in `filters.md`.
- [ ] **Step 3: Write `filters.md` §3.3** with the evidence (addresses) and state whether it supports the decision table's model. If it contradicts it (for example the line path is read through a different filter, or mux 1 reads the jack unfiltered), write the corrected model there and say so in the report: the controller rules on it before Task 2.
- [ ] **Step 4: Commit** `docs: how the OS samples (filters.md 3.3)`.

### Task 2: The input jack in the engine

**Files:**
- Modify: `engine/include/mirage/voice_filter.h`, `engine/src/voice_filter.cpp`, `engine/include/mirage/machine.h`, `engine/src/machine.cpp`
- Test: `tests/test_voice_filter.cpp`, `tests/test_machine_map.cpp`, `tests/test_snapshot.cpp`, `tests/test_audio.cpp`

**Interfaces:**
- Consumes: Task 1's confirmed model (if it changed the decision table, the controller's ruling).
- Produces (`FilterBank`): `void setExternalInput(float x);` added to channel 7's filter input on the next `process()` (and held until changed); `float output(int ch) const;` the last output of channel `ch`'s filter.
- Produces (`Machine`):
  - `void render(const float* in, float* out, size_t frames, double hostRate);` `in == nullptr` = nothing plugged in. The existing `render(out, frames, hostRate)` calls it with `nullptr`.
  - `static constexpr float kMicGain = 11.0f;`
  - `bool jackConnected() const;` true after a `render` with a non-null `in`, false after one with `nullptr` (saved state).
  - `kSnapshotVersion = 5`: the input resampler and the jack flag are saved.
  - `prepare(maxFrames, hostRate)` also reserves the input path; `render` with input never allocates after it.

- [ ] **Step 1: Write the failing tests:**
  - `machine with nothing plugged in renders exactly as before`: three machines boot SND-A01 from power-on for 20 s in 256-frame blocks at 48 kHz, with `render(out, ...)`, `render(nullptr, out, ...)` and `render(zeros, out, ...)`. Every block's audio and `display()`, and the sample RAM and `cycles()` at the end, are identical across all three (a silent jack adds 0.0f to filter 7, and mux 0 is unchanged).
  - `line input reads the jack through filter 7`: with filter 7 fully open (cutoff CV max, resonance 0), a DC input of 0.5 makes a mux-1 conversion read `128 + round(127 × y)` where y is filter 7's settled output; nothing connected reads `$80`.
  - `mic input adds gain and the compressor`: the same input read through mux 0 gives a larger deviation from `$80` than mux 1 for a small input (0.05), and stays within 1..255 for a full-scale square.
  - `jack input is placed in emulated time`: a single-sample impulse at frame k of a block shows up in filter 7's output at the DOC scan nearest `k / hostRate` after the block start, plus the resampler's fixed group delay (measure it once and pin it).
  - `jack input survives NaN and inf`: such samples are read as 0, no NaN in `filters().output(7)`.
  - `snapshot round-trips a machine with the jack connected mid-input` (version 5), and the version tests reject 4.
  - `render with input allocates nothing after prepare` (`AllocationScope`).
- [ ] **Step 2: Run them; expect FAIL** (no such API).
- [ ] **Step 3: Implement** per the decision table: a second `StreamResampler` (host rate in, DOC scan rate out) fed with the block's `in` samples before `runCycles`, popped once per DOC scan in `doc_.onFrame` before `filters_.process` (`setExternalInput(jack × (mux == 0 ? kMicGain : 1))`, mux = VIA PB3:PB2 at that scan); `adcRead` mux 1 = the line code when `jackConnected()`, else `kLineInputIdle`.
- [ ] **Step 4: Run the full suite.** No existing test may change; if one does, the silent-jack invariant is broken — fix the code, never the test.
- [ ] **Step 5: Docs.** `filters.md` §3.1 mux table rows 0 and 1, §3.3 the jack model and the group delay; the decision-table entries.
- [ ] **Step 6: Commit** `feat(engine): the audio input jack (line and mic paths into the A/D)`.

### Task 3: CLI `--audio-in` and the end-to-end sampling test

**Files:**
- Modify: `tools/mirage_cli/run_options.{h,cpp}`, `tools/mirage_cli/main.cpp`, top-level `CMakeLists.txt`, `tests/test_cli_run.cpp`, `tests/test_acceptance.cpp`, `README.md`

**Interfaces:**
- Consumes: `Machine::render(in, out, frames, rate)` (Task 2); `readWavFile` (`wav_import.h`, mono mix at the file's rate); test helpers in `tests/booted_machine.h` (`bootedMachineAt`, `pressKeys`, `showParameter`, `osSaveLowerSound`).
- Produces (CLI): `--audio-in "FILE@T[,gain=G][,loop=0|1]"` (repeatable; G default 1, loop default 0). From emulated time T the file plays into the jack at its own rate, resampled by the CLI to `--rate` (linear interpolation is fine here; the engine resamples again to the DOC rate). Without `--wav` the run still renders (output discarded) so the input is placed through `render`. A later `--audio-in` replaces the earlier from its T. A missing or unreadable file exits 1 with the `WavError` message before the run; a bad field exits 2 with `bad value for --audio-in: ...`. At the end the CLI prints `audio-in: FILE N frames played` per entry.

- [ ] **Step 1: Write the failing tests:** parse tests for every field and each bad value; a CLI ctest that feeds a generated WAV with no disk and checks the printed frame count.
- [ ] **Step 2: Implement `--audio-in`.**
- [ ] **Step 3: Write `acceptance 7: OS 3.2 samples a sine from the input jack`** (`[assets][acceptance]`): boot SND-A01 (`bootedMachineAt`), render with a 440 Hz sine at amplitude 0.5 through the jack (in-process, not via the CLI), press *Sample Lower* and *Enter* (the exact keys and waits from Task 1), keep feeding until the drive-free sampling run ends (Task 1 says how the OS signals the end). Then:
  - the lower half of sample RAM contains a sine: over the sampled pages the mean distance between rising zero crossings (around `$80`) is `rate / 440` bytes ± 2%, with rate = 1,000,000 / param 73 µs (34 µs default → 66.8 bytes);
  - the peak bytes are within the 8-bit range and not clipped (no run of more than 3 `$01` or `$FF`);
  - a note on the lower keyboard renders with peak > 0.05, and two notes an octave apart differ by 2× in fundamental ± 3%;
  - with param 75 off (mic), the same input gives a larger peak-to-peak in wave RAM than with param 75 on (line) (Review Focus 5).
- [ ] **Step 4: Write `acceptance 7: a silent jack never crosses the threshold`**: param 76 at its default, jack connected but silent, *Sample Lower*, *Enter*: after 5 s nothing has been written (sample RAM unchanged) and the display still shows level-detect.
- [ ] **Step 5: Write `acceptance 7: a sampled sound survives OS save and reopen`**, as in acceptance 3: after the Step 3 sampling, `osSaveLowerSound`, write the disk to a temp file, boot a fresh machine from it, and the same note's peak is within 10% and its fundamental within 3%.
- [ ] **Step 6: README `--audio-in` example; all tests green; commit** `feat: mirage_cli --audio-in; OS 3.2 samples from the input jack (acceptance 7)`.

### Task 4: Input through `RealtimeHost`

**Files:**
- Modify: `host/include/mirage/realtime_host.h`, `host/src/realtime_host.cpp`, `tests/test_realtime_host.cpp`

**Interfaces:**
- Produces:
  - `void process(const float* in, float* out, size_t frames, double hostRate, double nowSeconds);` `in` may be null (nothing plugged in). The existing `process(out, ...)` forwards with `nullptr`. Segments split at MIDI offsets pass `in + pos` with `out + pos`.
  - `void setInputGain(float g);` any thread (atomic), clamped to 0..4, applied on the audio thread into a buffer reserved by `prepare()`.
  - `HostStatus::inputPeak` (float, the largest |sample| of the last block after the gain; 0 with no input).

- [ ] **Step 1: Write the failing tests:** input reaches the machine (a DC block through `process` moves filter 7's output as `Machine::render` does directly); `in == nullptr` after blocks with input makes the jack disconnected; gain 0 gives silence at the jack but the jack stays connected; `inputPeak` reports the block's peak after gain; a rate change between blocks with input works; the allocation test runs `process` with an input buffer.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: All tests green; commit** `feat(host): audio input through RealtimeHost, input gain and peak`.

### Task 5: The app — input device, level, docs

**Files:**
- Modify: `app/src/AudioEngine.{h,cpp}`, `app/src/MainWindow.{h,cpp}`, `app/src/Settings.{h,cpp}` (input gain), `docs/app.md`, `docs/specs/2026-10-18-mirage-emulator-design.md`

**Interfaces:**
- Consumes: Task 4's `process(in, out, ...)`, `setInputGain`, `HostStatus::inputPeak`.
- Produces (app):
  - `deviceManager_.initialise(2, 2, savedState, true)`: *Audio → Audio Settings...* now offers input devices and channels (JUCE's selector). No input device = nothing plugged in.
  - The callback averages the active input channels into a mono buffer reserved in `audioDeviceAboutToStart` (size = the device's block size) and passes it to `process`; no active input → `nullptr`.
  - In the status strip: an **Input** level slider (0 to 4, default 1, saved in the settings) and a small peak meter fed by `inputPeak` at the strip's 10 Hz refresh. Not on the replica panel.
  - A device without inputs, a refused microphone permission, or an input device that disappears: the jack is unplugged; nothing crashes.

- [ ] **Step 1: Implement** the device, mono mix, slider and meter; build Release; render the window with `--panel-png` to check the strip (panel unchanged) and save the PNG to the scratch folder for the review.
- [ ] **Step 2: Docs.** `docs/app.md`: input device, the Input slider and meter, threading (input in the same callback), the decision rows, Known limitations (input stage is a model; external filter cartridge unmodelled; mono). Acceptance checklists, both "not yet run by the owner":
  - **Acceptance 7 (sample live):** start with an audio interface input chosen; *Open* SND-A01, Reset, wait for "2." "1"; play a sound into the input and check the Input meter moves; *Sample Lower*; watch the level-detect display (Task 1 describes it); *Enter*; play the sound; play the lower keyboard; OS-save, *Save As* (scratch folder), Open, Reset, Load Lower 1: it plays the same.
  - **Acceptance 8 (line vs mic):** sample the same sound with param 75 on and off; mic is louder and compressed.
- [ ] **Step 3: Main spec**: §2 goal 4, §3 (remove the audio-input non-goal), §5 (samples) point to `2026-10-22-audio-input-sampling-design.md`; one line each.
- [ ] **Step 4: All tests green (including `app_panel_png_smoke`); commit** `feat(app): audio input device, input level and meter; sampling docs`.
