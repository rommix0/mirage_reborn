#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "asset_paths.h"
#include "mirage/machine.h"

namespace mirage {
// Test-only access to the private bank hook (VIA port B takes over in Task 6).
struct MachineTestAccess {
  static void setSampleBank(Machine& m, int quarter) { m.setSampleBank(quarter); }
};
}  // namespace mirage

using namespace mirage;

namespace {

// 4 KB ROM with `code` at $F000 and the reset vector pointing at it.
std::vector<uint8_t> romWith(std::initializer_list<uint8_t> code) {
  std::vector<uint8_t> rom(4096, 0x12);  // NOP fill
  size_t i = 0;
  for (uint8_t b : code) rom[i++] = b;
  rom[0xFFE] = 0xF0;
  rom[0xFFF] = 0x00;
  return rom;
}

std::vector<uint8_t> loadFile(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("machine rejects a boot ROM that is not 4096 bytes") {
  std::vector<uint8_t> small(4095), big(4097);
  CHECK_THROWS_AS(Machine(small), std::invalid_argument);
  CHECK_THROWS_AS(Machine(big), std::invalid_argument);
  CHECK_NOTHROW(Machine(std::vector<uint8_t>(4096)));
}

TEST_CASE("machine reset vector comes from the ROM") {
  auto rom = romWith({});
  Machine m(rom);
  m.powerOn();
  CHECK(m.cpu().regs().pc == 0xF000);
  CHECK(m.peek(0xF000) == 0x12);
  CHECK(m.peek(0xFFFE) == 0xF0);
}

TEST_CASE("machine boots the real ROM to $FC7F", "[assets]") {
  auto p = assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin");
  REQUIRE_ASSET(p);
  auto rom = loadFile(p);
  REQUIRE(rom.size() == 4096);
  Machine m(rom);
  m.powerOn();
  CHECK(m.cpu().regs().pc == 0xFC7F);
}

TEST_CASE("machine main and expansion RAM read back") {
  // LDA #$5A; STA $8123; LDA #$A5; STA $C000; LDA #$11; STA $DFFF
  auto rom = romWith({0x86, 0x5A, 0xB7, 0x81, 0x23, 0x86, 0xA5, 0xB7, 0xC0, 0x00,
                      0x86, 0x11, 0xB7, 0xDF, 0xFF});
  Machine m(rom);
  m.powerOn();
  m.runCycles(30);
  CHECK(m.peek(0x8123) == 0x5A);
  CHECK(m.peek(0xC000) == 0xA5);
  CHECK(m.peek(0xDFFF) == 0x11);
  CHECK(m.peek(0x8124) == 0x00);
}

TEST_CASE("machine ROM is read-only") {
  // LDA #$55; STA $F000; STA $FFFE
  auto rom = romWith({0x86, 0x55, 0xB7, 0xF0, 0x00, 0xB7, 0xFF, 0xFE});
  Machine m(rom);
  m.powerOn();
  m.runCycles(20);
  CHECK(m.peek(0xF000) == 0x86);
  CHECK(m.peek(0xFFFE) == 0xF0);
}

TEST_CASE("machine sample window follows the bank") {
  // LDA #$77; STA $0005
  auto rom = romWith({0x86, 0x77, 0xB7, 0x00, 0x05});
  Machine m(rom);
  m.powerOn();
  MachineTestAccess::setSampleBank(m, 2);
  m.runCycles(10);
  CHECK(m.sampleRam(0x10005) == 0x77);
  CHECK(m.sampleRam(0x00005) == 0x00);
  CHECK(m.sampleRam(0x08005) == 0x00);
  CHECK(m.sampleRam(0x18005) == 0x00);
  CHECK(m.peek(0x0005) == 0x77);  // window reads the selected quarter
  MachineTestAccess::setSampleBank(m, 0);
  CHECK(m.peek(0x0005) == 0x00);
  MachineTestAccess::setSampleBank(m, 3);
  CHECK(m.peek(0x7FFF) == 0x00);
  CHECK(m.sampleRam(0x1FFFF) == 0x00);
}

TEST_CASE("machine I/O space reads $FF and ignores writes for now") {
  // LDA #$00; STA $E200; STA $E050; LDA $E200; STA $8000; LDA $E050; STA $8001
  auto rom = romWith({0x86, 0x00, 0xB7, 0xE2, 0x00, 0xB7, 0xE0, 0x50, 0xB6, 0xE2, 0x00,
                      0xB7, 0x80, 0x00, 0xB6, 0xE0, 0x50, 0xB7, 0x80, 0x01});
  Machine m(rom);
  m.powerOn();
  m.runCycles(40);
  CHECK(m.peek(0x8000) == 0xFF);  // VIA range
  CHECK(m.peek(0x8001) == 0xFF);  // unmapped $E000-$EFFF
  for (uint16_t a : std::initializer_list<uint16_t>{0xE000, 0xE100, 0xE200, 0xE41F, 0xE800, 0xEC00, 0xECEF, 0xEFFF})
    CHECK(m.peek(a) == 0xFF);
}

TEST_CASE("machine runCycles keeps an exact cycle total") {
  auto rom = romWith({});  // all NOPs, 2 cycles each
  Machine m(rom);
  m.powerOn();
  CHECK(m.cycles() == 0);
  m.runCycles(10);
  CHECK(m.cycles() == 10);
  m.runCycles(1);  // one NOP: 2 cycles, 1 cycle of credit carried
  CHECK(m.cycles() == 12);
  m.runCycles(1);  // paid from the credit
  CHECK(m.cycles() == 12);
  m.runCycles(1);
  CHECK(m.cycles() == 14);
}

TEST_CASE("machine powerOn clears RAM, sample RAM and bank") {
  // LDA #$5A; STA $8000; STA $0000
  auto rom = romWith({0x86, 0x5A, 0xB7, 0x80, 0x00, 0xB7, 0x00, 0x00});
  Machine m(rom);
  m.powerOn();
  MachineTestAccess::setSampleBank(m, 1);
  m.runCycles(20);
  REQUIRE(m.peek(0x8000) == 0x5A);
  REQUIRE(m.sampleRam(0x8000) == 0x5A);
  m.powerOn();
  CHECK(m.peek(0x8000) == 0x00);
  CHECK(m.sampleRam(0x8000) == 0x00);
  CHECK(m.cycles() == 0);
  CHECK(m.cpu().regs().pc == 0xF000);
}

TEST_CASE("machine save/load round-trips RAM, sample RAM and CPU") {
  // LDA #$5A; STA $8123; STA $0010; then NOPs
  auto rom = romWith({0x86, 0x5A, 0xB7, 0x81, 0x23, 0xB7, 0x00, 0x10});
  Machine m(rom);
  m.powerOn();
  MachineTestAccess::setSampleBank(m, 3);
  m.runCycles(20);
  const auto snap = m.saveState();
  const uint64_t cyc = m.cycles();
  const Regs regs = m.cpu().regs();

  Machine other(rom);
  other.powerOn();
  other.loadState(snap);
  CHECK(other.peek(0x8123) == 0x5A);
  CHECK(other.sampleRam(0x18010) == 0x5A);
  CHECK(other.peek(0x0010) == 0x5A);  // bank restored
  CHECK(other.cycles() == cyc);
  CHECK(other.cpu().regs().pc == regs.pc);
  CHECK(other.cpu().regs().a == regs.a);
  CHECK(other.saveState() == snap);

  // Both machines must now evolve identically.
  m.runCycles(50);
  other.runCycles(50);
  CHECK(other.saveState() == m.saveState());
}

TEST_CASE("machine loadState leaves the machine unchanged on error") {
  auto rom = romWith({0x86, 0x5A, 0xB7, 0x81, 0x23});
  Machine src(rom);
  src.powerOn();
  src.runCycles(10);
  const auto snap = src.saveState();

  Machine m(rom);
  m.powerOn();
  const auto before = m.saveState();

  auto truncated = std::vector<uint8_t>(snap.begin(), snap.end() - 5);
  CHECK_THROWS_AS(m.loadState(truncated), StateError);
  CHECK(m.saveState() == before);

  auto trailing = snap;
  trailing.push_back(0);
  CHECK_THROWS_AS(m.loadState(trailing), StateError);
  CHECK(m.saveState() == before);

  auto badTag = snap;
  badTag[0] ^= 0xFF;
  CHECK_THROWS_AS(m.loadState(badTag), StateError);
  CHECK(m.saveState() == before);

  auto badBank = snap;
  badBank[4] = 9;
  CHECK_THROWS_AS(m.loadState(badBank), StateError);
  CHECK(m.saveState() == before);
}
