# Mirage Emulator — Plan 1: Engine Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A headless, OS-agnostic Mirage hardware emulator that boots the
real boot ROM and a disk image, runs the disk's OS, accepts panel keys and
MIDI, and renders audio to WAV. It's driven by a command-line tool.

**Architecture:** A plain C++20 static library (`mirage_engine`) with one
class per chip (6809, VIA, ACIA, WD1772, DOC 5503, filters) wired together by
`Machine`. `Machine` owns the memory map and the clocks. A CLI
(`mirage_cli`) and Catch2 tests sit on top. There are no GUI or audio
dependencies; the JUCE app comes in Plan 2.

**Tech Stack:** C++20, CMake ≥ 3.25, MSVC 2019 (v142, x64, installed),
Catch2 v3.7.1 via FetchContent, git.

**Spec:** `emulator/docs/specs/2026-10-18-mirage-emulator-design.md`

**Plan series:** The spec is split into three plans. Each one produces
working, testable software:
1. **This plan:** engine core. Boot, play and render headlessly.
2. **Standalone JUCE app:** replica panel, menus, real-time audio/MIDI I/O,
   device selection, file dialogs, shortcuts. This is acceptance 1.
3. **Disk writing UX, formatting and the rest:** Save/Save As/unsaved
   prompts, New Blank Disk and WD1772 Write Track, sampling from audio input
   and the level meter, other-OS sweep, EDM. That's acceptance 2–6.

Plans 2–3 are written after this plan lands, because their details depend on
what Tasks 5, 7 and 11 discover.

## Global Constraints

- **Faithful emulation only.** No OS-specific code paths, no OS patching, no
  loading samples or synthesising notes outside the emulated chips (spec §3).
- **ROM and disk images are user-owned.** They are never copied into
  `emulator/` or committed. Tests find them through the CMake cache variable
  `MIRAGE_ASSET_ROOT`, which defaults to the parent folder of `emulator/`.
  Asset-dependent tests `SKIP` when files are missing.
- **Boot ROM:** `Ensoniq Mirage EPROM Firmware/mirage.bin`, 4096 bytes, SHA1
  `ec6ea5613eeafd21d8f3a7431a35a6ff16eed56d`, mapped at `$F000–$FFFF`.
- **Disk geometry:** 80 tracks, single-sided. Sectors 0–4 are 1024 bytes and
  sector 5 is 512 bytes. 5,632 bytes/track, 450,560 bytes/image. Byte offset
  is `t*5632 + s*1024` for s≤4 and `t*5632 + 5120` for s=5.
- **No `.hfe` code, ever.**
- **Engine code is our own.** MAME `src/mame/ensoniq/enmirage.cpp`
  (BSD-3-Clause) and its chip cores are behavioural references only.
- **Every chip class has state save/restore.**
  `void save(StateWriter&) const; void load(StateReader&);`. Emulation is
  deterministic for identical inputs (spec §6.9).
- **Hardware findings go in `emulator/docs/hardware/`.** Every constant
  derived from reverse engineering cites its evidence there (ROM address, OS
  address, or MAME).

### Hardware reference (MAME `enmirage.cpp`, checked against the boot ROM in Task 5)

Evidence for every row is in `docs/hardware/boot-rom.md`. Where the ROM and
MAME disagree, the ROM wins.

**Memory map (6809):**

| Address | Device |
|---|---|
| `$0000–$7FFF` | 32 KB window onto 128 KB sample RAM. Quarter selected by VIA PB1:PB0. |
| `$8000–$BFFF` | Main RAM |
| `$C000–$DFFF` | Expansion RAM |
| `$E100–$E101` | 6850 ACIA (MIDI). The ROM writes control at `$E100` (`$03` reset, `$95` at OS hand-off). |
| `$E200–$E2FF` | 6522 VIA (register = addr & 15) |
| `$E400–$E41F` | Filter CV writes. Channel = addr & 7, function = (addr>>3) & 3 (meaning confirmed in Task 11). |
| `$E800–$E803` | WD1772 |
| `$EC00–$ECEF` | DOC 5503. The DOC sees all 128 KB of sample RAM. |
| `$F000–$FFFF` | Boot ROM |

**Interrupts:**
- NMI ← WD1772 INTRQ.
- IRQ ← OR of VIA IRQ, WD1772 DRQ and DOC IRQ. The ROM moves disk data with
  `SYNC` while I/F are masked, which is consistent with DRQ on IRQ. NMI ←
  INTRQ is confirmed: the ROM's NMI handler `$F0B0` ends every FDC command.
- FIRQ ← ACIA IRQ.

**Clocks:**
- 8 MHz master, which also clocks the DOC and the WD1772.
- CPU E clock 2 MHz (MAME; verified in Task 7). The ROM supports this: VIA T1
  is free-running with latch 0, PB7 drives the ACIA at ÷16, and that gives
  31,250 baud only if the VIA's Φ2 = E = 2 MHz. MAME clocks its VIA at
  3 MHz, which would be wrong.

**VIA port A (front panel):**
- **Outputs:**
  - PA0–2: column/segment select, decoded by a 74LS145 into 8 lines.
  - PA3: **left**-digit enable.
  - PA4: **right**-digit enable. Both enables are active low.
  - MAME's comment has PA3/PA4 the other way round. The ROM wins: its error
    words `$7A9E` "dE", `$387A` "ud", `$2A7A` "nd" and `$FDB7` "O.S." put the
    high byte (the left glyph) on PA3 (`$F33C`).
  - DDRA = `$1F`.
- **Inputs:** PA5/PA6/PA7 sense key rows 0/1/2, active low.

**VIA port B:**
- **Outputs:**
  - PB0: 32 KB quarter select.
  - PB1: 64 KB half select.
  - PB2–3: ADC mux. 0 = compressed input (audio in mixed with voice output),
    1 = line input, 2 = pitch wheel, 3 = mod wheel.
  - PB3 also acts as sample/play.
  - PB4: drive select/motor/keyboard-controller reset, **active low** (0 =
    motor on; `$F4C6`/`$F4D6`).
  - PB7: ACIA clock (VIA T1 output).
- **Inputs:**
  - PB5: DOC sync, 500 kHz.
  - PB6: **disk present**, a level signal, 1 = disk in. The ROM reads it
    before any step and shows "nd" when it is 0 (`$F116`). It is not a
    latched disk-change.
- DDRB = `$1F`.
- **Boot ROM VIA setup:** ACR `$CC` (T1 free-run with PB7, T2 one-shot, SR
  in on CB1). PCR `$0E`. T1 latch 0. T2 = 5000 cycles. IER `$7F` during
  boot, `$A6` (T2, SR, CA1) at OS hand-off.
- **Shift register and CA2/CB1/CB2:** these go to the keyboard controller. Not
  emulated: the rack DMS-8 has none.

**Keypad matrix (row, column 0–7):**

| Row | Keys, columns 0–7 |
|---|---|
| 0 | LoadUpper, LoadLower, SampleUpper, PlaySeq, LoadSeq, SaveSeq, RecSeq, SampleLower |
| 1 | 3, 6, 9, 5, 8, 0, 2, Enter |
| 2 | 1, 4, 7, Up(On/Yes), Param, Down(Off/No), Value, Cancel |

**Display:** writing PA selects segment `PA&7` (a, b, c, d, e, f, g, dp =
0–7) on the enabled digit(s). The display is multiplexed, so it must be
integrated over time.

**Wheels at rest:** pitch `0x80`, mod `0x00`. This is a constant in
`clocks.h` and is revisited in Task 11 if the OS's center calibration
disagrees.

## Review Focus

1. **Non-bootable disks.** A blank, sound-only (`Soundprocess Sound
   Packs/IMG/*`) or garbage disk at power-on must leave the machine running
   whatever the boot ROM does (retry/error display). The host must never
   crash or hang. Test in Task 9.
2. **Bad image files.** Wrong-size, truncated or empty files, and
   extensionless files of the right size: the first group gets a clear error,
   the last loads. Test in Task 2.
3. **A runaway CPU.** Illegal opcodes or jumps into garbage must never throw,
   assert or loop the host. Record the event and continue. Test in Task 3.
4. **Noisy MIDI from real keyboards.** Running status, active sensing `0xFE`
   and clock `0xF8` floods are queued and paced at 320 µs/byte, with no loss
   while the OS reads in time. Test in Task 7.
5. **No disk while the OS accesses the drive.** No disk inserted, or eject
   mid-read: the FDC behaves as hardware does (no index pulses → Record Not
   Found / timeout), and nothing crashes. Test in Task 8.

---

## File Structure

```
emulator/
  CMakeLists.txt                 top-level: options, Catch2, subdirs
  .gitignore                     build/, *.wav, *.snap
  README.md                      build/test/run commands
  engine/CMakeLists.txt
  engine/include/mirage/
    clocks.h                     clock and timing constants
    state_io.h                   StateWriter / StateReader
    disk_image.h                 DiskImage
    bus.h                        Bus interface
    cpu6809.h                    Cpu6809, Regs, CC flags
    disasm6809.h                 disassemble6809()
    via6522.h  acia6850.h  wd1772.h  doc5503.h
    voice_filter.h               VoiceFilter, FilterBank (CV latches)
    panel.h                      PanelKey, DisplayState, Panel
    machine.h                    Machine
    wav_writer.h                 writeWav()
  engine/src/*.cpp               one .cpp per header
  tools/mirage_cli/main.cpp      headless tool
  tests/CMakeLists.txt
  tests/test_*.cpp               one test file per component
  tests/asset_paths.h            assetPath("…") + SKIP helper
  docs/hardware/
    boot-rom.md                  annotated boot ROM findings (Task 5)
    clocks-and-midi.md           E clock / baud derivation (Task 7)
    filters.md                   CV decode, calibration, mapping (Task 11)
```

---

### Task 1: Project scaffold, StateWriter/Reader

**Files:**
- Create: `emulator/CMakeLists.txt`, `emulator/.gitignore`,
  `emulator/README.md`, `emulator/engine/CMakeLists.txt`,
  `emulator/engine/include/mirage/state_io.h`,
  `emulator/engine/src/state_io.cpp`,
  `emulator/engine/include/mirage/clocks.h`, `emulator/tests/CMakeLists.txt`,
  `emulator/tests/asset_paths.h`, `emulator/tests/test_state_io.cpp`,
  `emulator/tools/mirage_cli/main.cpp` (prints usage)

**Interfaces:**
- Produces:
  - `class StateWriter { void u8(uint8_t); void u16(uint16_t); void u32(uint32_t); void u64(uint64_t); void i32(int32_t); void f64(double); void bytes(std::span<const uint8_t>); void tag(uint32_t fourcc); std::vector<uint8_t> take(); }`
  - `class StateReader { explicit StateReader(std::span<const uint8_t>); uint8_t u8(); …same set…; void tag(uint32_t expected) /* throws StateError on mismatch */; bool atEnd() const; }`
  - `struct StateError : std::runtime_error`. Little-endian.
- Produces `clocks.h`:
  `kMasterClockHz = 8'000'000`, `kCpuClockHz = 2'000'000`,
  `kMidiByteSeconds = 10.0/31250.0`, `kPitchWheelRest = 0x80`,
  `kModWheelRest = 0x00`.
- Produces `asset_paths.h`: `std::filesystem::path assetPath(std::string_view rel)`,
  plus macro `REQUIRE_ASSET(p)` that calls Catch2 `SKIP` if `p` is missing.

- [ ] **Step 1: `git init` in `emulator/`, and write the CMake files.**
  - Targets: `mirage_engine` (static), `mirage_tests` (Catch2WithMain,
    registered with `catch_discover_tests`), `mirage_cli`.
  - `MIRAGE_ASSET_ROOT` cache path, default `${CMAKE_CURRENT_SOURCE_DIR}/..`,
    passed to the tests as a compile definition.
  - MSVC flags: `/W4 /permissive- /utf-8`.

- [ ] **Step 2: Write the failing test `test_state_io.cpp`.**

```cpp
TEST_CASE("state round-trips all widths") {
  StateWriter w; w.tag('TEST'); w.u8(0xAB); w.u16(0xBEEF); w.u32(0xDEADBEEF);
  w.u64(0x0123456789ABCDEFull); w.i32(-5); w.f64(1.5);
  auto buf = w.take();
  StateReader r(buf); r.tag('TEST');
  REQUIRE(r.u8()==0xAB); REQUIRE(r.u16()==0xBEEF); REQUIRE(r.u32()==0xDEADBEEF);
  REQUIRE(r.u64()==0x0123456789ABCDEFull); REQUIRE(r.i32()==-5); REQUIRE(r.f64()==1.5);
  REQUIRE(r.atEnd());
}
TEST_CASE("state tag mismatch and overrun throw") {
  StateWriter w; w.tag('AAAA'); auto buf = w.take();
  StateReader r(buf); REQUIRE_THROWS_AS(r.tag('BBBB'), StateError);
  StateReader r2(buf); r2.tag('AAAA'); REQUIRE_THROWS_AS(r2.u8(), StateError);
}
```

- [ ] **Step 3: Configure and build. Expect a compile failure (no
  implementation yet).**
  - `cmake -S emulator -B emulator/build -G "Visual Studio 16 2019" -A x64`
  - `cmake --build emulator/build --config Debug`

- [ ] **Step 4: Implement `state_io.cpp`. Run
  `ctest --test-dir emulator/build -C Debug --output-on-failure`. Expect
  PASS.**

- [ ] **Step 5: Write `README.md` (the three commands above, plus
  `MIRAGE_ASSET_ROOT`). Commit:
  `git add -A && git commit -m "chore: scaffold engine, tests, state io"`.**
  Also commit the existing `docs/specs` and `docs/plans`.

---

### Task 2: DiskImage

**Files:**
- Create: `engine/include/mirage/disk_image.h`, `engine/src/disk_image.cpp`,
  `tests/test_disk_image.cpp`

**Interfaces:**
- Produces:

```cpp
struct DiskImageError : std::runtime_error { using runtime_error::runtime_error; };
class DiskImage {
public:
  static constexpr int kTracks = 80, kTrackBytes = 5632, kImageBytes = 450560;
  static DiskImage fromBytes(std::span<const uint8_t>);        // throws DiskImageError
  static DiskImage loadFile(const std::filesystem::path&);     // any extension
  static DiskImage blank();                                    // every track unformatted
  std::vector<uint8_t> toBytes() const;                        // throws if any track unformatted
  void saveFile(const std::filesystem::path&) const;
  bool trackFormatted(int track) const;
  int  sectorSize(int track, int sectorId) const;              // 1024, 512, or 0 if absent
  void readSector(int track, int sectorId, std::span<uint8_t> out) const;
  void writeSector(int track, int sectorId, std::span<const uint8_t> in); // sets dirty
  bool dirty() const; void clearDirty();
  void save(StateWriter&) const; void load(StateReader&);
};
```

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST_CASE("sector offsets match Mirage layout") {
  std::vector<uint8_t> raw(DiskImage::kImageBytes);
  raw[32*5632 + 5120 + 511] = 0x7E; raw[2*5632 + 0] = 0x11; raw[79*5632 + 4*1024] = 0x22;
  auto d = DiskImage::fromBytes(raw);
  std::array<uint8_t,1024> s{}; std::array<uint8_t,512> t{};
  d.readSector(32,5,t); REQUIRE(t[511]==0x7E);
  d.readSector(2,0,s);  REQUIRE(s[0]==0x11);
  d.readSector(79,4,s); REQUIRE(s[0]==0x22);
  REQUIRE(d.sectorSize(0,0)==1024); REQUIRE(d.sectorSize(0,5)==512); REQUIRE(d.sectorSize(0,6)==0);
}
TEST_CASE("wrong sizes are rejected with a clear message") {
  for (size_t n : {size_t(0), size_t(450559), size_t(450561), size_t(819200)})
    REQUIRE_THROWS_WITH(DiskImage::fromBytes(std::vector<uint8_t>(n)),
      Catch::Matchers::ContainsSubstring("450,560"));
}
TEST_CASE("write marks dirty and round-trips through bytes") {
  auto d = DiskImage::fromBytes(std::vector<uint8_t>(DiskImage::kImageBytes));
  REQUIRE_FALSE(d.dirty());
  std::array<uint8_t,1024> s{}; s[0] = 0x5A; d.writeSector(10,3,s);
  REQUIRE(d.dirty()); REQUIRE(d.toBytes()[10*5632 + 3*1024] == 0x5A);
  d.clearDirty(); REQUIRE_FALSE(d.dirty());
}
TEST_CASE("blank disk is unformatted and refuses toBytes") {
  auto d = DiskImage::blank(); REQUIRE_FALSE(d.trackFormatted(0));
  REQUIRE(d.sectorSize(0,0)==0); REQUIRE_THROWS_AS(d.toBytes(), DiskImageError);
}
TEST_CASE("factory disk directory byte", "[assets]") {
  auto p = assetPath("Factory Library/IMG/SND-A01.img"); REQUIRE_ASSET(p);
  std::array<uint8_t,512> t{}; DiskImage::loadFile(p).readSector(32,5,t); REQUIRE(t[511]==0x7E);
}
TEST_CASE("extensionless image loads", "[assets]") {
  auto p = assetPath("EnsoniqOS/os32"); REQUIRE_ASSET(p);
  REQUIRE_NOTHROW(DiskImage::loadFile(p));
}
```

- [ ] **Step 2: Run. Expect FAIL (no implementation).**
- [ ] **Step 3: Implement.** Store per-track
  `std::optional<std::array<uint8_t,5632>>` (nullopt means unformatted). The
  error text for a bad size is
  `"Not a Mirage disk image: expected exactly 450,560 bytes, got N."`.
- [ ] **Step 4: Run. Expect PASS.**
- [ ] **Step 5: Commit `feat(engine): DiskImage`.**

---

### Task 3: 6809 CPU core

**Files:**
- Create: `engine/include/mirage/bus.h`, `engine/include/mirage/cpu6809.h`,
  `engine/src/cpu6809.cpp`, `tests/test_cpu6809.cpp`

**Interfaces:**
- Produces:

```cpp
struct Bus { virtual uint8_t read(uint16_t) = 0; virtual void write(uint16_t, uint8_t) = 0; virtual ~Bus() = default; };
namespace cc { constexpr uint8_t E=0x80,F=0x40,H=0x20,I=0x10,N=0x08,Z=0x04,V=0x02,C=0x01; }
struct Regs { uint8_t a=0,b=0,dp=0,cc=0; uint16_t x=0,y=0,u=0,s=0,pc=0;
              uint16_t d() const; void setD(uint16_t); };
class Cpu6809 {
public:
  explicit Cpu6809(Bus&);
  void reset();                 // CC |= I|F, DP=0, PC = word at $FFFE, NMI disarmed until S written
  int  step();                  // one instruction or one interrupt entry; returns E cycles
  void setNmi(bool line);       // edge (low→high asserted) latched
  void setIrq(bool line); void setFirq(bool line);   // level
  Regs& regs(); const Regs& regs() const;
  uint64_t illegalCount() const; uint16_t lastIllegalPc() const;
  void save(StateWriter&) const; void load(StateReader&);
};
```

- Illegal opcodes: count them, record the PC, execute as a 2-cycle no-op, and
  never throw.
- The full instruction set includes the page-2 (`$10`) and page-3 (`$11`)
  prefixes, all indexed modes including indirect, CWAI, SYNC, SWI/SWI2/SWI3,
  and RTI with the E flag.
- Cycle counts follow the Motorola MC6809E datasheet tables, including the
  indexed-mode extra cycles.

- [ ] **Step 1: Write the failing tests.**
  - **Helper:** `struct RamBus : Bus` (64 KB). `Cpu6809 cpuAt(RamBus&, uint16_t pc, std::initializer_list<uint8_t> code)`
    writes the code at `pc`, points the reset vector at it, and resets.
  - **Required cases:** one `TEST_CASE` per row below. Each executes the code,
    then asserts the listed registers, CC bits and returned cycles.

| Code (hex) at $1000 | Setup | Expect after step(s) |
|---|---|---|
| `86 80` LDA #$80 | — | A=80, N=1 Z=0 V=0, 2 cyc |
| `8B 01` ADDA #1 | A=7F | A=80, V=1 N=1 H=1 C=0, 2 cyc |
| `80 01` SUBA #1 | A=00 | A=FF, C=1 N=1 V=0 |
| `3D` MUL | A=0C B=64 | D=04B0, C=1 Z=0, 11 cyc |
| `8B 01 19` ADDA #1; DAA | A=09 | A=10 |
| `1D` SEX | B=80 | A=FF, N=1 |
| `3A` ABX | X=1000 B=FF | X=10FF, CC unchanged, 3 cyc |
| `A6 80` LDA ,X+ | X=2000, [2000]=5A | A=5A, X=2001, 6 cyc |
| `A6 9F 30 00` LDA [$3000] | [3000]=40 00, [4000]=77 | A=77 |
| `A6 1F` LDA -1,X | X=2001, [2000]=33 | A=33, 5 cyc |
| `A6 8B` LDA D,X | D=0010 X=2000 [2010]=44 | A=44 |
| `30 8C 10` LEAX $10,PCR | — | X = 1003+10 = 1013, Z by result |
| `34 16` PSHS X,B,A; `35 16` PULS | S=8000 | after PSHS S=7FFC: [7FFC]=A [7FFD]=B [7FFE..7FFF]=X (hi,lo); PULS restores; PSHS 9 cyc |
| `BD 20 00` JSR $2000; at 2000 `39` RTS | S=8000 | PC=1003 after RTS; 8 + 5 cyc |
| `10 27 00 10` LBEQ | Z=1 | PC=1014, 6 cyc |
| `1E 89` EXG A,B | A=1 B=2 | A=2 B=1 |
| `11 83 12 34` CMPU #$1234 | U=1234 | Z=1 |

  - **Interrupt cases:**
    - **IRQ, I=0:** pushes 12 bytes, sets E=1 and I=1, PC=[FFF8], 19 cyc.
    - **FIRQ, F=0:** pushes PC+CC, E=0, I=F=1, PC=[FFF6], 10 cyc. RTI after
      FIRQ pops 3 bytes.
    - **NMI before any S write:** ignored. After `10 CE 80 00` (LDS #$8000),
      NMI is taken via [FFFC].
    - **CWAI `3C EF`:** waits until IRQ, then vectors without re-pushing.
    - **SYNC `13`:** resumes on IRQ with I=1 without vectoring.
  - **Illegal opcode `01`:** the step returns 2, `illegalCount()==1`,
    `lastIllegalPc()==0x1000`, and no exception.
  - **Save/load:** registers and pending-NMI state survive a save/load
    round-trip.
- [ ] **Step 2: Run. Expect FAIL.**
- [ ] **Step 3: Implement** with opcode tables generated from the datasheet
  (mnemonic, mode, base cycles). Share the table with Task 5's disassembler.
- [ ] **Step 4: Run. Expect PASS.**
- [ ] **Step 5: Commit `feat(engine): MC6809E core`.**

---

### Task 4: Machine skeleton and memory map

**Files:**
- Create: `engine/include/mirage/machine.h`, `engine/src/machine.cpp`,
  `tests/test_machine_map.cpp`

**Interfaces:**
- Consumes: `Cpu6809`, `Bus`, `DiskImage`, `StateWriter/Reader`.
- Produces (the initial `Machine`; later tasks extend it):

```cpp
class Machine : private Bus {
public:
  explicit Machine(std::span<const uint8_t> bootRom);  // throws std::invalid_argument unless 4096 bytes
  void powerOn();                                      // clears RAM to 0, resets all chips and CPU
  void runCycles(uint64_t eCycles);                    // steps CPU, then ticks devices by the cycles it took
  uint64_t cycles() const;
  uint8_t peek(uint16_t addr);                         // CPU view, no side effects on I/O
  uint8_t sampleRam(uint32_t addr) const;              // 0..0x1FFFF
  Cpu6809& cpu();
  std::vector<uint8_t> saveState() const; void loadState(std::span<const uint8_t>);
};
```

- Unmapped reads return `0xFF`; ROM writes are ignored. Device stubs are
  replaced in later tasks.
- The bank window uses the last value written to VIA port B bits 0–1 (wired
  in Task 6; until then expose `setSampleBank(int)` as private plus a test
  hook).

- [ ] **Step 1: Write the failing tests.**
  - **Reset:** after `powerOn()` with the real ROM, `cpu().regs().pc == 0xFC7F`
    (`[assets]`).
  - **ROM vectors:** a synthetic ROM with `FFFE=F0 00` gives PC=F000.
  - **RAM:** a write to `$8123` reads back.
  - **ROM is read-only:** a write to `$F000` doesn't change it.
  - **Bank window:** with bank 2, a CPU write to `$0005` gives
    `sampleRam(0x10005)`.
  - **Save/load:** `saveState`/`loadState` round-trips RAM and the CPU.
- [ ] **Step 2: Run. Expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run. Expect PASS.**
- [ ] **Step 5: Commit `feat(engine): machine memory map`.**

---

### Task 5: Disassembler, trace, boot ROM findings

**Files:**
- Create: `engine/include/mirage/disasm6809.h`, `engine/src/disasm6809.cpp`,
  `tests/test_disasm6809.cpp`, `docs/hardware/boot-rom.md`
- Modify: `tools/mirage_cli/main.cpp`, adding the `disasm` subcommand.

**Interfaces:**
- Produces:
  - `struct Disasm { std::string text; int length; };`
  - `Disasm disassemble6809(const std::function<uint8_t(uint16_t)>& peek, uint16_t pc);`
    The text format is `"LDA   ,X+"`, with mnemonic padding to 6.
- Produces CLI: `mirage_cli disasm <file> [--org 0xF000] [--from A --to B]`.

- [ ] **Step 1: Write the failing tests.**
  - `86 80` → `"LDA   #$80"`, length 2.
  - `10 27 00 10` at $1000 → `"LBEQ  $1014"`, length 4.
  - `A6 9F 30 00` → `"LDA   [$3000]"`.
  - `01` → `"FCB   $01"`, length 1.
- [ ] **Step 2: Run. Expect FAIL.**
- [ ] **Step 3: Implement** from the Task 3 opcode table.
- [ ] **Step 4: Run. Expect PASS.**
- [ ] **Step 5: Disassemble the boot ROM and write `docs/hardware/boot-rom.md`.**
  It must cover:
  - the reset path;
  - every I/O address touched, mapped to the device table above (confirm or
    correct each row);
  - how the OS is loaded (which tracks and sectors, to what RAM addresses,
    and the entry point);
  - interrupt vector usage (`$8008`/`$800B`);
  - display codes the ROM shows (for example while waiting for a disk or on
    error).

  Where the ROM contradicts MAME, the ROM wins: update the Hardware reference
  table in this plan, plus `clocks.h` if needed.
- [ ] **Step 6: Commit `feat(engine): 6809 disassembler; docs: boot ROM findings`.**

---

### Task 6: VIA 6522 and front-panel wiring

**Files:**
- Create: `engine/include/mirage/via6522.h`, `engine/src/via6522.cpp`,
  `engine/include/mirage/panel.h`, `engine/src/panel.cpp`,
  `tests/test_via6522.cpp`, `tests/test_panel.cpp`
- Modify: `machine.h/.cpp` to map `$E200`, wire port A to the panel, wire
  port B bits 0–1 to the bank and PB6 to disk change, and connect the VIA IRQ.

**Interfaces:**
- Produces:

```cpp
class Via6522 {
public:
  uint8_t read(uint8_t reg); void write(uint8_t reg, uint8_t v);   // reg 0..15
  void tick(uint32_t eCycles);
  bool irq() const;
  bool pb7() const;                                       // T1 PB7 output level
  std::function<uint8_t()> portAPins, portBPins;          // external input levels
  std::function<void(uint8_t)> portAOut, portBOut;        // (ORx & DDRx) | ~DDRx, on every change
  void save(StateWriter&) const; void load(StateReader&);
};
enum class PanelKey : uint8_t { LoadUpper, LoadLower, SampleUpper, PlaySeq, LoadSeq, SaveSeq, RecSeq, SampleLower,
  K3, K6, K9, K5, K8, K0, K2, Enter, K1, K4, K7, Up, Param, Down, Value, Cancel };  // = row*8 + column
struct DisplayState { uint8_t left = 0, right = 0; };     // bit n = segment n (a..g, dp)
class Panel {
public:
  void setKey(PanelKey, bool down);
  uint8_t rowSense(uint8_t portA) const;   // bits 5..7, active low, for column (portA & 7)
  void portAWritten(uint8_t portA, uint64_t cycle);       // records lit segments
  DisplayState display(uint64_t nowCycle) const;          // segments lit within the last 20 ms
  void save(StateWriter&) const; void load(StateReader&);
};
```

- Machine adds `void setKey(PanelKey, bool)` and
  `DisplayState display() const`.

- [ ] **Step 1: Write the failing tests.**
  - **VIA:**
    - DDR masking on reads and writes.
    - T1 one-shot sets IFR bit 6 after N+1.5 cycles (datasheet) and IRQ when
      IER bit 6 is set.
    - T1 free-run with ACR bit 7 toggles `pb7()` each time-out.
    - Reading T1C-L clears IFR6.
    - The T2 one-shot sets IFR5.
    - Writing IFR with bit 7 clears bits.
    - IER set/clear semantics (bit 7).
  - **Panel:**
    - With `Enter` down and portA column 7, `rowSense` bit 6 is 0 and bits 5
      and 7 are 1.
    - With no keys down, all row bits are 1.
    - Writing portA = `0b00010_000 | 3` (right digit enabled, segment d)
      gives `display().right == 0x08` and `left == 0`.
    - 25 ms after the last write the display is blank.
  - **Machine:** the bank select via VIA PB1:PB0 maps `$0000` to the right
    quarter.
- [ ] **Step 2: Run. Expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run. Expect PASS.**
- [ ] **Step 5: Commit `feat(engine): VIA 6522 and front panel`.**

---

### Task 7: ACIA 6850, MIDI in/out, clock verification

**Files:**
- Create: `engine/include/mirage/acia6850.h`, `engine/src/acia6850.cpp`,
  `tests/test_acia6850.cpp`, `docs/hardware/clocks-and-midi.md`
- Modify: `machine.*`. Map `$E100`, connect FIRQ, and add a MIDI input queue
  that delivers one byte per `kMidiByteSeconds` of emulated time.

**Interfaces:**
- Produces:

```cpp
class Acia6850 {
public:
  uint8_t read(uint8_t reg); void write(uint8_t reg, uint8_t v);  // 0=status/control, 1=data
  void receiveByte(uint8_t);        // sets RDRF; sets OVRN if RDRF already set (byte lost, as hardware)
  bool irq() const;
  std::function<void(uint8_t)> onTransmit;
  void save(StateWriter&) const; void load(StateReader&);
};
```

- Machine adds `void queueMidi(std::span<const uint8_t>)` and
  `std::function<void(uint8_t)> onMidiOut`.

- [ ] **Step 1: Write the failing tests.**
  - **Master reset:** write control `0x03`, then `0x95` (÷16, 8N1, RX IRQ
    on). Status == `0x02` (TDRE only; CTS/DCD tied active). Bits: RDRF=0,
    TDRE=1, OVRN=5, IRQ=7.
  - **Receive:** with RX IRQ enabled (CR7), `receiveByte` sets RDRF and
    `irq()`. Reading data clears both.
  - **Overrun:** two `receiveByte` calls without a read set OVRN, and the read
    returns the first byte.
  - **Transmit:** writing data calls `onTransmit`, and TDRE ends up set.
  - **MIDI pacing (Machine):** queue 1,000 bytes including `F8` and `FE`. Run
    a tiny CPU program at `$8000` that polls RDRF and stores the bytes in RAM.
    All 1,000 arrive in order, with no OVRN. Elapsed emulated time is
    ≈ 1000 × 320 µs ± 1 byte.
- [ ] **Step 2: Run. Expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run. Expect PASS.**
- [ ] **Step 5: Verify the clock and write it up.** After Task 9 boots,
  record the OS's VIA T1 latch and the ACIA divide setting in
  `clocks-and-midi.md`, and confirm they yield 31,250 baud with
  `kCpuClockHz`. If they don't, fix `kCpuClockHz` and note the evidence.
  This step can be completed after Task 9; mark it done then.
- [ ] **Step 6: Commit `feat(engine): ACIA 6850 and paced MIDI input`.**

---

### Task 8: WD1772 floppy controller (read and write sector)

**Files:**
- Create: `engine/include/mirage/wd1772.h`, `engine/src/wd1772.cpp`,
  `tests/test_wd1772.cpp`
- Modify: `machine.*`.
  - Map `$E800–$E803`.
  - INTRQ → NMI, DRQ → IRQ.
  - Motor from VIA PB4. Disk change → VIA PB6.
  - Add `insertDisk(DiskImage)`, `std::optional<DiskImage> ejectDisk()`,
    `DiskImage* disk()`, and `bool driveActive() const` (motor on).

**Interfaces:**
- Produces:

```cpp
class Wd1772 {
public:
  uint8_t read(uint8_t reg); void write(uint8_t reg, uint8_t v);   // 0 status/cmd, 1 track, 2 sector, 3 data
  void tick(uint32_t eCycles);
  bool intrq() const; bool drq() const;
  void setDisk(DiskImage*);          // nullptr = empty drive
  void setMotor(bool on);
  bool diskChanged() const; void ackDiskChange();
  uint64_t sectorsRead() const;      // diagnostics for the CLI and boot tests
  void save(StateWriter&) const; void load(StateReader&);
};
```

- **Behaviour** follows `Manuals/wd1772.txt`:
  - **Commands:**
    - Type I: Restore, Seek, Step, Step-in, Step-out, with step rates
      6/12/2/3 ms and verify.
    - Type II: Read Sector, Write Sector. Single sector and multi-sector (m
      flag).
    - Type III: Read Address.
    - Type IV: Force Interrupt.
    - Write Track and Read Track are Plan 3. Until then they complete with no
      data and set no error.
  - **Timing:** 250 kbit/s MFM, so one byte every 32 µs. Index every 200 ms.
    Spin-up waits 6 index pulses unless h=1.
  - **Errors:** Record Not Found after 5 revolutions without a matching ID.
    Lost Data if DRQ isn't serviced in time.
  - **Missing disk:** with no disk, no index pulses, so Type II times out with
    RNF.
  - **Sector size:** comes from `DiskImage::sectorSize`.

- [ ] **Step 1: Write the failing tests** against a synthetic DiskImage, with
  sector (3, 2) filled with `i & 0xFF`:
  - **Restore:** reaches track 0 and sets INTRQ.
  - **Seek:** seek to 3 updates the track register.
  - **Read Sector 2:** the data register serves 1,024 bytes in order, each
    with DRQ, then INTRQ with status clear of errors.
  - **Read Sector 6:** RNF (status bit 4) after 5 revolutions.
  - **Write Sector 5 on track 3:** 512 bytes reach `DiskImage`, which is
    marked dirty.
  - **Read Address:** returns track, side 0, sector, size code
    (3 → 1024, 2 → 512) and CRC.
  - **No disk:** Read Sector gives RNF, and nothing throws.
  - **Eject mid-read:** the command aborts with RNF/Lost Data, and nothing
    throws.
  - **Disk change:** inserting sets `diskChanged()`.
- [ ] **Step 2: Run. Expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run. Expect PASS.**
- [ ] **Step 5: Commit `feat(engine): WD1772 read/write sector`.**

---

### Task 9: First boot and the headless CLI `run`

**Files:**
- Modify: `tools/mirage_cli/main.cpp`, `machine.*` (trace sink)
- Create: `tests/test_boot.cpp`

**Interfaces:**
- Produces Machine: `void setTrace(std::function<void(uint16_t pc, const Regs&)>)`.
  It's called before each instruction when set; zero cost when unset.
- Produces CLI:

```
mirage_cli run --disk <img> [--rom <bin>] [--seconds S]
               [--key NAME@T[:HOLD]]... [--note N:VEL@T:DUR]... [--midi-hex "90 3C 64"@T]...
               [--wav out.wav --rate 48000] [--trace trace.txt [--trace-from T]]
               [--snapshot-out f] [--snapshot-in f]
```

  - `--rom` defaults to `<MIRAGE_ASSET_ROOT>/Ensoniq Mirage EPROM Firmware/mirage.bin`.
  - Key `NAME` uses PanelKey names, case-insensitive.
  - At the end it prints `display: LL RR` (segment hex), the `illegal`
    opcode count, the FDC sectors read, and the cycles.
  - Audio and `--wav` become live in Task 12. Snapshots become live in
    Task 13.

- [ ] **Step 1: Write the failing tests.**
  - **SND-A01 boots** (`[assets]`, ROM + `SND-A01.img`). After 10 emulated
    seconds:
    - the PC has executed from `$8000–$DFFF` (the OS is running in RAM);
    - `illegalCount()==0`;
    - the FDC read ≥ the number of OS sectors from `boot-rom.md`;
    - the display isn't blank.
  - **Blank disk:** after 10 s nothing threw, the machine is still running,
    and `illegalCount()==0`. The display shows the ROM's no-OS behaviour.
  - **Sound-only disk** (`Soundprocess/Soundprocess Sound Packs/IMG/*`, first
    file, `[assets]`): same as the blank disk.
- [ ] **Step 2: Run. Expect FAIL.**
- [ ] **Step 3: Implement the CLI and trace. Debug the boot using
  `--trace`.** Fix chip bugs found here in their own component with a new
  regression test in that component's test file. Expect an infinite wait on
  filter calibration or the DOC if the OS needs them. If so, this test may be
  marked `[!mayfail]` until Task 11 lands; record where it stops in
  `boot-rom.md`.
- [ ] **Step 4: Run. Expect PASS (or the documented `[!mayfail]`).**
- [ ] **Step 5: Commit `feat: first boot via mirage_cli run`.**

---

### Task 10: DOC 5503

**Files:**
- Create: `engine/include/mirage/doc5503.h`, `engine/src/doc5503.cpp`,
  `tests/test_doc5503.cpp`
- Modify: `machine.*`.
  - Map `$EC00–$ECEF`. The DOC reads sample RAM directly; BS selects the
    upper 64 KB.
  - DOC IRQ → IRQ.
  - Tick: one oscillator slot every 8 master clocks, which is 4 E cycles at
    2 MHz.

**Interfaces:**
- Produces:

```cpp
class Doc5503 {
public:
  explicit Doc5503(std::function<uint8_t(uint32_t addr17)> waveRead);
  uint8_t read(uint8_t reg); void write(uint8_t reg, uint8_t v);
  void tickMasterClocks(uint32_t n);                 // advances oscillator slots
  bool irq() const;
  double sampleRate() const;                         // 8e6 / (8 * (enabled + 2))
  std::function<uint8_t()> adcRead;                  // read of $E2 calls this
  // emitted once per full scan: per-channel sums of (sample-128)*volume, channels 0..7 = CA & 7
  std::function<void(const std::array<int32_t,8>&)> onFrame;
  void save(StateWriter&) const; void load(StateReader&);
};
```

- **Semantics** are exactly as the spec §6.2 and
  `datasheets/v4_13_EnsoniqDOC.pdf` describe:
  - **Accumulator:** add first, then form the address (§5 of the datasheet).
  - **Address formation:** pointer bits plus accumulator bits, per the
    table-size and resolution tables.
  - **Halt on zero:** a zero byte halts the oscillator and sets H.
  - **Modes:** one-shot halts at the end of the table. Free-run wraps.
  - **Sync and AM:** even/odd pairing per buchty's clarification. Sync resets
    the odd oscillator when the even one wraps; with the bit on the odd
    oscillator, AM replaces its volume with the even oscillator's data.
  - **Swap:** at the end, the even oscillator halts and the odd one is
    cleared and reset.
  - **Interrupts:** an interrupt FIFO feeds the OIR. Bit 7 low while pending,
    osc# in bits 5–1, bits 6 and 0 read as 1. A read pops the FIFO.
  - **Oscillator enable:** OER = 2 × count.

- [ ] **Step 1: Write the failing tests.**
  - **Sample rate:** OER `0x40` → 29411.76 Hz (±0.01). OER `0x30` →
    8e6/(8·26) = 38461.54 Hz.
  - **Address stepping:** osc 0 with ptr `0x12`, size 0, res 0, FC `0x0200`,
    started. Successive reads are from `0x1201, 0x1202, …` (accumulator bits
    16–9).
  - **Table size:** the general rule is
    `addr = ((ptr<<8) & ~mask) | ((acc >> (9 + res - size)) & mask)`, with
    `mask = (256<<size) - 1`. So size 3 (2 KB), res 3, ptr `0x2F` gives
    `0x2800 | ((acc>>9) & 0x7FF)`; pointer bits P2–P0 are ignored. With
    res 4 and size 3, the same FC steps twice as fast (`acc>>10`).
  - **Halt on zero:** wave byte 0 → H set, IE set → OIR == `0x40 | (5<<1) | 1`
    for osc 5. A second OIR read has bit 7 = 1.
  - **One-shot:** a 256-byte table halts after a single pass. Free-run is
    still running after 3 passes.
  - **Swap:** at the end of osc 2, osc 2 halts and osc 3 starts from acc 0.
  - **Sync:** when osc 0 wraps, osc 1's accumulator resets.
  - **Bank select:** BS=1 reads address + 0x10000.
  - **Output:** sample `0xC0`, volume 255, channel 3 → `onFrame[3] == (0xC0-128)*255`;
    other channels are 0.
  - **ADC:** a read of reg `0xE2` returns `adcRead()`.
- [ ] **Step 2: Run. Expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run. Expect PASS.**
- [ ] **Step 5: Commit `feat(engine): DOC 5503`.**

---

### Task 11: Filters, CV path, ADC mux, and boot calibration

**Files:**
- Create: `engine/include/mirage/voice_filter.h`,
  `engine/src/voice_filter.cpp`, `tests/test_voice_filter.cpp`,
  `docs/hardware/filters.md`
- Modify: `machine.*`.
  - Map `$E400–$E41F` → `FilterBank`.
  - DOC `adcRead` → mux by VIA PB2–3: channel 0 = compressed (filtered voice
    mix plus audio-in), 1 = line in, 2 = `kPitchWheelRest`,
    3 = `kModWheelRest`.

**Interfaces:**
- Produces:

```cpp
class VoiceFilter {                 // 4-pole resonant low-pass, CEM3328 in spirit
public:
  void setSampleRate(double hz);
  void setCutoffHz(double hz); void setResonance(double r01);   // r01: 0..1, ~0.95+ self-oscillates
  float process(float in);
  void save(StateWriter&) const; void load(StateReader&);
};
class FilterBank {                  // 8 voices; owns CV latches decoded from $E400-$E41F writes
public:
  void cvWrite(uint8_t offset, uint8_t value);       // offset 0..0x1F
  uint8_t cutoffCv(int ch) const; uint8_t resonanceCv(int ch) const;
  void process(const std::array<float,8>& voiceIn, double rateHz, float& mixOut);
  void save(StateWriter&) const; void load(StateReader&);
};
```

- The CV-byte → Hz and CV-byte → resonance mapping lives in one place,
  `voice_filter.cpp` (`cutoffHzFromCv`, `resonanceFromCv`). It's documented
  in `filters.md`.

- [ ] **Step 1: Write the failing filter tests** (48 kHz, sine
  measurements):
  - **Passband:** at resonance 0 and cutoff 5 kHz, a 100 Hz sine's gain is
    within ±1 dB of unity.
  - **Roll-off:** 20 kHz is at least 30 dB down.
  - **Volume drop with resonance:** at resonance 1.0 with cutoff 1 kHz, 100 Hz
    gain is between −14 and −3 dB (the CEM3328 passband drop).
  - **Self-oscillation:** after a single impulse, at resonance 1.0, the RMS
    over the second 1 s window is > 0.01.
  - **Stability:** no NaN/Inf for a 10 s full-scale square at every cutoff in
    20 Hz–20 kHz.
- [ ] **Step 2: Run. Expect FAIL.**
- [ ] **Step 3: Implement `VoiceFilter`.** Use a TPT/ZDF 4-pole ladder with
  `tanh` saturation in the feedback path.
- [ ] **Step 4: Reverse-engineer the CV writes and calibration.**
  - From OS 3.2 (`EnsoniqOS/os32`) under `mirage_cli run --trace`, plus
    disassembly:
    - decode what each `$E400–$E41F` function writes (cutoff, resonance,
      DAC preset, both);
    - work out how the OS calibrates filters at boot: which voice and
      waveform it plays, which ADC channel it reads, and what result it
      expects.
  - Write `filters.md`, then implement `FilterBank::cvWrite` and the
    `cutoffHzFromCv`/`resonanceFromCv` mapping so calibration succeeds on its
    own terms. That means no special-casing the OS.
  - If calibration needs a CEM property the model lacks (for example the
    exact Hz per CV step), adjust the mapping, not the OS.
- [ ] **Step 5: Write the decode tests in `test_voice_filter.cpp`.** For every
  function confirmed in `filters.md`, add an exact test, for example
  `cvWrite(0x0B, 0x40)` → `cutoffCv(3) == 0x40`.
- [ ] **Step 6: Run all tests. Expect PASS, including Task 9's boot test
  without `[!mayfail]`.** Boot also completes for `EnsoniqOS/masos` and
  `Soundprocess/Soundprocess OS/Soundprocess OS.img`; add those as extra
  `[assets]` boot cases.
- [ ] **Step 7: Commit `feat(engine): filters, CV path, ADC mux; boot calibrates`.**

---

### Task 12: Audio render, MIDI notes, WAV out

**Files:**
- Create: `engine/include/mirage/wav_writer.h`, `engine/src/wav_writer.cpp`,
  `tests/test_audio.cpp`
- Modify: `machine.*` (render path), `tools/mirage_cli/main.cpp` (`--wav`,
  `--note`, `--midi-hex` live)

**Interfaces:**
- Produces:
  - Machine: `void render(float* out, size_t frames, double hostRate);`
    - It runs emulation for exactly `frames/hostRate` seconds (carrying
      fractional cycles).
    - DOC frames go through `FilterBank`, are mixed and DC-blocked, and are
      resampled from `doc.sampleRate()` (which can change at run time) to
      `hostRate`. Output is mono, nominal peak ≤ 1.0.
    - The resampler choice is the builder's (spec §8), but it must not add
      interpolation inside the oscillator model.
  - `void writeWav(const std::filesystem::path&, std::span<const float>, int rate);`
    writes 16-bit PCM mono.
  - `class StreamResampler { explicit StreamResampler(double outRate); void setInputRate(double hz); void push(float s); bool pop(float& out); }`
    lives in `engine/include/mirage/stream_resampler.h` and `src/stream_resampler.cpp`.
    `Machine::render` uses it.

- [ ] **Step 1: Write the failing tests.**
  - **WAV writer:** the header has the correct sizes and rate, and 0.5 maps
    to 16384.
  - **Note playback** (`[assets]`):
    - Boot `SND-A01.img`. Use the panel to load the first sound, using the
      key sequence the OS needs (find it from the Musician's Guide/parameter
      card behaviour; record it in `boot-rom.md`).
    - Render 1 s of silence, which must have RMS < 1e-3.
    - Send `90 3C 64`, render 1 s, then `80 3C 00`. The note window's RMS is
      > 0.01.
    - Over the 4 s after release, the RMS falls below 10% of the note
      window's.
    - The pitch is checked by autocorrelation: MIDI 60 and 72 differ by a
      factor of 2 ±3%.
  - **Resampler rate change:** `StreamResampler(48000)` is fed a 440 Hz sine
    generated at 29411.76 Hz, then switched with `setInputRate(38461.54)` while
    generation continues at the new rate (same 440 Hz). In the output, the
    largest sample-to-sample step within ±50 samples of the switch is ≤ 1.5×
    the largest step elsewhere. The estimated pitch is 440 Hz ±1% on both
    sides.
- [ ] **Step 2: Run. Expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run. Expect PASS.** Then render
  `mirage_cli run --disk SND-A01.img --note 60:100@3:2 --wav a01.wav --seconds 8`
  and listen. Note any obvious artefacts in the commit message.
- [ ] **Step 5: Commit `feat(engine): audio render and WAV output`.**

---

### Task 13: Machine snapshots and determinism

**Files:**
- Modify: `machine.*` (`saveState`/`loadState` cover every chip, the disk,
  RAM, MIDI queue, render resampler and fractional-cycle state),
  `tools/mirage_cli/main.cpp` (`--snapshot-in/out` live)
- Create: `tests/test_snapshot.cpp`

**Interfaces:**
- Snapshot layout: tag `'MIRG'`, then `u32` version 1, then a tag per chip.
  `loadState` throws `StateError` on an unknown version and leaves the
  machine unchanged.

- [ ] **Step 1: Write the failing tests** (`[assets]`):
  - **Determinism:** boot SND-A01 for 10 s and `saveState()`. Render 2 s with
    a note → buffer A. Restore, then render the same 2 s with the same note →
    buffer B. A == B bit-for-bit.
  - **Bad version:** a snapshot with version 99 throws, and the display and
    cycles are unchanged.
  - **Disk dirt survives:** a disk written by the OS stays dirty after a
    save/load round-trip.
- [ ] **Step 2: Run. Expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run. Expect PASS.**
- [ ] **Step 5: Commit `feat(engine): deterministic machine snapshots`.**

---

## Done when

- `ctest -C Debug` passes with assets present. Without assets, the asset
  tests SKIP and everything else passes.
- `mirage_cli run --disk "…/SND-A01.img" --note 60:100@3:2 --wav a01.wav`
  produces a recognisable Mirage sound.
- `docs/hardware/` holds the boot ROM, clock/MIDI and filter findings that
  Plans 2 and 3 build on.
