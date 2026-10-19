#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "asset_paths.h"
#include "mirage/clocks.h"
#include "mirage/machine.h"

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

// LDA #v; STA addr  (7 E-cycles)
std::vector<uint8_t> ldaSta(uint8_t v, uint16_t addr) {
  return {0x86, v, 0xB7, uint8_t(addr >> 8), uint8_t(addr & 0xFF)};
}

// ROM image: `parts` concatenated at $F000, reset vector at $F000.
std::vector<uint8_t> romFrom(std::initializer_list<std::vector<uint8_t>> parts) {
  std::vector<uint8_t> rom(4096, 0x12);
  size_t i = 0;
  for (const auto& p : parts)
    for (uint8_t b : p) rom[i++] = b;
  rom[0xFFE] = 0xF0;
  rom[0xFFF] = 0x00;
  return rom;
}

// VIA port B: DDRB = $1F (the boot ROM's value), ORB = quarter. 14 E-cycles.
std::vector<uint8_t> selectBank(uint8_t quarter) {
  auto a = ldaSta(0x1F, 0xE202);
  auto b = ldaSta(quarter, 0xE200);
  a.insert(a.end(), b.begin(), b.end());
  return a;
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

TEST_CASE("machine sample window follows VIA PB1:PB0") {
  Machine m(romFrom({selectBank(2), ldaSta(0x77, 0x0005), ldaSta(0x00, 0xE200), ldaSta(0x03, 0xE200)}));
  m.powerOn();
  CHECK(m.peek(0x0005) == 0x00);
  m.runCycles(14 + 7);  // bank 2, store through the window
  REQUIRE(m.cycles() == 21);
  CHECK(m.sampleRam(0x10005) == 0x77);
  CHECK(m.sampleRam(0x00005) == 0x00);
  CHECK(m.sampleRam(0x08005) == 0x00);
  CHECK(m.sampleRam(0x18005) == 0x00);
  CHECK(m.peek(0x0005) == 0x77);  // window reads the selected quarter
  m.runCycles(7);                 // ORB = 0: quarter 0
  CHECK(m.peek(0x0005) == 0x00);
  m.runCycles(7);                 // ORB = 3: quarter 3
  CHECK(m.peek(0x7FFF) == 0x00);
  CHECK(m.sampleRam(0x1FFFF) == 0x00);
}

TEST_CASE("machine bank 1 maps $0000 to the second quarter") {
  Machine m(romFrom({selectBank(1), ldaSta(0x42, 0x0000)}));
  m.powerOn();
  m.runCycles(21);
  CHECK(m.sampleRam(0x08000) == 0x42);
  CHECK(m.sampleRam(0x00000) == 0x00);
}

TEST_CASE("machine bank uses only PB1:PB0 of port B") {
  Machine m(romFrom({selectBank(0x1E), ldaSta(0x42, 0x0000)}));  // PB4-PB1 high, PB0 low
  m.powerOn();
  m.runCycles(21);
  CHECK(m.sampleRam(0x10000) == 0x42);  // quarter 2
}

TEST_CASE("machine unmapped and write-only I/O reads $FF") {
  // LDA #$00; STA $E050; LDA $E050; STA $8000; STA $E400; LDA $E400; STA $8001
  auto rom = romWith({0x86, 0x00, 0xB7, 0xE0, 0x50, 0xB6, 0xE0, 0x50, 0xB7, 0x80, 0x00,
                      0xB7, 0xE4, 0x00, 0xB6, 0xE4, 0x00, 0xB7, 0x80, 0x01});
  Machine m(rom);
  m.powerOn();
  m.runCycles(40);
  CHECK(m.peek(0x8000) == 0xFF);  // unmapped $E000-$EFFF
  CHECK(m.peek(0x8001) == 0xFF);  // filter CV latches are write only
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
  // bank 1; LDA #$5A; STA $8000; STA $0000
  auto rom = romFrom({selectBank(1), {0x86, 0x5A, 0xB7, 0x80, 0x00, 0xB7, 0x00, 0x00}});
  Machine m(rom);
  m.powerOn();
  m.runCycles(40);
  REQUIRE(m.peek(0x8000) == 0x5A);
  REQUIRE(m.sampleRam(0x8000) == 0x5A);
  m.powerOn();
  CHECK(m.peek(0x8000) == 0x00);
  CHECK(m.sampleRam(0x8000) == 0x00);
  CHECK(m.cycles() == 0);
  CHECK(m.cpu().regs().pc == 0xF000);
}

TEST_CASE("machine save/load round-trips RAM, sample RAM and CPU") {
  // bank 3; T1 free-running with PB7; LDA #$5A; STA $8123; STA $0010; NOPs.
  // The VIA timers, the panel and a held key are all part of the snapshot.
  auto rom = romFrom({selectBank(3), ldaSta(0xC0, 0xE20B), ldaSta(0x03, 0xE205),
                      {0x86, 0x5A, 0xB7, 0x81, 0x23, 0xB7, 0x00, 0x10}});
  Machine m(rom);
  m.powerOn();
  m.setKey(PanelKey::Enter, true);
  m.runCycles(60);
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
  const std::array<uint8_t, 4> mchnTag = {'N', 'H', 'C', 'M'};  // fourcc("MCHN") as a LE u32
  const auto mchnAt =
      size_t(std::search(snap.begin(), snap.end(), mchnTag.begin(), mchnTag.end()) - snap.begin());
  REQUIRE(mchnAt < snap.size());
  badBank[mchnAt + 4] = 9;  // the bank byte follows the tag
  CHECK_THROWS_AS(m.loadState(badBank), StateError);
  CHECK(m.saveState() == before);

  // A corrupt panel section (last in the stream, after the CPU and VIA have
  // loaded) must not leave anything half-loaded.
  constexpr size_t kPanelBytes = 4 + 4 + 2 + 2 * 8 * 8;  // tag, keys, valid, lit times
  auto badPanelTag = snap;
  badPanelTag[badPanelTag.size() - kPanelBytes] ^= 0xFF;
  CHECK_THROWS_AS(m.loadState(badPanelTag), StateError);
  CHECK(m.saveState() == before);

  // The DOC section follows the VIA: tag, OER, clock, slot, last OIR,
  // 32 x 11 oscillator bytes, 8 channel sums, FIFO count (0 here), the A/D
  // (clock u64, start u64, result, sample, busy), then the machine's DOC clock
  // remainder (u32) and the filter bank.
  const std::array<uint8_t, 4> docTag = {'5', 'C', 'O', 'D'};  // fourcc("DOC5") as a LE u32
  const auto docAt =
      size_t(std::search(snap.begin(), snap.end(), docTag.begin(), docTag.end()) - snap.begin());
  REQUIRE(docAt < snap.size());
  const size_t fifoCountAt = docAt + 4 + 4 + 32 * 11 + 8 * 4;
  REQUIRE(snap[fifoCountAt] == 0);
  auto badDocFifo = snap;
  badDocFifo[fifoCountAt] = 0xFF;  // FIFO count > 32
  CHECK_THROWS_AS(m.loadState(badDocFifo), StateError);
  CHECK(m.saveState() == before);

  auto badDocAdc = snap;
  badDocAdc[fifoCountAt + 19] = 0xFF;  // A/D busy flag > 1
  CHECK_THROWS_AS(m.loadState(badDocAdc), StateError);
  CHECK(m.saveState() == before);

  auto badDocClock = snap;
  badDocClock[fifoCountAt + 23] = 0xFF;  // remainder high byte: >= kCpuClockHz
  CHECK_THROWS_AS(m.loadState(badDocClock), StateError);
  CHECK(m.saveState() == before);

  const size_t filtersAt = fifoCountAt + 24;
  REQUIRE(snap[filtersAt] == 'K');  // fourcc("FBNK") as a LE u32
  auto badFilters = snap;
  badFilters[filtersAt] ^= 0xFF;
  CHECK_THROWS_AS(m.loadState(badFilters), StateError);
  CHECK(m.saveState() == before);
}

TEST_CASE("machine powerOn is equivalent to a fresh power-on") {
  // $01 is an illegal opcode; then STA $8000, VIA setup (bank 2, T1 running), NOPs.
  auto rom = romFrom({{0x01, 0x86, 0x5A, 0xB7, 0x80, 0x00}, selectBank(2), ldaSta(0x00, 0xE205)});
  Machine fresh(rom);
  fresh.powerOn();
  const auto clean = fresh.saveState();

  Machine m(rom);
  m.powerOn();
  m.cpu().setIrq(true);
  m.cpu().setFirq(true);
  m.cpu().setNmi(true);
  m.runCycles(60);
  REQUIRE(m.cpu().illegalCount() > 0);
  REQUIRE(m.saveState() != clean);
  m.powerOn();
  CHECK(m.cpu().illegalCount() == 0);
  CHECK(m.saveState() == clean);
}

TEST_CASE("machine VIA T2 time-out interrupts the CPU through the IRQ line") {
  // $F000: IER = T2 enable; T2 = 16 cycles; CLI; spin.   $F100 (IRQ handler): mark; spin.
  auto rom = romFrom({ldaSta(0xA0, 0xE20E), ldaSta(0x10, 0xE208), ldaSta(0x00, 0xE209),
                      {0x1C, 0xEF, 0x20, 0xFE}});
  const auto handler = ldaSta(0x5A, 0x8000);
  size_t i = 0x100;
  for (uint8_t b : handler) rom[i++] = b;
  rom[i++] = 0x20;  // BRA *
  rom[i++] = 0xFE;
  rom[0xFF8] = 0xF1;  // IRQ vector -> $F100
  rom[0xFF9] = 0x00;
  Machine m(rom);
  m.powerOn();
  m.runCycles(21 + 4);  // set-up is done, T2 still running
  CHECK(m.peek(0x8000) == 0x00);
  m.runCycles(100);
  CHECK(m.peek(0x8000) == 0x5A);  // the handler ran
}

TEST_CASE("machine port A reads the keypad for the column selected on PA0-2") {
  // DDRA = $1F; ORA = column 7; read ORA into $8000; column 0 into $8001.
  auto rom = romFrom({ldaSta(0x1F, 0xE203), ldaSta(0x07, 0xE201),
                      {0xB6, 0xE2, 0x01, 0xB7, 0x80, 0x00},
                      ldaSta(0x00, 0xE201),
                      {0xB6, 0xE2, 0x01, 0xB7, 0x80, 0x01}});
  Machine m(rom);
  m.powerOn();
  m.setKey(PanelKey::Enter, true);  // row 1, column 7
  m.runCycles(60);
  CHECK(m.peek(0x8000) == 0xA7);  // PA6 low; PA5/PA7 high; PA0-2 = 7
  CHECK(m.peek(0x8001) == 0xE0);  // column 0: nothing down
}

TEST_CASE("machine port A drives the front-panel display") {
  Machine m(romFrom({ldaSta(0x1F, 0xE203), ldaSta(0x13, 0xE201)}));  // left on, segment d
  m.powerOn();
  m.runCycles(14);
  // Making PA0-4 outputs with ORA = 0 first lit segment a on both digits (the
  // behaviour boot-rom.md section 8 describes); the ORA write adds segment d
  // on the left digit only.
  CHECK(m.display().left == 0x09);
  CHECK(m.display().right == 0x01);
  m.runCycles(2 * 40000);  // 40 ms of NOPs
  CHECK(m.display().left == 0x00);
  CHECK(m.display().right == 0x00);
}

TEST_CASE("machine PB6 reads 0 (no disk) and DDRB masks port B reads") {
  // DDRB = $1F; ORB = $1F; read ORB -> $8000
  Machine m(romFrom({ldaSta(0x1F, 0xE202), ldaSta(0x1F, 0xE200), {0xB6, 0xE2, 0x00, 0xB7, 0x80, 0x00}}));
  m.powerOn();
  m.runCycles(40);
  CHECK(m.peek(0x8000) == 0x1F);  // PB5-7 inputs read 0
}

TEST_CASE("machine boot ROM fills sample RAM through the VIA bank bits", "[assets]") {
  auto p = assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin");
  REQUIRE_ASSET(p);
  auto rom = loadFile(p);
  Machine m(rom);
  m.powerOn();
  m.runCycles(2'500'000);  // past the $F1E5 fill, then waiting on the FDC stub
  // boot-rom.md section 5.5: the fill ORs the quarter number into the previous
  // ORB value, so quarters 0, 1, 3, 3 are written and quarter 2 is never filled.
  for (uint32_t base : {0x00000u, 0x08000u, 0x18000u}) {
    CHECK(m.sampleRam(base) == 0x7F);
    CHECK(m.sampleRam(base + 0x7FFF) == 0x7F);
  }
  CHECK(m.sampleRam(0x10000) == 0x00);
  CHECK(m.sampleRam(0x17FFF) == 0x00);
}

// DOC 5503 at $EC00-$ECEF (Task 10).

TEST_CASE("machine DOC registers are mapped at $EC00-$ECEF") {
  // LDA #$5A; STA $EC85; LDA $EC85; STA $8000; LDA $ECE1; STA $8001
  auto rom = romFrom({ldaSta(0x5A, 0xEC85),
                      {0xB6, 0xEC, 0x85, 0xB7, 0x80, 0x00, 0xB6, 0xEC, 0xE1, 0xB7, 0x80, 0x01}});
  Machine m(rom);
  m.powerOn();
  m.runCycles(30);
  CHECK(m.peek(0x8000) == 0x5A);  // wavetable pointer of oscillator 5
  CHECK(m.peek(0x8001) == 0xC1);  // OER after reset: X bits read 1
  CHECK(m.debugRead(0xEC85) == 0x5A);
  CHECK(m.peek(0xEC85) == 0xFF);  // peek has no I/O side effects
}

TEST_CASE("machine DOC reads sample RAM; BS selects the upper 64 KB") {
  // Sample RAM $00000 = $77 (quarter 0), $10000 = $99 (quarter 2). Oscillator 0:
  // pointer 0, FC 1 (stays on byte 0 for 511 scans), BS = 1, running.
  auto rom = romFrom({selectBank(0), ldaSta(0x77, 0x0000), selectBank(2), ldaSta(0x99, 0x0000),
                      ldaSta(0x01, 0xEC00), ldaSta(0x40, 0xECC0), ldaSta(0x00, 0xECA0)});
  Machine m(rom);
  m.powerOn();
  m.runCycles(100);
  CHECK(m.debugRead(0xEC60) == 0x99);
  m.debugWrite(0xECC0, 0x00);  // BS = 0
  m.runCycles(20);
  CHECK(m.debugRead(0xEC60) == 0x77);
}

TEST_CASE("machine DOC interrupt reaches the CPU IRQ line; the handler reads the OIR") {
  // OER = $00: 1 oscillator; oscillator 0 IE, free run, pointer 0: sample RAM is
  // all 0 after powerOn, so the first byte halts it and interrupts. CLI; spin.
  // IRQ handler: LDA $ECE0; STA $8000; LDA $ECE0; STA $8001; spin.
  auto rom = romFrom({ldaSta(0x00, 0xECE1), ldaSta(0x08, 0xECA0), {0x1C, 0xEF, 0x20, 0xFE}});
  const std::vector<uint8_t> handler = {0xB6, 0xEC, 0xE0, 0xB7, 0x80, 0x00,
                                        0xB6, 0xEC, 0xE0, 0xB7, 0x80, 0x01, 0x20, 0xFE};
  std::copy(handler.begin(), handler.end(), rom.begin() + 0x100);
  rom[0xFF8] = 0xF1;  // IRQ vector -> $F100
  rom[0xFF9] = 0x00;
  Machine m(rom);
  m.powerOn();
  m.runCycles(200);
  CHECK(m.peek(0x8000) == 0x41);  // osc 0 pending: bit 7 = 0
  CHECK(m.peek(0x8001) == 0xC1);  // FIFO empty: bit 7 = 1
  CHECK_FALSE(m.doc().irq());
}

TEST_CASE("machine DOC runs one slot per 8 master clocks (2 E cycles)") {
  Machine m(romWith({0x20, 0xFE}));  // BRA *
  m.powerOn();
  uint64_t frames = 0;
  m.onMix = [&frames](float) { ++frames; };
  m.runCycles(60001);
  // Reset default: 1 oscillator + 2 refresh slots = 24 master clocks = 6 E cycles.
  CHECK(frames == m.cycles() / 6);
  CHECK(m.doc().sampleRate() == 8e6 / 24);
}

TEST_CASE("machine save/load includes the DOC") {
  auto rom = romFrom({ldaSta(0x3E, 0xECE1), ldaSta(0x5A, 0xEC85), ldaSta(0x13, 0xEC00),
                      ldaSta(0x10, 0xECA0)});
  Machine m(rom);
  m.powerOn();
  m.runCycles(41);
  const auto snap = m.saveState();

  Machine other(rom);
  other.powerOn();
  other.loadState(snap);
  CHECK(other.debugRead(0xEC85) == 0x5A);
  CHECK(other.doc().enabledOscillators() == 32);
  CHECK(other.saveState() == snap);
  m.runCycles(1001);
  other.runCycles(1001);
  CHECK(other.saveState() == m.saveState());
}

// ---- Filters, CV path and A/D mux (Task 11, docs/hardware/filters.md) --------

namespace {
// Code that reads the DOC A/D the way the OS does ($B935): a read to start a
// conversion, a delay (MUL x3), then a read for the result, stored at `dst`.
std::vector<uint8_t> adcReadTo(uint16_t dst) {
  return {0xB6, 0xEC, 0xE2, 0x3D, 0x3D, 0x3D, 0xB6, 0xEC, 0xE2,
          0xB7, uint8_t(dst >> 8), uint8_t(dst & 0xFF)};
}
}  // namespace

TEST_CASE("machine $E400-$E41F writes reach the filter CV path") {
  // LDA #$40; STA $E41B (preset); STA $E413 (cutoff 3); LDA #$C8; STA $E41E;
  // STA $E40E (resonance 6); STA $E401 (both of channel 1).
  auto rom = romFrom({ldaSta(0x40, 0xE41B), {0xB7, 0xE4, 0x13}, ldaSta(0xC8, 0xE41E),
                      {0xB7, 0xE4, 0x0E}, {0xB7, 0xE4, 0x01}});
  Machine m(rom);
  m.powerOn();
  m.runCycles(40);
  CHECK(m.filters().cutoffCv(3) == 0x40);
  CHECK(m.filters().resonanceCv(6) == 0xC8);
  CHECK(m.filters().cutoffCv(1) == 0xC8);
  CHECK(m.filters().resonanceCv(1) == 0xC8);
  CHECK(m.filters().resonanceCv(3) == 0x00);
  CHECK(m.peek(0xE413) == 0xFF);
}

TEST_CASE("machine A/D reads the input selected by VIA PB3:PB2") {
  auto rom = romFrom({ldaSta(0x1F, 0xE202),
                      ldaSta(0x08, 0xE200), adcReadTo(0x8000),   // 2: pitch wheel
                      ldaSta(0x0C, 0xE200), adcReadTo(0x8001),   // 3: mod wheel
                      ldaSta(0x04, 0xE200), adcReadTo(0x8002),   // 1: line input
                      ldaSta(0x00, 0xE200), adcReadTo(0x8003),   // 0: compressed (silent voices)
                      {0x20, 0xFE}});
  Machine m(rom);
  m.powerOn();
  m.runCycles(2000);
  CHECK(m.peek(0x8000) == kPitchWheelRest);
  CHECK(m.peek(0x8001) == kModWheelRest);
  CHECK(m.peek(0x8002) == 0x80);
  CHECK(m.peek(0x8003) == 0x80);
}

TEST_CASE("machine A/D mux 0 sees a self-oscillating filter swing across $90 and $70") {
  // Channel 0: resonance $FF, cutoff $7A ($F5DC's sequence), mux 0, OER $FF
  // (32 oscillators, as the boot ROM leaves it); wait 0.26 s
  // (LDY #$FFFF; LEAY -1,Y; BNE), then 256 A/D reads to $8000-$80FF.
  auto rom = romFrom({ldaSta(0xFF, 0xECE1), ldaSta(0x1F, 0xE202), ldaSta(0x00, 0xE200),
                      ldaSta(0xFF, 0xE418), {0xB7, 0xE4, 0x08}, ldaSta(0x7A, 0xE418),
                      {0xB7, 0xE4, 0x10},
                      {0x10, 0x8E, 0xFF, 0xFF, 0x31, 0x3F, 0x26, 0xFC},
                      // LDX #$8000; LDA $ECE2; STA ,X+; CMPX #$8100; BNE; BRA *
                      {0x8E, 0x80, 0x00, 0xB6, 0xEC, 0xE2, 0xA7, 0x80, 0x8C, 0x81, 0x00, 0x26,
                       0xF6, 0x20, 0xFE}});
  Machine m(rom);
  m.powerOn();
  m.runCycles(kCpuClockHz / 2);
  CHECK(m.filters().resonanceCv(0) == 0xFF);
  CHECK(m.filters().cutoffCv(0) == 0x7A);
  int high = 0, low = 0;
  for (uint16_t a = 0x8001; a < 0x8100; ++a) {  // $8000 holds the conversion before the loop
    if (m.peek(a) > 0x90) ++high;
    if (m.peek(a) < 0x70) ++low;
  }
  CHECK(high > 20);
  CHECK(low > 20);
}

TEST_CASE("machine save/load includes the filter bank and the A/D") {
  auto rom = romFrom({ldaSta(0xFF, 0xECE1), ldaSta(0xFF, 0xE41A), {0xB7, 0xE4, 0x0A},
                      ldaSta(0x90, 0xE41A), {0xB7, 0xE4, 0x12}, {0xB6, 0xEC, 0xE2},
                      {0x20, 0xFE}});
  Machine m(rom);
  m.powerOn();
  m.runCycles(20001);
  const auto snap = m.saveState();
  Machine other(rom);
  other.powerOn();
  other.loadState(snap);
  CHECK(other.filters().cutoffCv(2) == 0x90);
  CHECK(other.filters().resonanceCv(2) == 0xFF);
  CHECK(other.filters().mix() == m.filters().mix());
  CHECK(other.saveState() == snap);
  m.runCycles(30001);
  other.runCycles(30001);
  CHECK(other.filters().mix() == m.filters().mix());
  CHECK(other.saveState() == m.saveState());
}
