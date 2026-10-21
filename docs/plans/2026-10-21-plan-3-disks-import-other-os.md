# Mirage Emulator — Plan 3: Saving, Formatting, WAV Import, Other OSes, EDM

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reach spec acceptances 2-6: edits survive OS save + *Save As* +
reopen, blank disks can be formatted by the OS, WAV files import into wave
RAM, every OS in the tree boots and plays, and `.edm` disks open and save.
Also: a paced SysEx feeder and a ctest smoke run of `Mirage --panel-png`.

**Architecture:** Same three layers as Plan 2. `mirage_engine` gains the
WD1772 Write Track command, a wave RAM write API, a JUCE-free WAV converter
and the Giebler EDM codec. `mirage_host` gains wave writes through the
SPSC queues, New Blank Disk and format-aware saving in `DiskSession`, and a
`MidiInFeeder` that paces long SysEx. The JUCE app adds *File → New Blank
Disk*, *File → Import WAV…* with its dialog, and `.edm` in the file dialogs.
Everything testable is in the two libraries and covered by Catch2.

**Tech Stack:** C++20, CMake ≥ 3.22, MSVC 2019 (v142, x64), Catch2 v3.7.1,
JUCE 8.0.4 (app only), as in Plan 2.

**Spec:** `docs/specs/2026-10-18-mirage-emulator-design.md` (§2 acceptances
2-6, §6.5 blank disks, §6.6 EDM, §6.7 WAV import, §7 steps 7-10). Also read
`docs/plans/2026-10-18-plan-1-followups.md` ("Plan 3"), `docs/app.md`
("Known limitations"), `docs/hardware/fdc.md`, `docs/hardware/boot-rom.md`
§13. Plan 2 (`docs/plans/2026-10-18-plan-2-standalone-app.md`) shows the
host/app patterns to copy.

## Global Constraints

- **Faithful emulation only.** No OS-specific code paths, no OS patching, no
  writing OS RAM or OS parameters (spec §3). The host may press panel keys,
  queue MIDI bytes, insert or eject disks and, new in this plan, write wave
  RAM for WAV import (spec §1, §6.7). `host/` and `app/` never call
  `debugRead`/`debugWrite`/`cpu()`. Tests may.
- **ROM and disk images are user-owned.** Never copied into `emulator/` or
  committed. Tests find them through `MIRAGE_ASSET_ROOT` and `SKIP` when
  missing. A test that writes a disk writes it under the build tree or a temp
  folder, never next to the user's assets.
- **Disk images:** `.img` and `.edm`; a raw image is exactly 450,560 bytes,
  any extension. Never `.hfe`.
- **The engine and `mirage_host` stay JUCE-free.** Only `app/` includes JUCE.
- **Real time:** after `prepare()`, nothing on the audio thread allocates,
  frees, locks or does I/O (the allocation-counting test enforces it). Wave
  writes, like disks, cross as `unique_ptr`s and go back to the UI thread to
  be freed.
- **Realistic timing:** no turbo or fast-forward, including during formatting
  (owner, 2026-10-18). Boot calibration tones stay unmuted in the standalone.
- **Panel artwork is finished (2026-10-21). Do not change the panel's look.**
- **Branch:** `plan-3-disks-import-os` in `emulator/`, no worktree (the
  default asset root `..` breaks inside one). LF line endings.
- **Build/test (Git Bash, from `emulator/`):** `cmake --build build --config Debug --parallel`,
  then `ctest --test-dir build -C Debug --parallel 8`. Never `-- /m`. Windows
  Python needs `cygpath -m` paths. Release for listening.

### Decisions made for this plan (record each in the doc named)

| Decision | Choice | Why / cost if wrong | Doc |
|---|---|---|---|
| Write Track parsing | The byte stream from index to index is parsed for `F5 F5 F5 FE t s n z F7` ID fields each followed by `F5 F5 F5 FB <data> F7`. If exactly ids 0-5 appear once each, track byte = head track, side 0, size codes 3,3,3,3,3,2, the track becomes formatted holding the written data bytes; otherwise it becomes unformatted. | Spec §6.5. A non-Mirage layout reads as unreadable, as on hardware. | fdc.md |
| Wave RAM halves | Import targets `$00000-$0FFFF` or `$10000-$1FFFF` of the 128 KB sample RAM; the dialog names them by the OS 3.2 keyboard half measured in Task 4 | Which half is "lower" is an OS convention; the address is hardware | app.md |
| WAV conversion | Mono mix (average), windowed-sinc low-pass + resample (cutoff 0.45 × min(source, target) rate), peak normalise on by default, round to unsigned 8-bit (`$80` = 0), no dither, `$00` written as `$01` | Keeps the Mirage's 8-bit grit; dither would add hiss. Easy to change by ear. | app.md |
| Import while the drive is busy | The UI disables Import while the drive LED is on; the audio thread also refuses a write that arrives while `driveActive()` and hands it back | No race between the UI check and the write | app.md |
| `.edm` files | Two kinds exist in the tree. 450,560-byte `.edm` (Metascape, MUG, K-Muse, Triton `SoundProcess_Vol*.edm`) are raw images. Giebler `.EDM` files start with `0D 0A "Mirage Disk"`, a 512-byte header with a sector bitmap at `$A0`, then the present sectors, per track in the order 5, 0, 1, 2, 3, 4. Both are read; detection is by content, not extension. | Spec §6.6 asks the builder to confirm and handle any header. | fdc.md (new "Disk file formats" section) |
| Saving `.edm` | *Save* writes the format the file was read in. *Save As* writes Giebler EDM for a `.edm` name and a raw image otherwise. The Plan 2 `.edm` save refusal goes. | Spec §6.6 "Saving an `.edm` writes back in `.edm` format"; a raw `.edm` stays raw so other tools keep reading it | app.md |
| Saving a partly formatted disk | Refused at save time with "This disk is not fully formatted (N of 80 tracks). Format it with the OS before saving." The disk stays modified. | Spec §6.5 | app.md |
| SysEx pacing | All MIDI input goes through a `MidiInFeeder` (mutex-guarded, never on the audio thread). Messages go straight to `postMidi` with their timestamps while nothing is backlogged; otherwise they wait in order behind the backlog and are fed so that at most 2048 bytes are in flight to the machine. Backlog cap 1 MiB; a message that does not fit is dropped whole and counted. Panic clears the backlog. | Removes the 4 KB / 12 KB truncation (app.md) | app.md |

## Review Focus

1. **A partly formatted disk saved** (New Blank Disk, then Save before or
   during a format). Clear message, disk stays modified, no file written, an
   existing file untouched. Test: Task 3 (`DiskSession` save of a blank disk).
2. **Odd WAV files** (WAVE_FORMAT_EXTENSIBLE, chunks before `fmt `/`data`,
   odd-sized chunk padding, a truncated `data` chunk, 0 frames, more than 2
   channels, 32-bit float above 1.0 or NaN). Each decodes correctly or is
   refused with a clear message; nothing crashes. Test: Task 4.
3. **Import selections that don't fit or make no sense** (end ≤ start, start
   past the end of the file, a start page near the end of the half, a silent
   selection with normalise on). Truncated with the dialog saying so, or
   refused; never a `$00` byte, never a write past the half. Test: Task 4.
4. **A `.edm` that isn't a disk** (wrong size raw, a truncated Giebler file, a
   bitmap that claims more sectors than the file holds). Clear message, the
   drive and machine unchanged. Test: Task 8.
5. **A long SysEx dump while playing** (a 200 KB dump with notes during it,
   Panic in the middle). Nothing truncated or reordered, notes after the dump
   still play, Panic empties the backlog. Test: Task 9.

---

## File Structure

```
emulator/
  engine/include/mirage/
    disk_image.h      + setTrack/unformatTrack, formattedTracks, DiskFormat, Giebler EDM codec
    wd1772.h          + Write Track state, $D4 every index, MO with h=1
    doc5503.h         + partner-start rule (comment)
    machine.h         + writeWaveRam, kSnapshotVersion 3
    wav_import.h      new: WAV decode, import plan, 8-bit conversion
  engine/src/         disk_image.cpp, edm_format.cpp (new), wd1772.cpp, doc5503.cpp,
                      machine.cpp, wav_import.cpp (new)
  host/include/mirage/
    realtime_host.h   + postWaveWrite/takeReturnedWave, midiBytesTaken/midiQueueDepth in HostStatus
    disk_session.h    + newBlank, format-aware save, partly-formatted refusal
    midi_in_feeder.h  new: paced MIDI input
  host/src/           realtime_host.cpp, disk_session.cpp, midi_in_feeder.cpp (new)
  tools/mirage_cli/   run_options.*, main.cpp: --import, --save-disk
  app/src/            MainWindow.*, AudioEngine.*, ImportDialog.{h,cpp} (new)
  app/CMakeLists.txt  + panel-png smoke test
  tests/              test_wd1772.cpp, test_disk_image.cpp, test_edm_format.cpp (new),
                      test_wav_import.cpp (new), test_doc5503.cpp, test_realtime_host.cpp,
                      test_disk_session.cpp, test_midi_in_feeder.cpp (new),
                      test_acceptance.cpp (new), booted_machine.h, cli tests in CMakeLists.txt
  docs/               app.md (checklists 2-6), hardware/fdc.md, hardware/boot-rom.md,
                      plans/2026-10-18-plan-1-followups.md, README.md
```

---

### Task 1: Acceptance 2 end to end — edit, OS save, Save As, reopen

No new feature: Plan 2 already ships Save/Save As. This task proves the whole
path headlessly and records the panel key sequence the owner will use.

**Files:**
- Modify: `tests/booted_machine.h` (boot any asset path), `tools/mirage_cli/run_options.{h,cpp}`, `tools/mirage_cli/main.cpp`, top-level `CMakeLists.txt`
- Create: `tests/test_acceptance.cpp` (add to `tests/CMakeLists.txt`)
- Modify: `docs/app.md`

**Interfaces:**
- Produces (`booted_machine.h`): `const Booted& bootedSnapshotAt(const std::string& assetRel);` and `std::unique_ptr<Machine> bootedMachineAt(const std::string& assetRel);` (paths relative to `MIRAGE_ASSET_ROOT`). The existing `bootedSnapshot(image)` becomes `bootedSnapshotAt("Factory Library/IMG/" + image)`. Same settle rule.
- Produces (`booted_machine.h`): `void pressKeys(Machine&, std::initializer_list<PanelKey>, double holdS = 0.15, double gapS = 0.25);` runs the machine through each press and release.
- Produces (`booted_machine.h`, OS 3.2 panel procedures from the manual, used again by Tasks 3, 5 and 8): `DisplayState showParameter(Machine&, int number);` (Param, the digits, Value; returns the display), `void osSaveLowerSound(Machine&);` and `void osLoadLowerSound(Machine&, int sound);` (each runs until the drive has been idle for 2 s, capped at 60 s, and `REQUIRE`s it got there).
- Produces (CLI): `--save-disk <file>` writes the inserted disk (`DiskImage::saveFile`) at the end of the run; exit 1 with `cannot write <file>` on failure.

- [ ] **Step 1: Find the key sequence.** From `../ensoniq_mirage_dsk-1_dsk-8_musicians_manual.pdf` and `../Manuals/ensoniq_mirage_dsk-8_os_update_v.3.0_manual.pdf` (pdf skill), find how OS 3.2 (a) selects a parameter and changes its value, and (b) saves the lower sound to the disk in the drive. Pick a parameter whose value shows on the display and that a save stores with the sound (for example a filter cutoff). Try the sequence with `mirage_cli run --disk SND-A01 --key ...` and read the display at each stage.
- [ ] **Step 2: Write the failing test** `acceptance 2: an edited parameter survives OS save, Save As and reopen` (`[assets][acceptance]`): `bootedMachineAt("Factory Library/IMG/SND-A01.img")`; read the parameter's value off the display (Param, digits); change it with Up presses; save the lower sound with the OS's key sequence; wait until `!driveActive()` for 2 s; `REQUIRE(disk()->dirty())`; `saveFile` to a temp path; load that file into a fresh `Machine`, power on, boot to settled; read the parameter the same way: `REQUIRE(display == edited value)` and `REQUIRE(edited != original)`.
- [ ] **Step 3: Run it.** `ctest --test-dir build -C Debug -R "acceptance 2"`. If it fails, find out why (systematic-debugging) before changing engine code: a failure here is an emulation bug (FDC write path, timing), never something to patch in the OS.
- [ ] **Step 4: Add `--save-disk`** with tests `cli_run_save_disk_unwritable` (exit 1, `^cannot write .*x.img/$`) and a real run that saves SND-A01 after 1 s and checks the file is 450,560 bytes (a small `cmake -P` check like `cli_snapshot_roundtrip.cmake`).
- [ ] **Step 5: Write the acceptance-2 checklist** in `docs/app.md` after acceptance 1's, in the same style: open SND-A01, the exact panel keys for the edit and the save, *File → Save As* to a new file, *File → Open* it, the keys that show the value. Status: "not yet run by the owner".
- [ ] **Step 6: All tests green; commit** `test: acceptance 2 end to end (edit, OS save, Save As, reopen)`.

### Task 2: WD1772 Write Track (format)

**Files:**
- Modify: `engine/include/mirage/disk_image.h`, `engine/src/disk_image.cpp`, `engine/include/mirage/wd1772.h`, `engine/src/wd1772.cpp`, `engine/include/mirage/machine.h` (snapshot version 3)
- Test: `tests/test_wd1772.cpp`, `tests/test_disk_image.cpp`, `tests/test_snapshot.cpp`
- Docs: `docs/hardware/fdc.md`

**Interfaces:**
- Produces (`DiskImage`):
  - `void setTrack(int track, std::span<const uint8_t, kTrackBytes> data);` formats the track with that content (sector order as in the raw image), sets dirty, counts one write. Never allocates (the `optional<Track>` is inline).
  - `void unformatTrack(int track);` sets dirty, counts one write.
  - `int formattedTracks() const;` 0-80.
- Produces (`Wd1772`): Write Track (`$F0-$FF` with the `$F4` style e/p bits honoured as in Type II) waits for the index pulse, then raises DRQ for each of the 6250 byte times to the next index pulse, interpreting `$F5` (writes `$A1`, presets CRC), `$F6` (writes `$C2`), `$F7` (writes two CRC bytes). Lost data writes `$00` and sets S2. At the next index pulse the stream is parsed (decision table above) and `setTrack`/`unformatTrack` is called on the head's track, then INTRQ. No disk: waits, as Type II. Eject mid-command: aborts without changing the track.
- Produces (`Machine`): `kSnapshotVersion = 3`; the WD1772 section saves the in-flight Write Track buffer and position.

- [ ] **Step 1: Write the failing tests** in `test_wd1772.cpp`, using the existing tick harness and `DiskImage::blank()`:
  - `wd1772 write track in the Mirage layout formats the track`: write a track stream built by a test helper `mirageTrackStream(track, fill)` (gap bytes `$4E`, then per id 0-5 `00×12, F5 F5 F5 FE t 00 id size F7, 4E×22, 00×12, F5 F5 F5 FB, fill×size, F7, 4E×N`); after INTRQ `trackFormatted(t)` and every sector reads back `fill`; status has no error bits.
  - `wd1772 write track with a foreign layout leaves the track unformatted` (ids 1-9, 512 bytes each), and on a formatted track it becomes unformatted.
  - `wd1772 write track that misses DRQs writes zeros and sets lost data`.
  - `wd1772 a formatted track reads with Read Sector` (end to end through the controller).
  - `wd1772 eject during write track leaves the track as it was`.
- [ ] **Step 2: Run them; expect FAIL** (Write Track completes at once today).
- [ ] **Step 3: Implement** `setTrack`/`unformatTrack`/`formattedTracks` (with `test_disk_image.cpp` tests for dirty and write count), then Write Track in `wd1772.cpp` as new phases (`TrackWaitIndex`, `TrackStream`). Keep a 6250-byte stream buffer in the controller.
- [ ] **Step 4: Snapshot.** Bump `kSnapshotVersion` to 3, save/load the new state; add `snapshot round-trips a machine mid write track` to `test_snapshot.cpp`. Update tests that pin version 2.
- [ ] **Step 5: Docs.** `fdc.md`: replace "Read Track / Write Track complete with INTRQ, no data" with the Write Track model and the parse rule; Read Track stays unmodelled (say so).
- [ ] **Step 6: All tests green; commit** `feat(engine): WD1772 Write Track formats tracks in the Mirage layout`.

### Task 3: New Blank Disk and acceptance 5

**Files:**
- Modify: `host/include/mirage/disk_session.h`, `host/src/disk_session.cpp`, `app/src/MainWindow.{h,cpp}`, `app/CMakeLists.txt`, `tests/test_disk_session.cpp`, `tests/test_acceptance.cpp`, `docs/app.md`

**Interfaces:**
- Consumes: Task 2's Write Track and `formattedTracks()`.
- Produces (`DiskSession`):
  - `bool newBlank();` posts `DiskImage::blank()` as an insert with an empty path (title "Untitled (blank)"); false if the queue is full or a save is in flight. The caller prompts first with `needsSaveBeforeReplacing()`, as for Open.
  - A save whose copy has unformatted tracks fails with exactly `This disk is not fully formatted (N of 80 tracks). Format it with the OS before saving.`; the disk stays modified and no file (not even `.tmp`) is left.
- Produces (app): *File → New Blank Disk* (Ctrl+N) between Open and Save; prompts like Open.
- Produces (CMake): when `MIRAGE_BUILD_APP` is on, a ctest `app_panel_png_smoke` runs `$<TARGET_FILE:MirageApp> --panel-png <build>/panel_smoke.png` and passes on exit 0 with a file whose first 8 bytes are the PNG signature (a `cmake -P` script).

- [ ] **Step 1: Write the failing tests**:
  - `disk session new blank inserts an unformatted disk with no path` (title, `path().empty()`, the host gets an insert).
  - `disk session refuses to save a partly formatted disk` (exact message above, still modified, target folder unchanged).
  - `acceptance 5: fmt2 formats a blank disk that then boots` (`[assets][acceptance]`): `bootedMachineAt("EnsoniqOS/fmt2")` (display "Fd": `71 5E`); eject, run 0.5 s, insert `DiskImage::blank()`; press K1, Enter; run until the display reads "FC" (`71 39`) or 180 s; `REQUIRE(formattedTracks() == 80)`; `toBytes()`, put the bytes in a fresh `Machine`, boot it to settled: the display reads `DB 06` (fmt2 writes OS 3.2 onto the disk, `About the disk Images.rtf`).
  - `acceptance 5: a sound saved to the formatted disk loads back` (`[assets][acceptance]`): boot SND-A01, swap in the formatted disk from the previous test (share it through a function-local static), save the lower sound with Task 1's key sequence, `saveFile` to temp, reopen into a fresh machine booted from SND-A01, swap the saved disk in, load the lower sound from it (Load Lower, digit, Enter) and check a note renders with peak > 0.05.
- [ ] **Step 2: Run them; expect FAIL** where the feature is missing. If fmt2 does not reach "FC", debug the controller (Read Address, verify after write, Type I with V) before anything else; record the commands fmt2 issues in `fdc.md`.
- [ ] **Step 3: Implement `newBlank` and the save refusal**, then the menu item and the panel-png smoke test.
- [ ] **Step 4: Measure** how long fmt2 takes to format on the emulated clock and put it in the checklist.
- [ ] **Step 5: Write the acceptance-5 checklist** in `docs/app.md` (open fmt2, wait for "Fd", *New Blank Disk*, 1, Enter, wait for "FC" (time), Save refused before formatting is done, then boot the new disk or save a sound to it, *Save As*, reopen, load). Add New Blank Disk to the menu table.
- [ ] **Step 6: All tests green (including `app_panel_png_smoke`); commit** `feat: New Blank Disk; formatting with fmt2 (acceptance 5); panel-png smoke test`.

### Task 4: WAV converter (engine, JUCE-free)

**Files:**
- Create: `engine/include/mirage/wav_import.h`, `engine/src/wav_import.cpp`, `tests/test_wav_import.cpp`
- Modify: `engine/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces (`wav_import.h`, namespace `mirage`):
```cpp
struct WavError : std::runtime_error { using std::runtime_error::runtime_error; };
struct WavAudio { double rate = 0; int channels = 0; int bits = 0; bool isFloat = false;
                  std::vector<float> mono; };  // mixed down, -1..1 nominal
WavAudio readWav(std::span<const uint8_t> file);           // throws WavError
WavAudio readWavFile(const std::filesystem::path& file);   // throws WavError
enum class WaveHalf : uint8_t { Lower, Upper };            // $00000 / $10000
struct ImportSettings { double rate = kDefaultImportRate; double startSeconds = 0, endSeconds = 0;
                        WaveHalf half = WaveHalf::Lower; int startPage = 0; bool normalize = true; };
struct ImportPlan { uint32_t address = 0; size_t bytes = 0; int lastPage = 0; double seconds = 0;
                    double fractionOfHalf = 0; bool truncated = false; std::string problem; };
ImportPlan planImport(const WavAudio&, const ImportSettings&);       // problem non-empty: refuse
std::vector<uint8_t> convertForWaveRam(const WavAudio&, const ImportSettings&);  // size == plan.bytes
inline constexpr double kMinImportRate = ..., kMaxImportRate = ..., kDefaultImportRate = ...;
```
- `problem` texts (exact): `Choose an end point after the start point.`, `The start point is past the end of the file.`, `The sample rate must be between <min> and <max> Hz.`, `The start page must be between 0 and 255.`
- Accepted: PCM 8 (unsigned), 16, 24, 32-bit, 32-bit float, `WAVE_FORMAT_EXTENSIBLE` with a PCM or float subformat, 1 or 2 channels, any rate. Refused (exact): `Not a WAV file.`, `Unsupported WAV format: only PCM (8, 16, 24 or 32-bit) and 32-bit float are supported.`, `Unsupported WAV file: only mono and stereo are supported.`, `The WAV file has no audio.`, `The WAV file is damaged (<what>).`

- [ ] **Step 1: Confirm the rate range.** From `../Manuals/ensoniq_mirage_advanced_sampler's_guide.pdf` find the OS 3.2 sampling-rate range and default (spec: roughly 10-33 kHz). Set the three constants; cite the page in a comment.
- [ ] **Step 2: Write the failing tests** (WAVs built in memory by a test helper):
  - decoding each accepted format gives the same mono samples (a 16-bit sine vs its 24-bit, 32-bit, float and 8-bit versions within one LSB of the coarser format); stereo averages; `LIST` before `fmt `, odd-sized chunks padded, `data` declared longer than the file → damaged message; each refusal's exact text.
  - `planImport`: 1.0 s at 20 kHz from page 0 → 20,000 bytes, `lastPage` 78, not truncated; 4 s at 30 kHz → truncated at 65,536 bytes, `lastPage` 255; page 250 + 2 s → truncated at 1,536 bytes; Upper half → address `$10000 + page*256`; each problem text.
  - `convertForWaveRam`: never contains `$00`; normalise puts the peak at `$01` or `$FF`; a silent selection with normalise gives all `$80`, no NaN; a 1 kHz sine at 44.1 kHz converted to 20 kHz has its zero crossings 20 bytes apart (±1); a 15 kHz tone converted to 20 kHz comes out below -30 dB (the low-pass works).
- [ ] **Step 3: Run them; expect FAIL** (no such file).
- [ ] **Step 4: Implement** `wav_import.cpp` per the decision table (windowed-sinc, Kaiser or Blackman, at least 32 taps per side).
- [ ] **Step 5: All tests green; commit** `feat(engine): WAV to Mirage 8-bit converter`.

### Task 5: Wave RAM writes in the engine and the CLI

**Files:**
- Modify: `engine/include/mirage/machine.h`, `engine/src/machine.cpp`, `tools/mirage_cli/run_options.{h,cpp}`, `tools/mirage_cli/main.cpp`, top-level `CMakeLists.txt`, `tests/test_machine_map.cpp`, `tests/test_cli_run.cpp`, `tests/test_acceptance.cpp`, `README.md`
- Docs: `docs/app.md` (pitch hint, half naming)

**Interfaces:**
- Consumes: Task 4's `readWavFile`, `planImport`, `convertForWaveRam`.
- Produces (`Machine`): `void writeWaveRam(uint32_t address, std::span<const uint8_t> bytes);` copies into sample RAM; throws `std::out_of_range` if `address + size > kSampleRamSize`; never allocates; for the thread that runs the machine, between `runCycles`/`render` calls.
- Produces (CLI): `--import "FILE@T[,rate=R][,start=S][,end=E][,half=lower|upper][,page=P][,normalize=0|1]"` (repeatable; defaults: the default rate, start 0, end = file end, lower, page 0, normalize 1). At T: if `driveActive()`, print `import: FILE refused, the drive is busy` and go on; else write and print `import: FILE N bytes at $XXXXX, last page P[, truncated]`. A bad file or a `problem` exits 1 with the message before the run.
- Produces (docs): the pitch hint formula `key ≈ k0 + 12·log2(rate / r0)` with the measured `k0`, `r0`.

- [ ] **Step 1: Write the failing tests**: `machine writeWaveRam writes sample RAM and nothing else` (sampleRam() changes only in range; RAM `$8000-$BFFF` untouched via `peek`); out-of-range throws; `writeWaveRam` under `AllocationScope` allocates nothing. CLI parse tests for `--import` (each field, a bad field → `bad value for --import: ...`).
- [ ] **Step 2: Measure which half the OS 3.2 lower sound uses**: after `bootedSnapshotAt` of SND-A01 (lower sound loaded, upper not), count non-`$00` bytes in each half. Name the halves in the dialog/CLI docs accordingly.
- [ ] **Step 3: Implement** `writeWaveRam` and `--import`.
- [ ] **Step 4: Write `acceptance 3: an imported sine plays at a pitch set by the key`** (`[assets][acceptance]`): boot SND-A01, import a generated 1 s 440 Hz sine WAV (written to temp) into the lower half page 0 at the default rate, play two notes an octave apart; the second's fundamental (zero-crossing count) is 2× the first's (±3%), and peak > 0.05. Use it to **measure the pitch hint**: for the key that plays wave RAM at the import rate, record `k0`/`r0` for the dialog (spec §6.7 "plays at original pitch around C4"). If the loaded sound's wavesample does not start at page 0, set its start/end from the panel in the test with the OS's keys, and put those keys in the checklist.
- [ ] **Step 5: All tests green; README: an `--import` example; commit** `feat: wave RAM writes and mirage_cli --import`.

### Task 6: Wave writes through RealtimeHost, and the Import WAV dialog

**Files:**
- Modify: `host/include/mirage/realtime_host.h`, `host/src/realtime_host.cpp`, `tests/test_realtime_host.cpp`
- Create: `app/src/ImportDialog.{h,cpp}`; modify `app/src/MainWindow.{h,cpp}`, `app/CMakeLists.txt`
- Docs: `docs/app.md`

**Interfaces:**
- Consumes: Task 4 (`WavAudio`, `ImportSettings`, `planImport`, `convertForWaveRam`), Task 5 (`Machine::writeWaveRam`).
- Produces (`realtime_host.h`):
```cpp
struct WaveWrite { uint32_t address = 0; std::vector<uint8_t> bytes; };
enum class WaveReturn : uint8_t { Written, RefusedDriveBusy };
struct ReturnedWave { std::unique_ptr<WaveWrite> write; WaveReturn why; };
bool postWaveWrite(std::unique_ptr<WaveWrite>&& w);     // UI thread; false: queue full or null
[[nodiscard]] std::optional<ReturnedWave> takeReturnedWave();  // UI thread, frees there
```
  Applied by `process()` with the UI queue, before rendering; refused (not written) when `driveActive()`.
- Produces (app): *File → Import WAV…* (Ctrl+I), greyed while the drive LED is on or no audio device runs. A file chooser (`*.wav`), then a modal `ImportDialog`: waveform with draggable start/end markers, start/end in seconds, rate slider (`kMinImportRate`-`kMaxImportRate`, default `kDefaultImportRate`), half (named per Task 5), start page 0-255, Normalise checkbox (on), and live text from `planImport`: selected length in seconds, `% of the half`, last page, the truncation warning, the pitch hint, any `problem` (Import disabled while there is one). *Import* converts on the UI thread, posts the write, and reports "Imported N bytes at $XXXXX (pages A-B)" or "Not imported: the drive was busy" in the status strip when the write comes back.

- [ ] **Step 1: Write the failing tests**: `host applies a wave write before the block and returns it Written`; `host refuses a wave write while the drive is active and returns it untouched` (boot a disk headlessly until `driveActive()`); `wave write under AllocationScope allocates and frees nothing on the audio thread` (extend the existing allocation test); the destructor frees queued writes.
- [ ] **Step 2: Run them; expect FAIL.**
- [ ] **Step 3: Implement** the host side (a dedicated SPSC queue of `unique_ptr<WaveWrite>` capacity 16 each way, or the UI queue's command variant — follow how disks travel).
- [ ] **Step 4: Implement the dialog and menu item.** Keyboard shortcuts must not reach the panel while it is open (Plan 2's dialog rule). Build Release, open it on a real WAV, take a screenshot of the dialog for the review (scratch folder, not committed).
- [ ] **Step 5: Write the acceptance-3 checklist** in `docs/app.md` (open SND-A01, wait for the settled display, *Import WAV…*, the settings to use, the panel keys from Task 5 for wavesample start/end if needed, play across the lower keyboard, the OS's save, *Save*, reopen, play). Add the menu item and dialog to the guide; remove "no WAV import yet" from Known limitations.
- [ ] **Step 6: All tests green; commit** `feat: Import WAV dialog writes wave RAM through the host`.

### Task 7: Other OSes (acceptance 4) and the controller/DOC sweep

Recon on 2026-10-21 (Release CLI, 30 s, note 60 at 20 s): `os32`, `masos`,
`pianos`, `rhodes`, `resmos32.s` settle at `DB 06` and play; `fmt2` shows
"Fd" and waits; the Soundprocess OS disk settles at "01" (`3F 06`, 142
sectors) and **the note is silent**.

**Files:**
- Modify: `engine/src/wd1772.cpp`, `engine/include/mirage/wd1772.h`, `engine/src/doc5503.cpp`, `engine/include/mirage/doc5503.h`, `tests/test_wd1772.cpp`, `tests/test_doc5503.cpp`, `tests/test_acceptance.cpp`
- Docs: `docs/hardware/fdc.md`, `docs/hardware/boot-rom.md` (other-OS table), `docs/plans/2026-10-18-plan-1-followups.md` (mark done), `docs/app.md`

**Interfaces:**
- Produces: `$D4` raises INTRQ at **every** index pulse until the next Force Interrupt or command (WD1772 I2); MO (status bit 7) goes on at every command start, h=1 only skips the 6-pulse wait (check `Manuals/wd1772.txt`; if the text disagrees, follow it and say so in `fdc.md`). DOC: when an even oscillator halts for any reason and its odd partner is in swap mode, the partner starts (H = 0, A = 0), MAME's rule; check sync/AM end-of-table behaviour against the datasheet and buchty's page and record the verdict in the `doc5503.h` comment.

- [ ] **Step 1: Find why Soundprocess is silent** (systematic-debugging). Read `../Manuals/SoundProcess OS Manual.PDF` for what the OS does after boot (MIDI channel/omni default, whether the OS disk carries a sound, what "01" means). Then trace (`--trace`) what happens when the note arrives (ACIA reads, DOC writes). A faithful outcome is fine (for example the OS disk holds no sound and you load one from a sound pack, or it listens on another channel); an emulation bug gets fixed and a unit test.
- [ ] **Step 2: Write the failing tests**: `wd1772 $D4 interrupts on every index pulse`, `wd1772 MO is set by a command with h=1`, `doc an even oscillator halting starts its odd swap partner`, plus whatever Step 1 found.
- [ ] **Step 3: Implement**; run the full suite (boot tests must not move; if a boot time changes, re-measure and explain it).
- [ ] **Step 4: Write `acceptance 4: each OS image boots and plays`** (`[assets][acceptance]`), one `SECTION` per image: `EnsoniqOS/os32`, `masos`, `pianos`, `rhodes`, `resmos32.s`, `Soundprocess/Soundprocess OS/Soundprocess OS.img` (with the Step 1 procedure, for example a sound pack swapped in and loaded): boot to settled with `bootedSnapshotAt`, note 60 vel 100 for 1 s, peak > 0.05, `illegalOps() == 0`.
- [ ] **Step 5: Docs.** boot-rom.md other-OS table (settle time, display, sectors, what plays); followups "Plan 3" items marked done with commits; the acceptance-4 checklist in `docs/app.md` (each image, what the display settles on, how to get sound from Soundprocess).
- [ ] **Step 6: All tests green; commit** `feat: other OSes boot and play; WD1772 $D4 and MO, DOC swap partner (acceptance 4)`.

### Task 8: EDM (acceptance 6)

**Files:**
- Create: `engine/src/edm_format.cpp`, `tests/test_edm_format.cpp`
- Modify: `engine/include/mirage/disk_image.h`, `engine/src/disk_image.cpp`, `host/include/mirage/disk_session.h`, `host/src/disk_session.cpp`, `app/src/MainWindow.cpp`, `tests/test_disk_session.cpp`, `tests/test_acceptance.cpp`
- Docs: `docs/hardware/fdc.md` ("Disk file formats"), `docs/app.md`

**Interfaces:**
- Produces (`disk_image.h`):
```cpp
enum class DiskFormat : uint8_t { Raw, GieblerEdm };
DiskFormat detectDiskFormat(std::span<const uint8_t> file);   // throws DiskImageError
static DiskImage fromFileBytes(std::span<const uint8_t> file, DiskFormat* found = nullptr);
static DiskImage loadFile(const std::filesystem::path&, DiskFormat* found = nullptr);  // either format
std::vector<uint8_t> toEdmBytes() const;                       // throws if any track unformatted
void saveFile(const std::filesystem::path&, DiskFormat format) const;  // atomic, as today
```
  `saveFile(path)` keeps meaning Raw. Error for anything else (exact): `<name>: not a Mirage disk image (expected 450,560 bytes or a Giebler .edm file)`.
- Produces (`DiskSession`): remembers the format `open()` found; `startSave()` writes it; `startSave(file)` for Save As writes `GieblerEdm` when `file` ends in `.edm` (any case), else Raw. `saveRefusal()` returns empty for `.edm` now (only the partly-formatted case refuses, at save time).
- Produces (app): Open dialog filter `*.img;*.edm;*`; Save As offers `.img` by default and keeps `.edm` when the disk came from one.

- [ ] **Step 1: Decode the format exactly** with the pairs in `../Soundprocess/TritonSoundprocess/` (`SP1.EDM`-`SP6.EDM` ↔ `SoundProcess_Vol1.edm`-`Vol6.edm`, `SNDPROCS.EDM` ↔ `Triton - Soundprocess OS.edm`; the `.edm` ones are raw). Known so far: 512-byte header (`0D 0A "Mirage Disk"`, spaces, `0D 0A`, spaces, `0D 0A 1A` up to `$9F`), a bitmap from `$A0`, one bit per sector MSB first in per-track order 5,0,1,2,3,4, **1 = not stored**; stored sectors follow from `$200` in that order. Still open: why each file is `512·k + 1` bytes longer than the bitmap predicts (sizes 389,633 / 400,897 / 457,217), what the last byte is, and how a not-stored sector reads (zeros?). Also check `../Tools/Ensoniq EFE File Format (Steve Quartly & Gary Giebler).txt` and `../Tools/Ensoniq Floppy Diskette Format (Gary Gebler).pdf`. If `M_10.EDM`-`M_12.EDM` (`../Tools/Ensoniq EDM2IMG*/[EDM_IN]/`) still don't decode, run the owner's `flopimage_convert.exe <in-dir> <out-dir> -IMG` (spec §6.6) with input and output folders in the scratch directory, and compare. Write the findings in `fdc.md`.
- [ ] **Step 2: Write the failing tests** (`[assets]`, skipping without the files): each Triton pair decodes to the raw image byte for byte; `toEdmBytes()` of each decoded image decodes back to the same image, and equals the original `.EDM` file byte for byte if Step 1 found the writer's rule (otherwise record in `fdc.md` why not); damaged Giebler files (cut at `$100`, cut mid-sector, bitmap with all sectors stored but file short) throw with a message and never read out of bounds; a 450,560-byte `.edm` loads as Raw; the wrong-size message is exact. `DiskSession`: Save after opening a Giebler file writes Giebler; Save As `x.edm` writes Giebler, `x.img` raw; a raw `.edm` saved stays raw.
- [ ] **Step 3: Run them; expect FAIL.**
- [ ] **Step 4: Implement** `edm_format.cpp`, the `DiskImage` and `DiskSession` changes and the dialog filter; remove the old `.edm` refusal and its tests.
- [ ] **Step 5: Write `acceptance 6: a Metascape .edm boots and plays`** (`MetascapeMetaSounds/mp1.edm`, the acceptance-1 conditions: settles at `DB 06`, a note peaks > 0.05) and `... a Giebler .EDM boots its OS` (`SNDPROCS.EDM` settles like the Soundprocess OS disk).
- [ ] **Step 6: Acceptance-6 checklist** in `docs/app.md` (acceptance 1 with `mp1.edm`; then a Giebler `SP1.EDM` with the Task 7 Soundprocess procedure; Save As `.edm` and reopen). Update the Open/Save rows of the menu table.
- [ ] **Step 7: All tests green; commit** `feat: .edm disks, raw and Giebler, open and save (acceptance 6)`.

### Task 9: Paced SysEx feeder

**Files:**
- Create: `host/include/mirage/midi_in_feeder.h`, `host/src/midi_in_feeder.cpp`, `tests/test_midi_in_feeder.cpp`
- Modify: `host/include/mirage/realtime_host.h`, `host/src/realtime_host.cpp`, `host/CMakeLists.txt`, `app/src/AudioEngine.{h,cpp}`, `app/src/MainWindow.cpp` (panic), `tests/CMakeLists.txt`, `docs/app.md`

**Interfaces:**
- Produces (`HostStatus`): `uint64_t midiBytesTaken` (cumulative bytes `process()` has taken from the MIDI queue: queued into the Machine, or discarded while the rate is invalid; not a panic flush, which the feeder clears itself) and `size_t midiQueueDepth` (`Machine::midiQueueDepth()` at the end of the block).
- Produces (`midi_in_feeder.h`):
```cpp
class MidiInFeeder {
 public:
  static constexpr size_t kMaxInFlight = 2048, kMaxBacklog = 1 << 20;
  explicit MidiInFeeder(RealtimeHost& host);
  void push(std::span<const uint8_t> msg, double timeSeconds);  // MIDI-input thread
  void pump();     // any non-audio thread, >= 200 Hz (the app's MIDI-out thread)
  void clear();    // UI thread, with postPanic: drops the backlog
  size_t backlog() const;
  uint64_t dropped() const;  // whole messages over kMaxBacklog
};
```
  The only caller of `postMidi`. One mutex serialises `push`/`pump`/`clear`, so `postMidi` keeps a single producer at a time. With an empty backlog a message that fits a single `postMidi` goes straight through with its timestamp; anything else joins the backlog, and `pump` posts backlog bytes (timestamp = now, chunks ≤ `kMidiQueueCommands × kMidiCommandBytes`) while `posted − status().midiBytesTaken + status().midiQueueDepth < kMaxInFlight`.
- Consumes: app calls `push` from `handleIncomingMidiMessage`, `pump` in the MIDI-out thread loop, `clear` before `postPanic`. `MIDI dropped` in the strip adds `dropped()` bytes.

- [ ] **Step 1: Write the failing tests** (drive a `RealtimeHost` with `process()` at 48 kHz, 256 frames, from the test thread; `pump()` between blocks): `feeder delivers a 200 KB SysEx whole and in order` (capture via a test hook or the ACIA RX path: count `midiOverflows() == 0` and the bytes reaching the machine equal the input); `feeder keeps a note after a long SysEx behind it`; `feeder never has more than kMaxInFlight bytes in flight`; `feeder clear drops the backlog`; `feeder drops a message bigger than kMaxBacklog and counts it`; short messages with an empty backlog keep their timestamps.
- [ ] **Step 2: Run; expect FAIL. Step 3: implement** the status fields and the feeder; wire the app.
- [ ] **Step 4: Docs.** `docs/app.md`: threading table (the feeder), MIDI-in queue notes, remove the SysEx limitation from Known limitations.
- [ ] **Step 5: All tests green; commit** `feat(host): paced MIDI input feeder; long SysEx is no longer truncated`.

### Task 10: Docs and the owner's listening pass

**Files:** `docs/app.md`, `README.md`, `docs/plans/2026-10-18-plan-1-followups.md`, `docs/specs/2026-10-18-mirage-emulator-design.md` (status line only, if acceptances pass)

- [ ] **Step 1: Read `docs/app.md` start to end** against the built Release app: every menu item, shortcut, dialog and checklist step exists and is worded as the app shows it. Known limitations reflect what is left (Read Track unmodelled, OS sampling input silent, …).
- [ ] **Step 2: Run the full suite in Debug and the acceptance tests in Release** (`ctest --test-dir build -C Release -R acceptance`); record the times.
- [ ] **Step 3: Commit** `docs: Plan 3 guide and acceptance 2-6 checklists`.
- [ ] **Step 4: Hand the checklists to the owner.** Each acceptance is passed only when the owner says so; record "passed by ear (owner, date)" per acceptance as they report. Merge to `main` only on the owner's word.
