# Mirage Emulator — Plan 2: Standalone App Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A standalone Windows JUCE app around the Plan 1 engine: a replica
DSK-8 front panel, live MIDI in/out, audio out, and *File* open/save/eject
dialogs with shortcuts. It reaches spec acceptance 1: open `SND-A01.img`,
boot, load a sound and play it from a MIDI keyboard.

**Architecture:** Three layers. `mirage_engine` (Plan 1) gets the real-time
and disk-swap work the Plan 1 reviews asked for. A new JUCE-free library,
`mirage_host`, owns the threading model: `RealtimeHost` keeps the `Machine`
on the audio thread and takes commands through lock-free SPSC queues, one per
producer thread. It publishes display, LED, disk and diagnostic state through
atomics, and hands disks back to the UI thread for freeing or saving. The JUCE
app (`app/`) is a thin shell: window, panel drawing, menus, devices, dialogs.
Everything testable lives in the two libraries and is covered by Catch2.

**Tech Stack:** C++20, CMake ≥ 3.22, MSVC 2019 (v142, x64), Windows SDK
10.0.22621, Catch2 v3.7.1, JUCE 8.0.4. Both are fetched by git through
FetchContent. Use `GIT_SHALLOW TRUE` and `GIT_CONFIG core.longpaths=true`
for JUCE: the URL download fails here on TLS, and a plain clone hits
Windows path limits.

**Spec:** `emulator/docs/specs/2026-10-18-mirage-emulator-design.md`. Plan 2
covers build-order step 6 (§7) plus Save/Save As (§6.5). Also read
`emulator/docs/plans/2026-10-18-plan-1-followups.md`, which lists the
required engine tasks folded in here.

## Global Constraints

- **Faithful emulation only.** No OS-specific code paths, no OS patching,
  no writing OS RAM or parameters (spec §3). The host may press panel keys,
  queue MIDI bytes and insert or eject disks, and nothing else.
- **ROM and disk images are user-owned.** They are never copied into
  `emulator/` or committed. Tests find them through `MIRAGE_ASSET_ROOT` and
  `SKIP` when they are missing (unchanged from Plan 1).
- **Boot ROM SHA-1:** `ec6ea5613eeafd21d8f3a7431a35a6ff16eed56d`. A
  different ROM is allowed but warned about.
- **Disk images:** `.img`, exactly 450,560 bytes, any extension. Never
  `.hfe`. EDM is Plan 3.
- **The engine and `mirage_host` stay free of JUCE.** Only `app/` includes
  JUCE.
- **Real time:** nothing on the audio thread allocates, frees, locks or
  does I/O after `prepare()`. This is enforced by an allocation-counting
  test.
- **Realistic timing:** no turbo or fast-forward during loads (owner,
  2026-10-18).
- **Branch:** `plan-2-standalone-app` in `emulator/`, no worktree (the
  default asset root `..` breaks inside a worktree). LF line endings
  (`.gitattributes`).
- **Build:** `cmake -S emulator -B emulator/build -G "Visual Studio 16 2019" -A x64`,
  then `cmake --build emulator/build --config Debug`, then
  `ctest --test-dir emulator/build -C Debug --output-on-failure`.
  `-DMIRAGE_BUILD_APP=OFF` skips the app (and the JUCE fetch). From Git
  Bash, use `--parallel`, not `-- /m` (Bash turns `/m` into a path). A spike
  confirmed that JUCE 8.0.4 configures, builds and runs with this toolchain
  (2026-10-18).

### Decisions made for this plan (record each in the doc named)

| Decision | Choice | Why / cost if wrong |
|---|---|---|
| Output headroom | Keep `kOutputGain` 0.5 and add a soft clip in `render()`: linear up to ±0.75, then `0.75 + 0.25·tanh((|x|−0.75)/0.25)`, never past ±1.0 | One voice peaks at about 0.26, so normal play is untouched. Chords can't hard-clip the device. CLI WAVs match the app. Easy to retune by ear. (`machine.h` comment) |
| Boot calibration tones (filter self-oscillation, about 1.3 s after the OS loads) | **Not muted** | This is what the emulated hardware emits. Muting would need a boot-phase detector, which is an OS-specific heuristic. Flag it at the acceptance listen. |
| MIDI input timing | Events are placed in the block by timestamp, MidiMessageCollector-style: `offset = frames − round((now − t)·rate)`, clamped and non-decreasing | Adds up to one block of latency but no jitter. |
| Minimum key hold | A panel key stays down at least 100 ms of emulated time | The OS scans one keypad column per main-loop pass, and a fast mouse click could fall between scans. |
| Disk swap | Eject now; insert 0.5 s of emulated time later | The OS sees PB6 drop (follow-ups). |
| Saving | The audio thread copies the disk into a UI-supplied buffer. The UI writes the file atomically, then sends "mark saved" with the disk's write count. Dirty clears only if no write happened in between. | Concurrent OS writes are never lost from the dirty flag. |
| Atomic `DiskImage::saveFile` | Moved here from Plan 3, because Save UI ships now | — |
| MIDI panic | *MIDI → Panic* flushes the input queue and sends CC 123 (All Notes Off) on all 16 channels | If an OS ignores CC 123, that's faithful behaviour. |
| JUCE licence | JUCE 8 (AGPLv3 or JUCE's free Starter licence). The engine and host stay JUCE-free, so the VST3 and licence choices stay open. | Note it in README. |

## Review Focus

1. **Opening a disk while one is in and has unsaved OS writes.** The user is
   asked Save / Discard / Cancel first. Cancel changes nothing. Test: Task 11
   (`DiskSession` unit tests).
2. **Bad files from the Open dialog** (wrong size, unreadable, a directory,
   a 0-byte file). A clear message; the drive and machine are unchanged.
   Test: Task 11.
3. **No audio device, or the device stops** (unplugged, rate 0). The app
   doesn't crash. Emulation simply doesn't advance (`render` guards the
   rate), and the panel keeps the last state. Test: Task 8 (`process` with
   rate 0 consumes commands safely and writes silence).
4. **A MIDI flood** (clock, active sensing, SysEx dumps, a stuck controller).
   Nothing blocks. Overflowing bytes are dropped and counted, never
   reordered. Test: Task 4 (ring overflow) and Task 8 (host queue full).
5. **A save failing midway** (read-only target, disk full). The old file is
   untouched, the disk stays dirty, and the error is shown. Test: Task 3
   (atomic save) and Task 11 (`DiskSession` keeps dirty on failure).

---

## File Structure

```
emulator/
  CMakeLists.txt                  + add_subdirectory(host), option MIRAGE_BUILD_APP, app/
  README.md                       + app build/run, JUCE licence note
  engine/ (modify)                wd1772.cpp, panel.*, disk_image.*, machine.*, acia6850.*,
                                  stream_resampler.cpp, state_io.h
  host/CMakeLists.txt             mirage_host static library (no JUCE)
  host/include/mirage/
    spsc_queue.h                  SpscQueue<T, N>: lock-free single-producer/consumer FIFO
    midi_byte_parser.h            MidiByteParser: byte stream -> whole MIDI messages
    panel_layout.h                replica panel geometry + labels; shortcut -> PanelKey
    realtime_host.h               RealtimeHost: Machine on the audio thread
    disk_session.h                DiskSession: open/save/eject state machine (UI side)
    boot_rom.h                    kKnownBootRomSha1, isKnownBootRom()
  host/src/*.cpp
  app/CMakeLists.txt              JUCE fetch + MirageApp target
  app/src/Main.cpp                JUCEApplication, --panel-png
  app/src/MainWindow.{h,cpp}      DocumentWindow, menus, commands, key shortcuts
  app/src/AudioEngine.{h,cpp}     device manager, audio callback, MIDI in, MIDI out pump
  app/src/PanelComponent.{h,cpp}  replica panel drawing + buttons + display + LED
  app/src/Settings.{h,cpp}        ApplicationProperties wrapper, boot ROM location
  tests/                          + test_realtime.cpp, test_spsc_queue.cpp,
                                  test_midi_byte_parser.cpp, test_panel_layout.cpp,
                                  test_realtime_host.cpp, test_disk_session.cpp,
                                  alloc_counter.{h,cpp}
  docs/hardware/fdc.md            + cumulative sector layout
  docs/hardware/clocks-and-midi.md + TX pacing
  docs/hardware/boot-rom.md       + new boot timings (13.6)
  docs/app.md                     window, menus, shortcuts, threading, acceptance-1 checklist
```

---

### Task 1: FDC sector layout from cumulative byte offsets

**Files:**
- Modify: `engine/src/wd1772.cpp` (`delayToSlot`, layout constants),
  `docs/hardware/fdc.md` ("Track layout seen by the controller")
- Modify, re-measuring the timings: `tests/test_boot.cpp`,
  `tests/booted_machine.h` (comment), top-level `CMakeLists.txt` CLI
  regexes, `README.md` ("about 35-37 s"), `docs/hardware/boot-rom.md` (new
  §13.6)
- Test: `tests/test_wd1772.cpp`

**Interfaces:**
- Produces: no API change. Slot k's ID field starts at the cumulative byte
  offset below.

The layout, in byte times (one revolution = `kDiskIndexCycles / kFdcByteCycles`
= 6250 bytes):
- `kGap1 = 16`: index to the first ID field.
- `slotBytes(id) = 10 (ID) + 22 (gap 2) + 16 (sync + DAM) + size + 2 (CRC)`,
  which is 1074 for ids 0–4 and 562 for id 5.
- `kGap3 = (6250 − kGap1 − 5·1074 − 562) / 6 = 50` after every sector. The
  remainder (2 bytes) is gap 4, before the index.
- `slotStart(k) = kGap1 + Σ_{j<k} (slotBytes(j) + kGap3)`, in cycles
  `× kFdcByteCycles`.

- [ ] **Step 1: Write the failing test** `wd1772 reads sectors 0-5 of a track in under two revolutions back to back`. Use `makeDisk()` and the existing tick harness. Issue `$88` (Read Sector, h=1 style as the ROM does) for sector 0. Drain DRQs. On each INTRQ, after a 200-cycle command latency (`kOsLatency`, an NMI handler's worth), issue the next sector. Assert all six complete without RNF, and that the cycles from the first command to the last INTRQ are `< 2 * kDiskIndexCycles`. With the old k/6 spacing this takes about 6 revolutions.
- [ ] **Step 2: Run it and confirm it fails.** Run `ctest --test-dir build -C Debug -R "back to back"`. Expected: FAIL (about 2.4M cycles).
- [ ] **Step 3: Replace the k/6 spacing in `delayToSlot` with `slotStart(slot)`**, using `constexpr` helpers and the constants above. Keep `kFirstReadByte` etc. relative to the slot start.
- [ ] **Step 4: Run the test suite.** The new test passes. Boot tests whose times were measured under the old layout may now fail. For each, re-measure (`mirage_cli run --seconds T`, `bootedSnapshot().seconds`) and update the expectation to the new measured value, keeping the condition the test asserts (for example "calibrates" and "settles at DB 06"). Expected: the SND-A01 settle time drops from 37.2 s to about 10 s or less. **If the settle time is still over 15 s, the OS is missing gap 3:** stop and report the measured INTRQ-to-next-command latency instead of widening the gaps.
- [ ] **Step 5: Add `boot: SND-A01 settles with its lower sound loaded in under 15 s`** to `test_boot.cpp`: `bootedSnapshot("SND-A01.img").seconds < 15.0`.
- [ ] **Step 6: Update the docs.** In `fdc.md`, describe the new layout, state that it is a model choice, and give the measured sectors/s. Add a new boot-rom.md §13.6 with the new SND-A01 timeline (OS loaded, main loop, settled) and the old→new numbers. Update the README's note-timing paragraph and the `--note` example times.
- [ ] **Step 7: Run all tests; expect them green. Then commit.**

```bash
git commit -am "feat(engine): WD1772 cumulative sector layout; sound loads in seconds"
```

### Task 2: Panel display persistence and key query

**Files:**
- Modify: `engine/src/panel.cpp` (`kPersistCycles`), `engine/include/mirage/panel.h`,
  `engine/include/mirage/machine.h`
- Test: `tests/test_panel.cpp`, `tests/test_boot.cpp`

**Interfaces:**
- Produces: `bool Panel::keyDown(PanelKey) const;` and
  `bool Machine::keyDown(PanelKey key) const { return panel_.keyDown(key); }`

- [ ] **Step 1: Measure.** Write a scratch probe (not committed) that restores `bootedMachine("SND-A01.img")`, runs 10 s, and records for every lit segment the longest interval between two successive lit writes. Report the maximum in ms.
- [ ] **Step 2: Write the failing test** `boot: settled SND-A01 display never reads blank` (`[assets]`). From `bootedMachine`, sample `display()` every 10 ms for 3 s. Every sample must be `{0xDB, 0x06}`.
- [ ] **Step 3: Run it; expect FAIL** (about 1 in 10 samples blank).
- [ ] **Step 4: Set `kPersistCycles`** to the measured maximum × 1.5, rounded up to a whole 5 ms. It must stay below 137 ms, the ROM's blink phase (boot-rom.md §8). Document the measurement in the constant's comment and in boot-rom.md §13.1's "Other observations" bullet. Update the persistence-window unit tests (`display is blank 25 ms after ...`) to the new window.
- [ ] **Step 5: Add `boot: no disk still blinks "nd"`.** With no disk, sample every 10 ms from 2 s to 6 s. At least one sample must be lit (either digit non-zero) and at least one fully dark. Then add `panel keyDown reflects setKey`.
- [ ] **Step 6: Run all tests; expect them green. Commit** `feat(engine): panel persistence covers the OS's longest scan pass; keyDown query`.

### Task 3: Disk ownership, atomic save, write count

**Files:**
- Modify: `engine/include/mirage/disk_image.h`, `engine/src/disk_image.cpp`,
  `engine/include/mirage/machine.h`, `engine/src/machine.cpp`
- Modify callers: `tests/test_wd1772.cpp`, `tests/test_boot.cpp`,
  `tests/test_snapshot.cpp`, `tests/booted_machine.h`,
  `tools/mirage_cli/*.cpp`
- Test: `tests/test_disk_image.cpp`, `tests/test_wd1772.cpp`

**Interfaces:**
- Produces (`DiskImage`):
  - It is move-only: copy constructor and copy assignment are `= delete`,
    moves are `= default`.
  - `DiskImage clone() const;`
  - `void copyTo(DiskImage& dest) const;` does not allocate when `dest` is
    not moved-from (for example `DiskImage::blank()`). It copies tracks and
    the dirty flag, and `dest.writeCount()` becomes `writeCount()`.
  - `uint64_t writeCount() const;` counts `writeSector` calls since
    construction. It is not saved in snapshots and restarts at 0 on `load`.
  - `void saveFile(const std::filesystem::path&) const;` is now atomic:
    `toBytes()` first, then write `<path>.tmp` in the same folder, then
    `std::filesystem::rename` over the target. On any failure, remove the
    temp file and throw `DiskImageError` naming the path. The target is never
    left partial.
- Produces (`Machine`):
  - `std::unique_ptr<DiskImage> insertDisk(std::unique_ptr<DiskImage> disk);`
    returns the disk it replaced, or null. A null argument acts as an eject.
  - `std::unique_ptr<DiskImage> insertDisk(DiskImage disk);` is a
    convenience that heap-allocates.
  - `std::unique_ptr<DiskImage> ejectDisk();`
  - `DiskImage* disk();` and `const DiskImage* disk() const;`
  - Swapping pointers never allocates or frees. `disk_` becomes
    `std::unique_ptr<DiskImage>`.

- [ ] **Step 1: Write the failing tests.**
  - `disk image is move-only`: `static_assert(!std::is_copy_constructible_v<DiskImage>)` and `std::is_nothrow_move_constructible_v`.
  - `disk image clone and copyTo copy tracks, dirty flag and write count`.
  - `disk image writeCount counts writeSector calls`.
  - `disk image saveFile replaces an existing file atomically and leaves no .tmp`.
  - `disk image saveFile into a missing folder throws and leaves no .tmp`.
  - `disk image saveFile of a disk with an unformatted track throws and leaves the old file byte-identical`.
  - `machine insertDisk returns the previous disk`.
  - `machine ejectDisk on an empty drive returns null`.
- [ ] **Step 2: Run; expect compile failures and failing tests.**
- [ ] **Step 3: Implement.** Fix every caller: `.has_value()` becomes a null check, and copies become `clone()`.
- [ ] **Step 4: Run all tests; expect them green. Commit** `feat(engine): move-only DiskImage, atomic save, insertDisk returns the previous disk`.

### Task 4: Real-time `render()`: prepare, MIDI ring buffer, soft clip

**Files:**
- Modify: `engine/include/mirage/machine.h`, `engine/src/machine.cpp`,
  `engine/src/stream_resampler.cpp`
- Create: `tests/alloc_counter.h`, `tests/alloc_counter.cpp`, `tests/test_realtime.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces (`Machine`):
  - `void prepare(size_t maxFrames, double hostRate);` reserves `pending_`
    for `2 * maxFrames + 64` samples. Afterwards `render(out, n, rate)` with
    `n <= maxFrames` never allocates. Larger `n` still works but may
    allocate.
  - `static constexpr size_t kMidiQueueCapacity = 4096;` The MIDI input is a
    fixed ring buffer. `queueMidi` drops bytes beyond capacity and counts
    them.
  - `size_t midiQueueDepth() const;`, `uint64_t midiOverflows() const;`
    (not saved), and `void flushMidi();`, which discards queued, undelivered
    bytes. A byte already in the ACIA stays.
  - `static float softClip(float x);` is applied to every sample `render()`
    hands out.
- Produces (tests): `alloc_counter.h`, with
  `struct AllocationScope { AllocationScope(); ~AllocationScope(); size_t count() const; };`.
  It counts global `operator new` calls made on the constructing thread
  while it is alive. `alloc_counter.cpp` replaces the global `new`/`delete`
  family, using `malloc`/`free` plus a `thread_local` flag.
- `StreamResampler`: its constructor reserves `changes_` to
  `kMaxChanges + 1`.

- [ ] **Step 1: Write the failing tests** in `test_realtime.cpp`:
  - `softClip is identity to 0.75, odd, monotonic, below 1.0`: check 0.5→0.5, 0.75→0.75, −0.6→−0.6, 10→(0.99,1.0), and strictly increasing over a sweep.
  - `render after prepare allocates nothing` (`[assets]`). Take `bootedMachine("SND-A01.img")` and `prepare(512, 48000)`. Inside an `AllocationScope`, run 200 calls of `render(buf, 512, 48000)`, queue a note-on/off between them, and run one `flushMidi()`. Expect `count() == 0`.
  - `MIDI ring drops bytes beyond capacity and counts them`: queue 5000 bytes into a fresh machine. `midiQueueDepth() == 4096`, `midiOverflows() == 904`.
  - `flushMidi empties the queue`.
  - `snapshot rejects a MIDI queue longer than the ring`. Hand-edit a saved snapshot's count field, or build one via `StateWriter`.
- [ ] **Step 2: Run; expect FAIL.**
- [ ] **Step 3: Implement.** The ring is a `std::array<uint8_t, kMidiQueueCapacity>` with head and count. Remove `render()`'s per-call `reserve`. Apply `softClip` where samples enter `pending_`. Snapshot layout: unchanged order; the queue count is bounded by capacity. Update `machine.h`'s render comment with the headroom decision from the table above.
- [ ] **Step 4: Run all tests; expect them green.** Existing audio tests are unaffected, because their peaks are under 0.75. **Commit** `feat(engine): allocation-free render with prepare(), MIDI ring buffer, soft clip`.

### Task 5: MIDI out pacing with a cycle stamp

**Files:**
- Modify: `engine/include/mirage/acia6850.h`, `engine/src/acia6850.cpp`,
  `engine/include/mirage/machine.h`, `engine/src/machine.cpp`,
  `docs/hardware/clocks-and-midi.md` ("Transmit is instantaneous" bullet)
- Test: `tests/test_acia6850.cpp`

**Interfaces:**
- Produces (`Acia6850`):
  - `void tick(uint32_t eCycles, uint64_t now);`
  - `static constexpr uint32_t kByteCycles = 640;`
  - `onTransmit` becomes `std::function<void(uint8_t byte, uint64_t cycle)>`.
  - The transmitter is double-buffered as on a 6850. A data write goes to
    the holding register (TDRE=0). When the shift register is idle, the byte
    moves to it at once (TDRE=1 again) and `onTransmit(byte, now)` fires.
    The shift register stays busy for `kByteCycles`.
  - A write while TDRE=0 replaces the held byte, which is lost, as on
    hardware.
  - TX IRQ (CR6:5=01) follows TDRE.
  - Master reset clears both registers.
  - Save/load includes the held byte, the full flag and the shifter cycles
    left.
- Produces (`Machine`):
  - `std::function<void(uint8_t byte, uint64_t cycle)> onMidiOut;` where
    `cycle` is `cycles()` when the byte starts on the wire.
  - `kSnapshotVersion = 2`.
  - `tickDevices` ticks the ACIA and calls `updateFirq()`.

- [ ] **Step 1: Write the failing tests.**
  - `machine MIDI output is paced at one byte per 640 cycles`: a ROM program writes 3 bytes, each after polling TDRE. The stamps differ by 640 within one instruction's cycles (±8).
  - `machine MIDI output: two unpolled writes both go out, 640 cycles apart`. This replaces `reaches onMidiOut immediately`: the first stamp falls at the write, and the second byte waits in the holding register.
  - `ACIA TDRE reads 0 while the holding register is full`.
  - `ACIA save/load round-trips a byte in flight`.
  - `machine rejects a version 1 snapshot`.
- [ ] **Step 2: Run; expect FAIL.**
- [ ] **Step 3: Implement.** Update `mirage_cli` if it uses `onMidiOut` (it doesn't today), and update `clocks-and-midi.md`.
- [ ] **Step 4: Run all tests, including `cli_run_snapshot_round_trip`; expect them green. Commit** `feat(engine): paced MIDI transmit with cycle stamps (snapshot v2)`.

### Task 6: Snapshot hardening and the host-facing API

**Files:**
- Modify: `engine/include/mirage/state_io.h`, `engine/include/mirage/machine.h`,
  `engine/src/machine.cpp`
- Modify callers of `doc()`/`fdc()` that write: `tests/test_machine_map.cpp`,
  `tests/test_audio.cpp`
- Test: `tests/test_snapshot.cpp`, `tests/test_state_io.cpp`

**Interfaces:**
- Produces:
  - `StateReader(std::vector<uint8_t>&&) = delete;`
  - `const Doc5503& doc() const;` and `const Wd1772& fdc() const;` (the
    non-const overloads are removed).
  - `uint8_t Machine::debugRead(uint16_t addr);` and
    `void Machine::debugWrite(uint16_t addr, uint8_t value);` are the CPU
    view **with** I/O side effects, for tests and diagnostics. Tests use
    them for writes such as `debugWrite(0xECE1, 0x3E)` in place of
    `doc().write(0xE1, ...)`.
- `loadState` rejects these with `StateError`:
  - `credit > 64`;
  - with a non-empty MIDI queue, `nextMidiCycle` outside
    `(cycles − 64, cycles + 640]`.
- `loadState` keeps its RAM, expansion RAM and sample RAM temporaries on the
  heap (`std::make_unique`), not the stack.

- [ ] **Step 1: Write the failing tests.**
  - `StateReader cannot be built from a temporary vector`: `static_assert(!std::is_constructible_v<StateReader, std::vector<uint8_t>&&>)` and constructible from `const std::vector<uint8_t>&`.
  - `snapshot with an out-of-range credit is rejected and the machine is unchanged`.
  - `snapshot with a far-future next-MIDI cycle is rejected`.
  - Patch the fields at their offsets: the MCHN section is at a fixed offset after the 28-byte header (magic + version + SHA-1). Locate `nextMidiCycle` by writing the snapshot with a known queue and searching for the expected u64.
- [ ] **Step 2: Run; expect compile failure or FAIL.**
- [ ] **Step 3: Implement and fix callers.**
- [ ] **Step 4: Run all tests; expect them green. Commit** `fix(engine): snapshot bounds, heap temporaries, const chip accessors`.

### Task 7: `mirage_host` basics: SPSC queue, MIDI parser, panel layout, ROM check

**Files:**
- Create: `host/CMakeLists.txt`, `host/include/mirage/spsc_queue.h`,
  `host/include/mirage/midi_byte_parser.h`, `host/src/midi_byte_parser.cpp`,
  `host/include/mirage/panel_layout.h`, `host/src/panel_layout.cpp`,
  `host/include/mirage/boot_rom.h`, `host/src/boot_rom.cpp`
- Modify: `CMakeLists.txt` (`add_subdirectory(host)` before tests),
  `tests/CMakeLists.txt` (link `mirage_host`)
- Test: `tests/test_spsc_queue.cpp`, `tests/test_midi_byte_parser.cpp`,
  `tests/test_panel_layout.cpp`

**Interfaces:**
- Produces:

```cpp
// spsc_queue.h — capacity N (power of two); T default-constructible, movable.
// One producer thread calls push, one consumer thread calls pop. No allocation.
template <class T, size_t N> class SpscQueue {
 public:
  bool push(T&& v);        // false when full (v untouched)
  bool push(const T& v);
  bool pop(T& out);        // false when empty
  size_t sizeApprox() const;
};

// midi_byte_parser.h — rebuilds whole messages from a raw MIDI byte stream
// (running status, SysEx F0..F7, real-time F8-FF interleaved anywhere,
// stray data bytes dropped). For the MIDI-out thread; may allocate.
class MidiByteParser {
 public:
  void feed(uint8_t b, const std::function<void(std::span<const uint8_t>)>& emit);
};

// panel_layout.h — the replica panel in panel units (see Task 10 for values).
struct PanelRect { float x, y, w, h; };
enum class CapColour : uint8_t { Yellow, Grey, White, Black };
struct PanelButton { PanelKey key; std::string_view label; CapColour cap; PanelRect r; };
std::span<const PanelButton> panelButtons();  // 24 entries, one per PanelKey
struct PanelHeading { std::string_view text; float centreX; };
std::span<const PanelHeading> panelHeadings();  // VOLUME CONTROL SELECT SEQ SAMPLE LOAD
struct PanelGeometry { float width, height; PanelRect display, volume, driveSlot, driveLed,
                       ensoniqBadge, mirageLogo; float headingY, lipY; };
const PanelGeometry& panelGeometry();
enum class ShortcutKey : uint8_t { Up, Down, Return, Escape };
std::optional<PanelKey> panelKeyForChar(char32_t c);        // '0'..'9' -> K0..K9
std::optional<PanelKey> panelKeyForShortcut(ShortcutKey k); // Up, Down, Enter, Cancel

// boot_rom.h
inline constexpr std::array<uint8_t, 20> kKnownBootRomSha1 = {0xec, 0x6e, 0xa5, ...};
bool isKnownBootRom(const std::array<uint8_t, 20>& sha1);
```

- [ ] **Step 1: Write the failing tests.**
  - `spsc queue fills to N, rejects N+1, pops in order`.
  - `spsc queue moves unique_ptr through without leaks`.
  - `spsc queue survives a producer/consumer thread pair` (1,000,000 ints, in order).
  - Parser: `note on with running status emits two messages`, `real-time byte inside a message is emitted alone and the message completes`, `sysex is emitted whole`, `stray data bytes are dropped`, `program change is two bytes`.
  - `panel layout has each of the 24 keys exactly once`, `panel buttons lie inside the panel and don't overlap each other or the display`, `digits and arrows map to keys; other chars don't`.
  - `known boot ROM hash matches` (the hash of the real `mirage.bin` when present, via `Machine::romSha1()`).
- [ ] **Step 2: Run; expect FAIL. Step 3: implement. Step 4: run; expect green.**
- [ ] **Step 5: Commit** `feat(host): SPSC queue, MIDI byte parser, panel layout, ROM check`.

### Task 8: `RealtimeHost`

**Files:**
- Create: `host/include/mirage/realtime_host.h`, `host/src/realtime_host.cpp`
- Test: `tests/test_realtime_host.cpp`

**Interfaces:**
- Consumes: Tasks 2–7 (`Machine::prepare`, `queueMidi`, `flushMidi`,
  `keyDown`, `insertDisk`/`ejectDisk` with `unique_ptr`, `DiskImage::copyTo`,
  `writeCount`, `onMidiOut(byte, cycle)`, `SpscQueue`).
- Produces:

```cpp
struct HostStatus {
  DisplayState display; bool driveActive = false, diskPresent = false, diskDirty = false;
  uint64_t illegalOps = 0, midiDropped = 0, cycles = 0;
};
enum class DiskReturn : uint8_t { Ejected, Unused, Copy, NoDiskToCopy };
struct ReturnedDisk { std::unique_ptr<DiskImage> disk; DiskReturn why; uint64_t writeCount = 0; };
struct MidiOutByte { uint8_t byte; uint64_t cycle; };

class RealtimeHost {
 public:
  static constexpr uint64_t kSwapGapCycles = kCpuClockHz / 2;      // 0.5 s
  static constexpr uint64_t kMinKeyHoldCycles = kCpuClockHz / 10;  // 100 ms
  explicit RealtimeHost(std::span<const uint8_t> bootRom);  // powered on, drive empty
  ~RealtimeHost();
  std::array<uint8_t, 20> romSha1() const;
  // Audio thread only.
  void prepare(size_t maxFrames, double hostRate);
  void process(float* out, size_t frames, double hostRate, double nowSeconds);
  // MIDI-input thread only (the single producer of the MIDI queue).
  bool postMidi(std::span<const uint8_t> bytes, double timeSeconds);
  // UI thread only (the single producer of the UI queue).
  bool postKey(PanelKey key, bool down);
  bool postInsert(std::unique_ptr<DiskImage>&& disk);  // moved from only on success
  bool postEject();
  bool postReset();                                     // Machine::powerOn(); disk kept
  bool postPanic();                                     // flushMidi + CC123 x 16 channels
  bool postCopyDisk(std::unique_ptr<DiskImage>&& dest); // dest = DiskImage::blank()
  bool postMarkSaved(uint64_t writeCount);
  std::optional<ReturnedDisk> takeReturnedDisk();       // UI thread
  bool popMidiOut(MidiOutByte& out);                    // MIDI-output thread
  HostStatus status() const;                            // any thread
  // Tests only: the Machine, for use on the thread that calls process() while
  // no process() call is running.
  Machine& machineForTest();
};
```

Behaviour the tests pin down:
- **`process` order.**
  1. Drain the UI queue: keys, insert, eject, reset, panic, copy, mark
     saved.
  2. Drain up to 256 MIDI commands into a fixed array with block offsets.
     Each offset is `frames − llround((now − t)·hostRate)`, clamped to
     `[previous offset, frames − 1]`.
  3. Render the block in segments split at event offsets, and
     `queueMidi` each event at its offset.
  4. At every segment boundary, apply any due key release and any due swap
     insert.
  5. Publish the status atomics.
- **An invalid `hostRate`** (0, NaN) still drains and applies the UI queue,
  writes silence and advances nothing.
- **Each MIDI command carries up to 12 bytes inline.** `postMidi` splits
  longer messages (SysEx) into consecutive commands with the same
  timestamp. When the queue is full it returns false and adds the bytes to
  `midiDropped`.
- **Insert into an empty drive with nothing pending** happens now.
  Otherwise the current disk is ejected now (returned `Ejected`), and the
  new disk goes in once `cycles ≥ ejectCycle + kSwapGapCycles`. A second
  insert during the gap returns the earlier pending disk as `Unused`. Eject
  during the gap returns the pending disk as `Unused`.
- **Key release** before `kMinKeyHoldCycles` have passed since the press is
  deferred until they have.
- **Copy** copies the inserted disk into `dest` and returns it as `Copy`
  with the write count, or returns it as `NoDiskToCopy`. **Mark saved**
  clears dirty only if the write count still matches.
- **`onMidiOut`** pushes to an 8192-entry out queue. On overflow the byte is
  dropped and counted in `midiDropped`.
- **The destructor** frees anything left in the queues.

- [ ] **Step 1: Write the failing tests** (these use `bootedSnapshot` via a host built from the ROM plus `insert` where assets are needed; pure-logic tests use the synthetic ROMs from `test_machine_map.cpp`'s helper; move that helper to `tests/test_roms.h` if needed):
  - `host starts powered on with an empty drive` (`status().diskPresent == false`).
  - `host insert into an empty drive is immediate`.
  - `host swap ejects, waits 0.5 s of emulated time, then inserts`. Use 64-frame blocks at 48 kHz. `diskPresent` is false over `[eject, eject + 0.5 s)` and true after; the old disk comes back `Ejected`.
  - `host second insert during the gap returns the first as Unused`.
  - `host short key press is held for 100 ms`: post down then up before one 64-frame block. `machineForTest().keyDown(k)` stays true through every block ending before 100 ms of emulated time and is false after.
  - `host places MIDI by timestamp within the block`: 480-frame blocks at 48 kHz. Post a note-on stamped `now − 5 ms`, then `process(..., now)`. A reference `Machine` (same ROM, same prior blocks) renders the same block as `render(240)`, `queueMidi`, `render(240)`. Both machines' `saveState()` must be equal.
  - `host process allocates nothing after prepare`: `AllocationScope` around 100 `process` calls interleaved with posted keys, MIDI, an insert, a copy and an eject (the posts themselves happen outside the scope).
  - `host copy returns the disk contents and mark-saved clears dirty only when unchanged`.
  - `host invalid rate writes silence but still applies an eject`.
  - `host MIDI queue full drops and counts`.
  - `host panic flushes and sends CC123 on 16 channels`: queue 100 bytes, post panic, and run one tiny block. `machineForTest().midiQueueDepth()` is 48 or fewer (48 minus any already delivered), and the first queued bytes are `B0 7B 00`.
  - `host threads: a MIDI producer and a UI producer against a processing thread for 2 s run clean` (no crash; status readable throughout).
- [ ] **Step 2: Run; expect FAIL. Step 3: implement. Step 4: run; expect green.**
- [ ] **Step 5: Commit** `feat(host): RealtimeHost owns the Machine on the audio thread`.

### Task 9: JUCE app shell: window, menus, audio, MIDI, settings

**Files:**
- Create: `app/CMakeLists.txt`, `app/src/Main.cpp`, `app/src/MainWindow.{h,cpp}`,
  `app/src/AudioEngine.{h,cpp}`, `app/src/Settings.{h,cpp}`
- Modify: `CMakeLists.txt` (`option(MIRAGE_BUILD_APP "Build the JUCE app" ON)`,
  `if(MIRAGE_BUILD_APP) add_subdirectory(app) endif()`), `README.md`

**Interfaces:**
- Consumes: `RealtimeHost`, `MidiByteParser`, `isKnownBootRom`.
- Produces:
  - `class AudioEngine` with:
    - `explicit AudioEngine(mirage::RealtimeHost&);`
    - `juce::AudioDeviceManager& devices();`
    - `void setMidiInput(const juce::String& identifier);` (empty = none)
    - `void setMidiOutput(const juce::String& identifier);`
    - `juce::String midiInputId() const;` and `juce::String midiOutputId() const;`
    - `double cpuLoad() const;`
    - `void setVolume(float gain01);`, an atomic host-side gain applied after
      `process` (the panel's volume slider)
    - `std::unique_ptr<juce::XmlElement> saveAudioState() const;`
  - `class Settings` wraps `juce::ApplicationProperties` (folder
    "MirageEmulator") with `romPath`, `lastDiskDir`, `audioState`,
    `midiIn`, `midiOut`, `volume` and `windowState`.
  - `MainWindow` owns one `PanelComponent` (Task 10) and a status strip.

Requirements:
- `app/CMakeLists.txt`:
  - `FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.4 GIT_SHALLOW TRUE GIT_CONFIG core.longpaths=true)`.
  - `juce_add_gui_app(MirageApp PRODUCT_NAME "Mirage" VERSION 0.2.0 COMPANY_NAME "Mirage Emulator")`.
  - Definitions: `JUCE_WEB_BROWSER=0`, `JUCE_USE_CURL=0`,
    `JUCE_APPLICATION_NAME_STRING`, `JUCE_APPLICATION_VERSION_STRING`,
    and `MIRAGE_ASSET_ROOT` (for the default ROM location).
  - Links `mirage_host`, `juce::juce_gui_extra`, `juce::juce_audio_utils`
    and the JUCE recommended flags.
  - The engine's `/W4` must not apply to JUCE sources: add compile options
    per target rather than globally if JUCE warnings break the build.
- **Boot ROM.**
  1. Try `Settings::romPath`, then
     `MIRAGE_ASSET_ROOT/Ensoniq Mirage EPROM Firmware/mirage.bin`.
  2. Otherwise show a FileChooser ("Locate the Mirage boot ROM
     (mirage.bin)"). A file that is not 4096 bytes is refused with a
     message, and the dialog is offered again or the user quits.
  3. If `!isKnownBootRom`, show a non-blocking warning naming the expected
     SHA-1.
- **Audio.**
  - Restore the saved device state, or default to 0 inputs and 2 outputs.
  - The callback reads `juce::Time::getMillisecondCounterHiRes() * 0.001`
    as `now` and calls `host.process(channel0, n, rate, now)`.
  - It copies channel 0 to every other output channel, and zeroes outputs
    if the host is not ready.
  - `audioDeviceAboutToStart` calls `host.prepare(maxBlock, rate)`.
- **MIDI in.** Use `deviceManager.setMidiInputDeviceEnabled` plus
  `addMidiInputDeviceCallback`. The callback posts
  `msg.getRawData()/getRawDataSize()` with `msg.getTimeStamp()` (same
  clock as above).
- **MIDI out.** A `juce::Thread` "MIDI out" loops `host.popMidiOut` →
  `MidiByteParser` → `MidiOutput::sendMessageNow`, waiting 1 ms when idle.
  Bytes are sent in cycle order. No output device = drain and discard.
- **Menus** (`MenuBarModel`):
  - *File*: Open Disk Image… (Ctrl+O), Save Disk Image (Ctrl+S), Save Disk
    Image As… (Ctrl+Shift+S), Eject Disk (Ctrl+E), separator, Exit. The
    actions are wired in Task 11; until then they are disabled.
  - *Machine*: Reset (Ctrl+R).
  - *MIDI*: the input devices (radio items plus "None"), the output devices
    (radio items plus "None"), separator, Panic.
  - *Audio*: Audio Settings… (an `AudioDeviceSelectorComponent` in a
    `DialogWindow`, outputs only, no MIDI section).
- **Status strip** below the panel (outside the replica), refreshed at
  10 Hz:
  - the disk file name (or "No disk") and "modified" while dirty;
  - the audio device and rate, and CPU %;
  - "illegal ops: N" and "MIDI dropped: N" only when non-zero.
- **On quit,** save the settings. Task 11 adds the unsaved-disk prompt.

- [ ] **Step 1: Configure with the app.** Run `cmake -S emulator -B emulator/build ...`. Expected: JUCE fetched, configure succeeds. First fetch takes minutes; run it in the background.
- [ ] **Step 2: Build Debug and Release.** `cmake --build emulator/build --config Release --target MirageApp`. Expected: 0 errors.
- [ ] **Step 3: Smoke-run.** `emulator/build/app/MirageApp_artefacts/Release/Mirage.exe --panel-png <tmp>/shell.png` (Task 10 supplies the panel; at this task the flag renders the window content). Expected: exit 0, PNG written. Then run all ctest tests; expect green.
- [ ] **Step 4: Commit** `feat(app): JUCE shell with menus, audio and MIDI devices, settings`.

### Task 10: Replica front panel and keyboard shortcuts

**Files:**
- Create: `app/src/PanelComponent.{h,cpp}`
- Modify: `host/src/panel_layout.cpp` (final geometry), `app/src/Main.cpp`
  (`--panel-png`), `app/src/MainWindow.cpp` (key handling)
- Test: `tests/test_panel_layout.cpp` (extend), visual check of the PNG

**Interfaces:**
- Consumes: `panelButtons()`, `panelGeometry()`, `panelKeyForChar`,
  `panelKeyForShortcut`, `RealtimeHost::postKey`, `status()`.
- Produces: `class PanelComponent : public juce::Component, juce::Timer` with
  `PanelComponent(mirage::RealtimeHost&)`. It repaints at 30 Hz from
  `status()`, and only when the display or LED changed.

Layout source: `C:\Users\abart\AppData\Roaming\Claude\side-session-notes\local_ad6c4925-51e6-41c1-a28d-cd8cc1f8b84e\dsk8-panel-layout.md`
(external photos plus the keypad matrix), summarised in the **Panel layout**
section at the end of this plan. The values go into `panel_layout.cpp`.

Requirements:
- **Vector drawing only** (no bitmaps): the dark case, the "ensoniq" badge,
  the yellow "Mirage" logo and subtitle, the white rules and pinstripe, the
  section headings, and labelled caps in their colours, as in the Panel
  layout section.
- **A red two-digit 7-segment LED** drawn per segment from `DisplayState`:
  bit n = segment a..g, dp, with a lit red and a dim unlit red. The dp sits
  at the lower left of each digit, with the ⊥/L brackets to Param/Value.
- **The lip strip** holds the drive slot and the drive-activity LED, which
  is lit while `status().driveActive`. The slot shows the disk's file name
  while a disk is in.
- **The volume slider** is a `juce::Slider` (LinearVertical, no text box)
  styled to match. It sets `AudioEngine::setVolume(float)` and is persisted.
- **Each button** is a child component. Mouse down calls
  `postKey(key, true)`; mouse up or exit while down calls
  `postKey(key, false)`. It shows a pressed state.
- **The panel scales** with the window, keeping its aspect ratio; the window
  is resizable.
- **Keyboard shortcuts.** `MainWindow` is a `KeyListener`.
  `keyPressed` maps characters through `panelKeyForChar` and keys through
  `panelKeyForShortcut`: `KeyPress::upKey` → Up (On/Yes), `downKey` → Down
  (Off/No), `returnKey` → Enter, `escapeKey` → Cancel. It posts a press once
  per physical press (auto-repeat ignored). `keyStateChanged` releases any
  held shortcut key that `KeyPress::isKeyCurrentlyDown` reports up.
  Shortcuts are ignored while any modal dialog or file chooser is open
  (`juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0`,
  or a `dialogOpen_` flag set around native choosers). Ctrl/Alt combinations
  go to the menus.
- **`--panel-png <path>`** renders a `PanelComponent` at 1200 px wide via
  `createComponentSnapshot` with a fixed sample display (`{0xDB, 0x06}`,
  "2." "1") and the LED on, writes a PNG and exits 0.

- [ ] **Step 1: Extend `test_panel_layout.cpp`** with the final geometry assertions from the Panel layout section:
  - The display is above Param and Value and horizontally spans both.
  - On/▲ (Up) is right of Off/▼ (Down) on the same row, below Param/Value.
  - K1..K3, K4..K6, K7..K9 and Cancel/K0/Enter each share a row, left to right.
  - The SEQ, SAMPLE and LOAD columns are right of SELECT, in that order.
  - Each cap colour matches the table.
  - The labels are exactly "0/Prog", "Off/▼", "On/▲" (UTF-8).
- [ ] **Step 2: Implement `panel_layout.cpp` values and `PanelComponent`.**
- [ ] **Step 3: Build Release, then run `Mirage.exe --panel-png <scratch>/panel.png`.** Inspect the image (Read tool) against the layout section: every button has its label and position, and the display reads "2.1". Fix and repeat until it matches.
- [ ] **Step 4: Run all tests; expect them green. Commit** `feat(app): replica front panel, LED display and keyboard shortcuts`.

### Task 11: Disk files: open, save, save as, eject, unsaved prompts

**Files:**
- Create: `host/include/mirage/disk_session.h`, `host/src/disk_session.cpp`
- Modify: `app/src/MainWindow.cpp` (File menu actions, quit prompt, title)
- Test: `tests/test_disk_session.cpp`

**Interfaces:**
- Consumes: `RealtimeHost` (`postInsert`, `postEject`, `postCopyDisk`,
  `postMarkSaved`, `takeReturnedDisk`, `status().diskDirty`),
  `DiskImage::loadFile`, `saveFile`, `blank`.
- Produces: `DiskSession`, a JUCE-free UI-thread state machine:

```cpp
class DiskSession {
 public:
  explicit DiskSession(RealtimeHost& host);
  // Loads and validates now; on success posts the insert and remembers the path.
  // Returns an error message (empty on success). Does not prompt: the caller
  // checks needsSaveBeforeReplacing() first.
  std::string open(const std::filesystem::path& file);
  void eject();
  // Starts a save to `file` (current path when empty). Completes in poll().
  bool startSave(const std::filesystem::path& file = {});
  // Call on the UI thread at >= 10 Hz: frees returned disks, finishes saves.
  // Returns a finished save's outcome once (empty optional while none).
  std::optional<SaveOutcome> poll();
  bool needsSaveBeforeReplacing() const;   // a disk is in and status().diskDirty
  bool saving() const;
  const std::filesystem::path& path() const;  // empty: no disk
  std::string title() const;  // "SND-A01.img", "SND-A01.img (modified)" or "No disk"
};
struct SaveOutcome { bool ok; std::string message; std::filesystem::path path; };
```

- A save posts `postCopyDisk(DiskImage::blank())`. When the `Copy` returns,
  it calls `saveFile` (atomic) and, on success, `postMarkSaved(writeCount)`
  and records the new path (Save As). On failure it reports the error
  message and leaves dirty set. `NoDiskToCopy` reports "There is no disk in
  the drive."
- Every `Ejected` or `Unused` disk returned is freed in `poll()`.
- App wiring (MainWindow):
  - *Open* (dialog filters `*.img;*` with "All files"; this follows spec
    §6.5, where any 450,560-byte file is accepted): if
    `needsSaveBeforeReplacing()`, ask "Save changes to X before opening
    another disk?" with Save / Don't Save / Cancel. Save runs the save, and
    opens once its outcome is ok. Error messages from `open` are shown in an
    alert.
  - *Eject* and *Exit* (and the window close button) prompt the same way.
  - *Save* with no path runs *Save As*. *Save As* uses a save dialog with
    the `.img` default and an overwrite warning (JUCE flag
    `warnAboutOverwriting`).
  - The window title is `"Mirage — " + session.title()`. Menu items enable
    and disable with the disk state (no Save or Eject without a disk).

- [ ] **Step 1: Write the failing tests** (asset-free, using a synthetic ROM host plus generated disks written to a temp folder):
  - `session open rejects a wrong-size file with a clear message and leaves the drive empty`.
  - `session open of a directory or missing file reports an error`.
  - `session open inserts and reports the title`.
  - `session save writes the bytes the OS wrote and clears modified`: open, pump one `process`, then write a sector through `host.machineForTest().disk()->writeSector` (as the OS would). `title()` shows "(modified)". Start the save, then pump `process` + `poll` until the outcome arrives. The file holds the new sector, and dirty is clear.
  - `session save to an unwritable path fails and keeps modified`.
  - `session needsSaveBeforeReplacing only when dirty`.
  - `session save as switches the path`.
  - `session frees ejected disks` (no leak: the returned count hits zero).
- [ ] **Step 2: Run; expect FAIL. Step 3: implement `DiskSession` and the app wiring. Step 4: run all tests; expect green.**
- [ ] **Step 5: Commit** `feat: disk open/save/save-as/eject with unsaved-change prompts`.

### Task 12: Docs, acceptance checklist, final verification

**Files:**
- Create: `docs/app.md`
- Modify: `README.md`, `docs/plans/2026-10-18-plan-1-followups.md` (mark
  the Plan 2 items done, each with its commit)

- [ ] **Step 1: Write `docs/app.md`.** Cover the window, menus, shortcuts, threading model (who owns what, the queues and their capacities), settings location, the decisions table from this plan, and the **acceptance-1 checklist**:
  1. Start `Mirage.exe`. The display blinks "nd" after about 0.5 s.
  2. *MIDI → input:* your keyboard. *Audio Settings:* your output.
  3. *File → Open* `Factory Library/IMG/SND-A01.img`. The drive LED lights, and the display shows the boot. You hear the calibration tones briefly (expected, not muted).
  4. Wait for the display to settle on "2. 1" (about 10 s). The OS has auto-loaded the lower sound.
  5. Play. Then load another sound from the panel (*Load Upper/Lower*, a digit, Enter, as the OS 3.2 manual describes) and play it.
- [ ] **Step 2: Update the README** with app build and run instructions, the Release recommendation for live play, and the JUCE licence note.
- [ ] **Step 3: Verify.** Clean configure, Debug and Release builds, and full `ctest` in Debug: all green, with the counts reported. Run `Mirage.exe --panel-png` and confirm exit 0. Launch `Mirage.exe` normally for 10 s and confirm it stays up and quits cleanly via the window close.
- [ ] **Step 4: Commit** `docs: app guide and acceptance-1 checklist`.

---

## Panel layout

Sources: the Musician's Manual "The Front Panel" (pp. 4–6, with a numbered
illustration), close-up photos of the later grey DSK-8, and MAME's
`enmirage.lay`. They agree. The full research, with URLs, is in the notes
file named in Task 10. This is the later grey DSK-8.

**Case and branding:**
- Very dark charcoal case.
- Top strip, left to right:
  - a boxed, outlined **"ensoniq"** badge in light grey, low at the far
    left, with a thin pinstripe running right from it;
  - the control group;
  - **"Mirage"** in bold solid yellow, right of centre, with **"DIGITAL
    SAMPLING KEYBOARD"** under it in small, widely spaced light-grey caps.
- No "DSK-8" text.
- The floppy drive sits on the front lip below the left of the keybed.
  The window draws it as a lip strip under the top strip: the drive slot
  and its activity LED. That LED is the only drive light; its colour is
  unconfirmed, so use red.

**Control group:** a volume slider, then 8 button columns by 4 rows. Each
button's label is printed above it in small white mixed-case text. Six
uppercase section headings sit in one row under a white rule at the
bottom. Buttons are small rectangular "calculator" caps.

```
            [ 8.8 ]        1     2     3       Rec    Upper   Upper
   |=|      .    .        [Y]   [Y]   [Y]      [g]    [B]     [W]
   |=|     ⊥    L__       ------------------------------------------
   |=|                     4     5     6       Play   Lower   Lower
   |#|                    [Y]   [g]   [g]      [g]    [B]     [W]
   |=|     Param  Value   ------------------------------------------
   |=|      [g]    [g]     7     8     9       Load
   |=|                    [g]   [g]   [g]      [g]
   |=|  ----------------------------------------------------- pinstripe
           Off/▼   On/▲   Cancel 0/Prog Enter   Save
            [g]    [g]     [g]   [Y]   [W]      [g]
  ==================================================================
  VOLUME   CONTROL           SELECT            SEQ    SAMPLE   LOAD
```

Cap colours: Y = yellow/amber, g = light grey, W = white/cream, B = black.

| PanelKey | Label | Column, row (R1 top) | Cap |
|---|---|---|---|
| LoadUpper / LoadLower | "Upper" / "Lower" | LOAD (col 7), R1 / R2 | white |
| SampleUpper / SampleLower | "Upper" / "Lower" | SAMPLE (col 6), R1 / R2 | black |
| RecSeq, PlaySeq, LoadSeq, SaveSeq | "Rec" "Play" "Load" "Save" | SEQ (col 5), R1–R4 | grey |
| K1 K2 K3 | "1" "2" "3" | SELECT cols 2–4, R1 | yellow |
| K4 / K5 K6 | "4" / "5" "6" | SELECT R2 | yellow / grey |
| K7 K8 K9 | "7" "8" "9" | SELECT R3 | grey |
| Cancel, K0, Enter | "Cancel" "0/Prog" "Enter" | SELECT R4 | grey, yellow, white |
| Param, Value | "Param" "Value" | CONTROL cols 0–1, R3 | grey |
| Down, Up | "Off/▼" "On/▲" | CONTROL cols 0–1, R4 (Off left, On right) | grey |

**Display:**
- The two-digit LED sits above Param/Value (CONTROL, rows 1–2) behind a
  dark tinted window.
- Each digit's decimal point is that digit's indicator dot: the left dp is
  the manual's "Param Indicator" and the right dp the "Value Indicator".
  Draw the dp at the **lower left** of each digit.
- A thin white ⊥ bracket runs from the left dot down to "Param", and an L
  bracket from the right dot down to "Value".
- Segments are red, as the spec says (the photos leave the colour open).

**Volume slider:**
- A vertical slider with tick marks at the far left, headed "VOLUME".
- It is the analog output volume, so it is a **host-side gain** (0–1,
  default 1, persisted in Settings). `AudioEngine` applies it after
  `process`, never inside the engine.

**Rules:** white rules run under rows 1 and 2 from key 1 to the LOAD
column. The rule above row 4 runs from Param across the panel as the
pinstripe.

**Starting geometry** (panel units; the panel is 1000 × 300; refine it
visually in Task 10):
- Top strip: y 0–230. Lip strip: y 230–300.
- Columns:

  | Column | Centre x |
  |---|---|
  | slider | 150 |
  | CONTROL 0 / 1 | 210 / 262 |
  | SELECT 2 / 3 / 4 | 330 / 382 / 434 |
  | SEQ 5 | 500 |
  | SAMPLE 6 | 552 |
  | LOAD 7 | 604 |

- Buttons are 40 × 20. Row centres: y 62 / 104 / 146 / 188. Each label
  sits 14 above its button.
- Headings at y 214. Display: x 186–286, y 34–92.
- "ensoniq" badge: x 20–120, y 180–210. "Mirage": x 680–960, y 60–150.
- Drive slot: x 140–420, y 250–270. Drive LED: centre (440, 260).
