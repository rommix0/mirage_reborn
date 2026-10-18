#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "mirage/acia6850.h"
#include "mirage/clocks.h"
#include "mirage/machine.h"
#include "mirage/state_io.h"

using namespace mirage;

namespace {

constexpr uint8_t kRdrf = 0x01, kTdre = 0x02, kOvrn = 0x20, kIrq = 0x80;

// The boot ROM's hand-off setting: divide-by-16, 8N1, RTS low / TX IRQ off,
// RX IRQ on ($F152).
constexpr uint8_t kRomControl = 0x95;

void romSetup(Acia6850& a) {
  a.write(0, 0x03);
  a.write(0, kRomControl);
}

// 4 KB ROM: `code` at $F000, reset vector -> $F000, FIRQ vector -> `firq`.
std::vector<uint8_t> rom(const std::vector<uint8_t>& code, uint16_t firq = 0xF100,
                         const std::vector<uint8_t>& handler = {}) {
  std::vector<uint8_t> r(4096, 0x12);
  for (size_t i = 0; i < code.size(); ++i) r[i] = code[i];
  for (size_t i = 0; i < handler.size(); ++i) r[(firq & 0xFFF) + i] = handler[i];
  r[0xFFE] = 0xF0;
  r[0xFFF] = 0x00;
  r[0xFF6] = uint8_t(firq >> 8);
  r[0xFF7] = uint8_t(firq);
  return r;
}

// Polls the ACIA status; records any OVRN sighting at $9001 and stores each
// received byte at $8000,X+. ROM-style setup first ($03 reset, $95).
const std::vector<uint8_t> kReceiver = {
    0x86, 0x03, 0xB7, 0xE1, 0x00,  // LDA #$03; STA $E100
    0x86, 0x95, 0xB7, 0xE1, 0x00,  // LDA #$95; STA $E100
    0x8E, 0x80, 0x00,              // LDX #$8000
    0xB6, 0xE1, 0x00,              // loop: LDA $E100
    0x85, 0x20,                    // BITA #$20
    0x27, 0x03,                    // BEQ noov
    0xB7, 0x90, 0x01,              // STA $9001
    0x85, 0x01,                    // noov: BITA #$01
    0x27, 0xF2,                    // BEQ loop
    0xB6, 0xE1, 0x01,              // LDA $E101
    0xA7, 0x80,                    // STA ,X+
    0x20, 0xEB,                    // BRA loop
};

}  // namespace

TEST_CASE("acia master reset then ROM setup reads TDRE only") {
  Acia6850 a;
  romSetup(a);
  CHECK(a.read(0) == kTdre);  // RDRF=0, TDRE=1, DCD/CTS tied active (0), IRQ=0
  CHECK_FALSE(a.irq());
}

TEST_CASE("acia receive sets RDRF and IRQ, data read clears both") {
  Acia6850 a;
  romSetup(a);
  a.receiveByte(0xF8);
  CHECK(a.irq());
  CHECK(a.read(0) == (kRdrf | kTdre | kIrq));
  CHECK(a.read(1) == 0xF8);
  CHECK(a.read(0) == kTdre);
  CHECK_FALSE(a.irq());
}

TEST_CASE("acia receive without RX IRQ enabled sets RDRF but not IRQ") {
  Acia6850 a;
  a.write(0, 0x03);
  a.write(0, 0x15);  // CR7 = 0
  a.receiveByte(0x90);
  CHECK(a.read(0) == (kRdrf | kTdre));
  CHECK_FALSE(a.irq());
}

TEST_CASE("acia overrun keeps the first byte and flags OVRN") {
  Acia6850 a;
  romSetup(a);
  a.receiveByte(0x11);
  a.receiveByte(0x22);
  CHECK((a.read(0) & kOvrn) == kOvrn);
  CHECK(a.irq());
  CHECK(a.read(1) == 0x11);
  CHECK(a.read(0) == kTdre);  // read clears RDRF and OVRN
  CHECK_FALSE(a.irq());
}

TEST_CASE("acia transmit calls onTransmit and TDRE stays set") {
  Acia6850 a;
  romSetup(a);
  std::vector<uint8_t> sent;
  a.onTransmit = [&](uint8_t b) { sent.push_back(b); };
  a.write(1, 0x90);
  a.write(1, 0x40);
  CHECK(sent == std::vector<uint8_t>{0x90, 0x40});
  CHECK((a.read(0) & kTdre) == kTdre);
}

TEST_CASE("acia TX interrupt asserts while TDRE and CR6:5 = 01") {
  Acia6850 a;
  a.write(0, 0x03);
  a.write(0, 0x35);  // CR6:5 = 01, RX IRQ off
  CHECK(a.irq());
  CHECK(a.read(0) == (kTdre | kIrq));
  a.write(0, 0x15);  // TX IRQ off
  CHECK_FALSE(a.irq());
}

TEST_CASE("acia master reset clears receive state") {
  Acia6850 a;
  romSetup(a);
  a.receiveByte(1);
  a.receiveByte(2);
  a.write(0, 0x03);
  CHECK(a.read(0) == kTdre);
  a.write(0, kRomControl);
  CHECK_FALSE(a.irq());
}

TEST_CASE("acia state round-trips and rejects bad state") {
  Acia6850 a;
  romSetup(a);
  a.receiveByte(0x5A);
  a.receiveByte(0x5B);
  StateWriter w;
  a.save(w);
  const auto blob = w.take();
  Acia6850 b;
  StateReader r(blob);
  b.load(r);
  CHECK(r.atEnd());
  CHECK(b.read(0) == a.read(0));
  CHECK(b.irq());
  CHECK(b.read(1) == 0x5A);

  auto bad = blob;
  bad[bad.size() - 1] = 7;  // flag byte out of range
  Acia6850 c;
  StateReader rb(bad);
  CHECK_THROWS_AS(c.load(rb), StateError);
}

TEST_CASE("machine paces 1000 MIDI bytes at one per 320 us with no loss") {
  Machine m(rom(kReceiver));
  m.powerOn();
  std::vector<uint8_t> in(1000);
  for (size_t i = 0; i < in.size(); ++i) in[i] = uint8_t(i * 7 + 3);
  in[10] = 0xF8;
  in[11] = 0xFE;
  in[500] = 0xF8;
  in[501] = 0xFE;
  in[999] = 0xFE;  // non-zero sentinel for completion
  m.queueMidi(in);

  const double byteCycles = kMidiByteSeconds * kCpuClockHz;
  CHECK(byteCycles == 640.0);
  const uint64_t limit = 2 * 1000 * uint64_t(byteCycles);
  while (m.peek(0x8000 + 999) == 0 && m.cycles() < limit) m.runCycles(8);

  for (size_t i = 0; i < in.size(); ++i) {
    REQUIRE(m.peek(uint16_t(0x8000 + i)) == in[i]);
  }
  CHECK(m.peek(0x9001) == 0);  // never saw OVRN

  const double elapsed = double(m.cycles());
  CHECK(std::abs(elapsed - 1000 * byteCycles) <= byteCycles);
}

TEST_CASE("machine MIDI input is deterministic across save and load") {
  std::vector<uint8_t> in(40);
  for (size_t i = 0; i < in.size(); ++i) in[i] = uint8_t(0x80 + i);
  in[39] = 0xFE;
  Machine a(rom(kReceiver)), b(rom(kReceiver));
  a.powerOn();
  a.queueMidi(in);
  a.runCycles(10'000);  // part-way: some bytes delivered, the rest queued
  REQUIRE(a.peek(0x8000 + 39) == 0);
  REQUIRE(a.peek(0x8000) == in[0]);
  b.powerOn();
  b.loadState(a.saveState());
  a.runCycles(20'000);
  b.runCycles(20'000);
  for (uint16_t i = 0; i < 40; ++i) CHECK(a.peek(uint16_t(0x8000 + i)) == in[i]);
  CHECK(a.saveState() == b.saveState());
}

TEST_CASE("machine MIDI output reaches onMidiOut immediately") {
  // reset ACIA, then LDA #$90; STA $E101; LDA #$40; STA $E101
  Machine m(rom({0x86, 0x03, 0xB7, 0xE1, 0x00, 0x86, 0x90, 0xB7, 0xE1, 0x01,
                 0x86, 0x40, 0xB7, 0xE1, 0x01}));
  std::vector<uint8_t> out;
  m.onMidiOut = [&](uint8_t b) { out.push_back(b); };
  m.powerOn();
  m.runCycles(40);
  CHECK(out == std::vector<uint8_t>{0x90, 0x40});
}

TEST_CASE("machine ACIA interrupt drives FIRQ, not IRQ") {
  // Setup $95, LDS, clear F (leave I set), then spin. The FIRQ handler stores
  // a marker, reads the data register and returns.
  const std::vector<uint8_t> main = {
      0x86, 0x03, 0xB7, 0xE1, 0x00,  // reset ACIA
      0x86, 0x95, 0xB7, 0xE1, 0x00,  // RX IRQ on
      0x10, 0xCE, 0x9F, 0x00,        // LDS #$9F00
      0x1C, 0xBF,                    // ANDCC #$BF (clear F)
      0x20, 0xFE,                    // spin
  };
  const std::vector<uint8_t> handler = {
      0x86, 0xAA, 0xB7, 0x90, 0x02,  // LDA #$AA; STA $9002
      0xB6, 0xE1, 0x01,              // LDA $E101 (clears RDRF)
      0x3B,                          // RTI
  };
  Machine m(rom(main, 0xF100, handler));
  m.powerOn();
  m.runCycles(100);
  CHECK(m.peek(0x9002) == 0);
  const std::vector<uint8_t> one = {0x42};
  m.queueMidi(one);
  m.runCycles(2000);
  CHECK(m.peek(0x9002) == 0xAA);
}

TEST_CASE("machine loadState rejects a corrupt ACIA or MIDI section and stays unchanged") {
  Machine src(rom(kReceiver));
  src.powerOn();
  const std::vector<uint8_t> two = {1, 2};
  src.queueMidi(two);
  src.runCycles(100);
  const auto snap = src.saveState();

  Machine m(rom(kReceiver));
  m.powerOn();
  const auto before = m.saveState();

  // Stream tail: ACIA (tag + 4), next-delivery u64, count u32, 2 queued bytes,
  // then the 84-byte panel section.
  constexpr size_t kPanelBytes = 4 + 4 + 2 + 2 * 8 * 8;
  const size_t queueEnd = snap.size() - kPanelBytes;
  auto badAcia = snap;
  badAcia[queueEnd - 2 - 4 - 8 - 1] = 7;  // OVRN flag
  CHECK_THROWS_AS(m.loadState(badAcia), StateError);
  CHECK(m.saveState() == before);

  auto badCount = snap;
  badCount[queueEnd - 2 - 1] = 0x7F;  // count high byte: absurd queue length
  CHECK_THROWS_AS(m.loadState(badCount), StateError);
  CHECK(m.saveState() == before);

  CHECK_NOTHROW(m.loadState(snap));
  CHECK(m.saveState() == snap);
}
