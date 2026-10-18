// First boot with the real boot ROM and real disk images (Task 9), and the
// OS's filter calibration (Task 11). docs/hardware/boot-rom.md sections 12-13
// and docs/hardware/filters.md describe what these tests expect.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "asset_paths.h"
#include "mirage/clocks.h"
#include "mirage/machine.h"

using namespace mirage;

namespace {

constexpr uint64_t kSecond = kCpuClockHz;
// boot-rom.md section 6: the ROM reads 12 sectors (T0-1) + 9 sectors (S5 of
// T2-10) of OS image, then the config sector T11 S5.
constexpr uint64_t kBootSectors = 12 + 9 + 1;
// runCycles may overshoot by the tail of its last instruction (credited
// against the next call); no 6809 instruction or interrupt entry is this long.
constexpr uint64_t kMaxOvershoot = 32;

bool ranFor(uint64_t cycles, uint64_t requested) {
  return cycles >= requested && cycles < requested + kMaxOvershoot;
}

std::vector<uint8_t> loadFile(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// The ROM's display words use bit 7 = segment a (boot-rom.md section 8);
// DisplayState uses bit 0 = segment a.
constexpr uint8_t fromRomFont(uint8_t v) {
  uint8_t r = 0;
  for (int i = 0; i < 8; ++i)
    if (v & (0x80 >> i)) r |= uint8_t(1 << i);
  return r;
}

bool shows(const DisplayState& d, uint16_t romWord) {
  return d.left == fromRomFont(uint8_t(romWord >> 8)) && d.right == fromRomFont(uint8_t(romWord));
}

constexpr uint16_t kUd = 0x387A, kNd = 0x2A7A;  // boot-rom.md section 8

// One pass of the ROM's filter calibration $F571 (filters.md 4), seen at its
// exit $F5AC: B is the cutoff CV it settled on, Y the count of its last
// measurement ($81-$84 when it converged; 0 or >= $FA0 when it gave up).
struct Calibration {
  uint8_t cv = 0;
  uint16_t count = 0;
  uint64_t cycle = 0;
};

// What a run looked like: the PCs seen, the calibrations, and the display
// sampled every 10 ms (it is multiplexed and blinks, so one sample is not
// enough).
struct BootRun {
  bool ranFromRam = false;    // PC in $8000-$DFFF at some point
  bool jumpedTo800E = false;  // the ROM's hand-off JMP $800E was taken
  bool sawUd = false, sawNd = false, sawLit = false;
  std::vector<Calibration> calibrations;
  uint64_t cycles = 0;
};

// Runs `seconds`, or with `untilSettled` until the OS has finished booting
// (display lit throughout the last second and no sector read for 2 s), at most
// `seconds`.
BootRun boot(Machine& m, uint64_t seconds, bool untilSettled = false) {
  BootRun out;
  m.setTrace([&out, &m](uint16_t pc, const Regs& r) {
    if (pc >= 0x8000 && pc < 0xE000) out.ranFromRam = true;
    if (pc == 0x800E) out.jumpedTo800E = true;
    if (pc == 0xF5AC) out.calibrations.push_back({r.b, r.y, m.cycles()});
  });
  uint64_t lastSectors = m.fdc().sectorsRead(), quietTicks = 0, litTicks = 0;
  for (uint64_t t = 0; t < seconds * 100; ++t) {
    m.runCycles(kSecond / 100);
    const DisplayState d = m.display();
    if (d.left || d.right) out.sawLit = true;
    litTicks = d.left || d.right ? litTicks + 1 : 0;
    if (shows(d, kUd)) out.sawUd = true;
    if (shows(d, kNd)) out.sawNd = true;
    quietTicks = m.fdc().sectorsRead() == lastSectors ? quietTicks + 1 : 0;
    lastSectors = m.fdc().sectorsRead();
    if (untilSettled && litTicks >= 100 && quietTicks >= 200) break;
  }
  m.setTrace(nullptr);
  out.cycles = m.cycles();
  return out;
}

// The OS calibrated all 8 filters, each converging (filters.md 4) near the
// OS's centre CV $7A, and the last one finished by `bySeconds`.
void checkCalibrated(const BootRun& r, double bySeconds) {
  REQUIRE(r.calibrations.size() == 8);
  for (const Calibration& c : r.calibrations) {
    CHECK(c.count >= 0x81);
    CHECK(c.count <= 0x84);
    CHECK(int(c.cv) >= 0x7A - 8);
    CHECK(int(c.cv) <= 0x7A + 8);
  }
  CHECK(double(r.calibrations.back().cycle) / kSecond < bySeconds);
}

#define LOAD_ROM(var)                                                           \
  const auto var##Path = assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"); \
  REQUIRE_ASSET(var##Path);                                                     \
  const std::vector<uint8_t> var = loadFile(var##Path);                         \
  REQUIRE(var.size() == 4096)

const char* const kSndA01 = "Factory Library/IMG/SND-A01.img";

// The first image (by name) of the Soundprocess sound packs.
std::filesystem::path firstSoundPack() {
  const auto dir = assetPath("Soundprocess/Soundprocess Sound Packs/IMG");
  std::vector<std::filesystem::path> files;
  if (std::filesystem::is_directory(dir))
    for (const auto& e : std::filesystem::directory_iterator(dir))
      if (e.is_regular_file()) files.push_back(e.path());
  std::sort(files.begin(), files.end());
  return files.empty() ? dir / "missing.img" : files.front();
}

}  // namespace

TEST_CASE("boot: SND-A01 loads the OS and runs it from RAM", "[assets]") {
  LOAD_ROM(rom);
  const auto disk = assetPath(kSndA01);
  REQUIRE_ASSET(disk);
  Machine m(rom);
  m.insertDisk(DiskImage::loadFile(disk));
  m.powerOn();
  const BootRun r = boot(m, 10);
  CHECK(ranFor(r.cycles, 10 * kSecond));
  CHECK(r.jumpedTo800E);
  CHECK(r.ranFromRam);
  CHECK(m.cpu().illegalCount() == 0);
  CHECK(m.fdc().sectorsRead() >= kBootSectors);
  CHECK_FALSE(r.sawUd);
}

// The OS calls the ROM's filter calibration $F571 once per filter, which
// times each filter's self-oscillation through the DOC A/D (filters.md 4),
// then enters its main loop and scans the display (about 7.4 s). The display
// goes dark again while the OS loads the lower sound (from about 7.4 s to 35 s
// with the FDC model's one sector per revolution, fdc.md), so it is sampled
// over the whole 10 s.
TEST_CASE("boot: SND-A01 lights the display within 10 s", "[assets]") {
  LOAD_ROM(rom);
  const auto disk = assetPath(kSndA01);
  REQUIRE_ASSET(disk);
  Machine m(rom);
  m.insertDisk(DiskImage::loadFile(disk));
  m.powerOn();
  const BootRun r = boot(m, 10);
  CHECK(r.calibrations.size() == 8);
  CHECK(r.sawLit);
}

// The whole boot: the OS calibrates the 8 filters within about a second of
// its start (6.25 s), then loads the lower sound and settles.
TEST_CASE("boot: SND-A01 calibrates its filters and finishes booting", "[assets]") {
  LOAD_ROM(rom);
  const auto disk = assetPath(kSndA01);
  REQUIRE_ASSET(disk);
  Machine m(rom);
  m.insertDisk(DiskImage::loadFile(disk));
  m.powerOn();
  const BootRun r = boot(m, 60, true);
  checkCalibrated(r, 8.0);
  CHECK(r.cycles < 60 * kSecond);  // settled before the cap
  CHECK(r.sawLit);
  CHECK_FALSE(r.sawUd);
  CHECK(m.fdc().sectorsRead() > kBootSectors);  // the OS loaded a sound
  CHECK(m.cpu().illegalCount() == 0);
}

// Other operating systems call the same ROM calibration from their own code
// (filters.md 4); the hardware model has to satisfy them all unchanged.
TEST_CASE("boot: EnsoniqOS masos calibrates its filters and boots", "[assets]") {
  LOAD_ROM(rom);
  const auto disk = assetPath("EnsoniqOS/masos");
  REQUIRE_ASSET(disk);
  Machine m(rom);
  m.insertDisk(DiskImage::loadFile(disk));
  m.powerOn();
  const BootRun r = boot(m, 60, true);
  checkCalibrated(r, 8.0);
  CHECK(r.jumpedTo800E);
  CHECK(r.cycles < 60 * kSecond);
  CHECK(r.sawLit);
  CHECK_FALSE(r.sawUd);
  CHECK(m.fdc().sectorsRead() > kBootSectors);  // the OS loaded a sound
  CHECK(m.cpu().illegalCount() == 0);
}

TEST_CASE("boot: the Soundprocess OS disk calibrates its filters and boots", "[assets]") {
  LOAD_ROM(rom);
  const auto disk = assetPath("Soundprocess/Soundprocess OS/Soundprocess OS.img");
  REQUIRE_ASSET(disk);
  Machine m(rom);
  m.insertDisk(DiskImage::loadFile(disk));
  m.powerOn();
  const BootRun r = boot(m, 60, true);
  checkCalibrated(r, 8.0);
  CHECK(r.jumpedTo800E);
  CHECK(r.cycles < 60 * kSecond);
  CHECK(r.sawLit);
  CHECK_FALSE(r.sawUd);
  CHECK(m.fdc().sectorsRead() > kBootSectors);  // the OS loaded a sound
  CHECK(m.cpu().illegalCount() == 0);
}

TEST_CASE("boot: a blank disk shows \"ud\" and the ROM retries", "[assets]") {
  LOAD_ROM(rom);
  Machine m(rom);
  m.insertDisk(DiskImage::blank());
  m.powerOn();
  BootRun r;
  REQUIRE_NOTHROW(r = boot(m, 10));
  CHECK(ranFor(r.cycles, 10 * kSecond));
  CHECK(r.sawUd);
  CHECK_FALSE(r.sawNd);
  CHECK_FALSE(r.jumpedTo800E);
  CHECK_FALSE(r.ranFromRam);
  CHECK(m.cpu().illegalCount() == 0);
  CHECK(m.fdc().sectorsRead() == 0);
  // Still running: another second of emulation advances by that second.
  m.runCycles(kSecond);
  CHECK(ranFor(m.cycles(), 11 * kSecond));
}

TEST_CASE("boot: no disk shows \"nd\"", "[assets]") {
  LOAD_ROM(rom);
  Machine m(rom);
  m.powerOn();
  const BootRun r = boot(m, 5);
  CHECK(r.sawNd);
  CHECK_FALSE(r.sawUd);
  CHECK_FALSE(r.jumpedTo800E);
  CHECK(m.cpu().illegalCount() == 0);
}

// The Soundprocess sound packs carry the Soundprocess OS in tracks 0-10, so
// the ROM boots them like a factory disk (boot-rom.md 13).
TEST_CASE("boot: a Soundprocess sound pack boots its own OS", "[assets]") {
  LOAD_ROM(rom);
  const auto disk = firstSoundPack();
  REQUIRE_ASSET(disk);
  Machine m(rom);
  m.insertDisk(DiskImage::loadFile(disk));
  m.powerOn();
  BootRun r;
  REQUIRE_NOTHROW(r = boot(m, 10));
  CHECK(ranFor(r.cycles, 10 * kSecond));
  CHECK(r.jumpedTo800E);
  CHECK(r.ranFromRam);
  CHECK_FALSE(r.sawUd);
  CHECK(m.cpu().illegalCount() == 0);
  CHECK(m.fdc().sectorsRead() >= kBootSectors);
  m.runCycles(kSecond);
  CHECK(ranFor(m.cycles(), 11 * kSecond));
}

// A sound disk whose OS area (tracks 0-11) is zero-filled: every sector reads
// fine, so the ROM loads zeros and jumps to $800E (it never checks what it
// loaded, boot-rom.md 6). The zeros run as NEG <$00 with DP=$E8, i.e. on the
// FDC registers, and the CPU ends up in garbage. The host must keep running.
TEST_CASE("boot: a disk with a zero-filled OS area is jumped into; garbage runs on", "[assets]") {
  LOAD_ROM(rom);
  const auto src = assetPath(kSndA01);
  REQUIRE_ASSET(src);
  std::vector<uint8_t> bytes = loadFile(src);
  REQUIRE(bytes.size() == size_t(DiskImage::kImageBytes));
  std::fill(bytes.begin(), bytes.begin() + 12 * DiskImage::kTrackBytes, uint8_t(0));
  Machine m(rom);
  m.insertDisk(DiskImage::fromBytes(bytes));
  m.powerOn();
  BootRun r;
  REQUIRE_NOTHROW(r = boot(m, 10));
  CHECK(ranFor(r.cycles, 10 * kSecond));
  CHECK(r.jumpedTo800E);
  CHECK_FALSE(r.sawUd);
  CHECK(m.fdc().sectorsRead() >= kBootSectors);
  CHECK(m.cpu().illegalCount() > 0);  // counted, and execution went on
  m.runCycles(kSecond);
  CHECK(ranFor(m.cycles(), 11 * kSecond));
}

// Garbage disks (Review Focus 1): every sector reads, so the ROM loads noise
// and jumps into it. The host must keep running whatever the CPU does.
TEST_CASE("boot: a disk of LCG noise is jumped into and the CPU keeps executing", "[assets]") {
  LOAD_ROM(rom);
  std::vector<uint8_t> bytes(DiskImage::kImageBytes);
  uint32_t x = 12345;  // fixed seed: the run is deterministic
  for (uint8_t& b : bytes) {
    x = x * 1103515245u + 12345u;
    b = uint8_t(x >> 16);
  }
  Machine m(rom);
  m.insertDisk(DiskImage::fromBytes(bytes));
  m.powerOn();
  BootRun r;
  REQUIRE_NOTHROW(r = boot(m, 10));
  CHECK(ranFor(r.cycles, 10 * kSecond));
  CHECK(r.jumpedTo800E);
  CHECK(m.fdc().sectorsRead() >= kBootSectors);
  const uint64_t illegal = m.cpu().illegalCount();
  CHECK(illegal > 0);
  // Still genuinely running: during another second the CPU executes
  // instructions at many addresses and keeps counting illegal opcodes.
  std::vector<bool> seen(0x10000);
  REQUIRE_NOTHROW(m.setTrace([&seen](uint16_t pc, const Regs&) { seen[pc] = true; }));
  m.runCycles(kSecond);
  m.setTrace(nullptr);
  CHECK(ranFor(m.cycles(), 11 * kSecond));
  CHECK(std::count(seen.begin(), seen.end(), true) > 1);
  CHECK(m.cpu().illegalCount() > illegal);
}

// All $FF: $800E is STU $FFFF, repeated up to $DFFF and on into I/O and ROM;
// the CPU reaches the ROM's Read Sector primitive $F000 and waits in SYNC for a
// DRQ that never comes, because the motor is off (PB4=1 since the hand-off)
// and the ROM has no time-outs (boot-rom.md 5.1). Hardware would hang the same
// way; the host just keeps counting cycles.
TEST_CASE("boot: an all-$FF disk ends with the CPU parked in SYNC; the host runs on", "[assets]") {
  LOAD_ROM(rom);
  Machine m(rom);
  m.insertDisk(DiskImage::fromBytes(std::vector<uint8_t>(DiskImage::kImageBytes, 0xFF)));
  m.powerOn();
  BootRun r;
  REQUIRE_NOTHROW(r = boot(m, 10));
  CHECK(ranFor(r.cycles, 10 * kSecond));
  CHECK(r.jumpedTo800E);
  CHECK(r.ranFromRam);
  CHECK(m.cpu().illegalCount() > 0);  // met on the way; counted and skipped
  CHECK(m.cpu().waiting());
  CHECK(m.cpu().regs().pc == 0xF00D);  // after the SYNC at $F00C
  CHECK(m.fdc().busy());               // Read Sector waiting for rotation
  CHECK_FALSE(m.driveActive());
  m.runCycles(kSecond);
  CHECK(ranFor(m.cycles(), 11 * kSecond));
}

TEST_CASE("machine trace is called before each instruction with its PC", "[assets]") {
  LOAD_ROM(rom);
  Machine m(rom);
  m.powerOn();
  std::vector<uint16_t> pcs;
  m.setTrace([&pcs](uint16_t pc, const Regs& r) {
    CHECK(pc == r.pc);
    pcs.push_back(pc);
  });
  m.runCycles(2);  // CLRA (2 cycles)
  m.runCycles(1);  // LDX # (3 cycles)
  m.setTrace(nullptr);
  m.runCycles(100);
  // Reset entry $FC7F: CLRA, then LDX #$C000.
  REQUIRE(pcs.size() == 2);
  CHECK(pcs[0] == 0xFC7F);
  CHECK(pcs[1] == 0xFC80);
}

TEST_CASE("machine trace skips steps spent waiting in SYNC") {
  // $F000: LDS #$BF80; SYNC; NOP; BRA *. Nothing raises an interrupt, so the
  // CPU stays in SYNC.
  std::vector<uint8_t> rom(4096, 0x12);
  const uint8_t code[] = {0x10, 0xCE, 0xBF, 0x80, 0x13, 0x12, 0x20, 0xFE};
  std::copy(std::begin(code), std::end(code), rom.begin());
  rom[0xFFE] = 0xF0;
  rom[0xFFF] = 0x00;
  Machine m(rom);
  m.powerOn();
  std::vector<uint16_t> pcs;
  m.setTrace([&pcs](uint16_t pc, const Regs&) { pcs.push_back(pc); });
  m.runCycles(1000);
  REQUIRE(pcs.size() == 2);
  CHECK(pcs[0] == 0xF000);
  CHECK(pcs[1] == 0xF004);
}
