#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "mirage/via6522.h"

using namespace mirage;

namespace {
enum Reg : uint8_t {
  ORB = 0, ORA = 1, DDRB = 2, DDRA = 3, T1CL = 4, T1CH = 5, T1LL = 6, T1LH = 7,
  T2CL = 8, T2CH = 9, SR = 10, ACR = 11, PCR = 12, IFR = 13, IER = 14, ORA_NH = 15
};

void loadT1(Via6522& v, uint16_t n) {
  v.write(T1CL, n & 0xFF);
  v.write(T1CH, n >> 8);
}
}  // namespace

TEST_CASE("via port B DDR masks reads and writes") {
  Via6522 v;
  v.portBPins = [] { return uint8_t(0xA0); };
  std::vector<uint8_t> out;
  v.portBOut = [&](uint8_t b) { out.push_back(b); };
  v.write(DDRB, 0x0F);
  v.write(ORB, 0xFF);
  CHECK(v.read(ORB) == 0xAF);  // pins for inputs, ORB for outputs
  v.write(ORB, 0x35);
  REQUIRE(!out.empty());
  CHECK(out.back() == 0xF5);   // (ORB & DDRB) | ~DDRB
  CHECK(v.read(DDRB) == 0x0F);
  CHECK(v.outB() == 0xF5);
}

TEST_CASE("via port A DDR masks reads and writes, with and without handshake reg") {
  Via6522 v;
  v.portAPins = [] { return uint8_t(0xE5); };
  std::vector<uint8_t> out;
  v.portAOut = [&](uint8_t a) { out.push_back(a); };
  v.write(DDRA, 0x1F);
  v.write(ORA, 0x13);
  CHECK(out.back() == 0xF3);
  CHECK(v.read(ORA) == (0xE0 | 0x13));
  v.write(ORA_NH, 0x0B);
  CHECK(out.back() == 0xEB);
  CHECK(v.read(ORA_NH) == (0xE0 | 0x0B));
  CHECK(v.outA() == 0xEB);
}

TEST_CASE("via unconnected input pins read high") {
  Via6522 v;
  CHECK(v.read(ORA) == 0xFF);
  CHECK(v.read(ORB) == 0xFF);
}

TEST_CASE("via T1 one-shot sets IFR6 after N+2 cycles and raises IRQ when enabled") {
  Via6522 v;
  v.write(IER, 0x80 | 0x40);
  loadT1(v, 5);
  v.tick(6);
  CHECK((v.read(IFR) & 0x40) == 0);
  CHECK_FALSE(v.irq());
  v.tick(1);  // 7 = N+2
  CHECK((v.read(IFR) & 0x40) != 0);
  CHECK((v.read(IFR) & 0x80) != 0);
  CHECK(v.irq());
}

TEST_CASE("via T1 time-out with the interrupt disabled leaves IRQ low") {
  Via6522 v;
  loadT1(v, 5);
  v.tick(100);
  CHECK((v.read(IFR) & 0x40) != 0);
  CHECK((v.read(IFR) & 0x80) == 0);
  CHECK_FALSE(v.irq());
}

TEST_CASE("via T1 one-shot fires once") {
  Via6522 v;
  loadT1(v, 5);
  v.tick(7);
  REQUIRE((v.read(IFR) & 0x40) != 0);
  v.write(IFR, 0x40);
  v.tick(200000);
  CHECK((v.read(IFR) & 0x40) == 0);
}

TEST_CASE("via reading T1C-L clears IFR6, reading T1C-H does not") {
  Via6522 v;
  loadT1(v, 2);
  v.tick(10);
  REQUIRE((v.read(IFR) & 0x40) != 0);
  v.read(T1CH);
  CHECK((v.read(IFR) & 0x40) != 0);
  v.read(T1CL);
  CHECK((v.read(IFR) & 0x40) == 0);
}

TEST_CASE("via writing T1C-H clears IFR6 and reloads from the latch") {
  Via6522 v;
  loadT1(v, 2);
  v.tick(10);
  REQUIRE((v.read(IFR) & 0x40) != 0);
  v.write(T1CH, 0);
  CHECK((v.read(IFR) & 0x40) == 0);
  v.tick(3);
  CHECK((v.read(IFR) & 0x40) == 0);
  v.tick(1);  // latch low was 2: N+2 = 4
  CHECK((v.read(IFR) & 0x40) != 0);
}

TEST_CASE("via T1 counter reads back while running") {
  Via6522 v;
  loadT1(v, 0x0100);
  v.tick(10);
  const uint16_t c = uint16_t(v.read(T1CH) << 8) | v.read(T1CL);
  CHECK(c == 0x0100 - 10);
}

TEST_CASE("via T1 latch registers do not start the timer") {
  Via6522 v;
  v.write(T1LL, 0x04);
  v.write(T1LH, 0x00);
  v.tick(100);
  CHECK((v.read(IFR) & 0x40) == 0);
  CHECK(v.read(T1LL) == 0x04);
  CHECK(v.read(T1LH) == 0x00);
}

TEST_CASE("via T1 free-run toggles PB7 every N+2 cycles") {
  Via6522 v;
  v.write(ACR, 0xC0);
  v.write(T1LL, 3);
  v.write(T1LH, 0);
  v.write(T1CL, 3);
  v.write(T1CH, 0);  // load: PB7 low
  CHECK_FALSE(v.pb7());
  v.tick(4);
  CHECK_FALSE(v.pb7());
  v.tick(1);  // 5 = N+2
  CHECK(v.pb7());
  v.tick(4);
  CHECK(v.pb7());
  v.tick(1);
  CHECK_FALSE(v.pb7());
  // IFR6 is set at every time-out.
  CHECK((v.read(IFR) & 0x40) != 0);
}

TEST_CASE("via T1 free-run with latch 0 gives PB7 at Phi2/4") {
  // boot-rom.md derives 31,250 baud from this: PB7 period is 4 cycles.
  Via6522 v;
  v.write(ACR, 0xC0);
  loadT1(v, 0);
  std::vector<bool> level;
  for (int i = 0; i < 16; ++i) {
    v.tick(1);
    level.push_back(v.pb7());
  }
  // Starts low, toggles every 2 cycles.
  const bool expect[16] = {false, true, true, false, false, true, true, false,
                           false, true, true, false, false, true, true, false};
  for (int i = 0; i < 16; ++i) CHECK(level[size_t(i)] == expect[i]);
}

TEST_CASE("via T1 free-run bulk ticking equals single-cycle ticking") {
  for (uint8_t latch : {uint8_t(0), uint8_t(1), uint8_t(5), uint8_t(40)}) {
    Via6522 a, b;
    for (Via6522* v : {&a, &b}) {
      v->write(ACR, 0xC0);
      v->write(T1LL, latch);
      v->write(T1LH, 0);
      v->write(T1CL, latch);
      v->write(T1CH, 0);
    }
    const uint32_t chunks[] = {1, 7, 3, 20, 2, 100, 19, 1, 64};
    for (uint32_t n : chunks) {
      a.tick(n);
      for (uint32_t i = 0; i < n; ++i) b.tick(1);
      CHECK(a.pb7() == b.pb7());
      CHECK(a.read(T1CL) == b.read(T1CL));
      CHECK(a.read(T1CH) == b.read(T1CH));
    }
  }
}

TEST_CASE("via PB7 drives port B output only while ACR7 is set") {
  Via6522 v;
  std::vector<uint8_t> out;
  v.portBOut = [&](uint8_t b) { out.push_back(b); };
  v.write(DDRB, 0x1F);
  v.write(ORB, 0x00);
  CHECK(out.back() == 0xE0);
  v.write(ACR, 0xC0);
  loadT1(v, 0);  // PB7 low
  CHECK((v.outB() & 0x80) == 0);
  out.clear();
  v.tick(2);  // PB7 rises
  REQUIRE(!out.empty());
  CHECK((out.back() & 0x80) != 0);
  CHECK((v.read(ORB) & 0x80) != 0);  // reading port B shows the T1 output
}

TEST_CASE("via T1 one-shot with ACR7 pulls PB7 low until time-out") {
  Via6522 v;
  v.write(ACR, 0x80);
  loadT1(v, 4);
  CHECK_FALSE(v.pb7());
  v.tick(5);
  CHECK_FALSE(v.pb7());
  v.tick(1);
  CHECK(v.pb7());
  v.tick(100);
  CHECK(v.pb7());
}

TEST_CASE("via T2 one-shot sets IFR5 after N+2 cycles") {
  Via6522 v;
  v.write(T2CL, 0x10);
  v.write(T2CH, 0x00);
  v.tick(17);
  CHECK((v.read(IFR) & 0x20) == 0);
  v.tick(1);  // 18
  CHECK((v.read(IFR) & 0x20) != 0);
}

TEST_CASE("via T2 loaded with the boot ROM value 0x1388 times out at 5002 cycles") {
  Via6522 v;
  v.write(IER, 0xA0);
  v.write(T2CL, 0x88);
  v.write(T2CH, 0x13);
  v.tick(5001);
  CHECK_FALSE(v.irq());
  v.tick(1);
  CHECK(v.irq());
}

TEST_CASE("via reading T2C-L or writing T2C-H clears IFR5") {
  Via6522 v;
  v.write(T2CL, 1);
  v.write(T2CH, 0);
  v.tick(10);
  REQUIRE((v.read(IFR) & 0x20) != 0);
  v.read(T2CL);
  CHECK((v.read(IFR) & 0x20) == 0);
  v.tick(10);
  CHECK((v.read(IFR) & 0x20) == 0);  // one-shot: no second flag
  v.write(T2CH, 0);
  v.tick(10);
  REQUIRE((v.read(IFR) & 0x20) != 0);
  v.write(T2CH, 0);
  CHECK((v.read(IFR) & 0x20) == 0);
}

TEST_CASE("via T2 counter keeps counting after time-out") {
  Via6522 v;
  v.write(T2CL, 0);
  v.write(T2CH, 0);
  v.tick(2);  // time-out, counter = $FFFE
  CHECK(v.read(T2CH) == 0xFF);
  CHECK(v.read(T2CL) == 0xFE);
}

TEST_CASE("via writing IFR clears the flags written as 1") {
  Via6522 v;
  loadT1(v, 1);
  v.write(T2CL, 1);
  v.write(T2CH, 0);
  v.tick(10);
  REQUIRE((v.read(IFR) & 0x60) == 0x60);
  v.write(IFR, 0x80 | 0x40);  // bit 7 set: only bit 6 is a flag to clear
  CHECK((v.read(IFR) & 0x60) == 0x20);
  v.write(IFR, 0x20);
  CHECK((v.read(IFR) & 0x7F) == 0);
}

TEST_CASE("via IFR bit 7 follows enabled flags") {
  Via6522 v;
  loadT1(v, 1);
  v.tick(10);
  CHECK(v.read(IFR) == 0x40);
  v.write(IER, 0x80 | 0x40);
  CHECK(v.read(IFR) == 0xC0);
  v.write(IER, 0x40);  // bit 7 clear: disables
  CHECK(v.read(IFR) == 0x40);
  CHECK_FALSE(v.irq());
}

TEST_CASE("via IER set and clear semantics") {
  Via6522 v;
  CHECK(v.read(IER) == 0x80);          // bit 7 always reads 1
  v.write(IER, 0x80 | 0x22);
  CHECK(v.read(IER) == 0xA2);
  v.write(IER, 0x80 | 0x04);
  CHECK(v.read(IER) == 0xA6);          // ROM hand-off value
  v.write(IER, 0x02);                  // bit 7 clear: clears bit 1
  CHECK(v.read(IER) == 0xA4);
  v.write(IER, 0x7F);                  // ROM boot value: all off
  CHECK(v.read(IER) == 0x80);
}

TEST_CASE("via boot ROM setup values read back") {
  Via6522 v;
  v.write(DDRB, 0x1F);
  v.write(DDRA, 0x1F);
  v.write(ACR, 0xCC);
  v.write(PCR, 0x0E);
  v.write(SR, 0x5A);
  CHECK(v.read(DDRB) == 0x1F);
  CHECK(v.read(DDRA) == 0x1F);
  CHECK(v.read(ACR) == 0xCC);
  CHECK(v.read(PCR) == 0x0E);
  CHECK(v.read(SR) == 0x5A);
  v.write(T2CL, 0x88);
  v.write(T2CH, 0x13);
  v.write(IER, 0xA6);
  CHECK(v.read(IER) == 0xA6);
}

TEST_CASE("via register index uses the low four bits") {
  Via6522 v;
  v.write(0x12, 0x1F);  // DDRB
  CHECK(v.read(DDRB) == 0x1F);
  CHECK(v.read(0xF2) == 0x1F);
}

TEST_CASE("via reset clears registers and keeps the callbacks") {
  Via6522 v;
  int calls = 0;
  v.portBOut = [&](uint8_t) { ++calls; };
  v.write(DDRB, 0x1F);
  v.write(IER, 0xFF);
  loadT1(v, 1);
  v.tick(10);
  v.reset();
  CHECK(v.read(DDRB) == 0x00);
  CHECK(v.read(IER) == 0x80);
  CHECK(v.read(IFR) == 0);
  CHECK_FALSE(v.irq());
  CHECK(v.pb7());
  const int before = calls;
  v.write(DDRB, 0x1F);
  CHECK(calls > before);
}

TEST_CASE("via save/load round-trips and continues identically") {
  Via6522 a;
  a.write(ACR, 0xC0);
  a.write(IER, 0xC0);
  a.write(DDRB, 0x1F);
  a.write(ORB, 0x13);
  a.write(T1LL, 6);
  a.write(T1LH, 0);
  a.write(T1CL, 6);
  a.write(T1CH, 0);
  a.write(T2CL, 0x40);
  a.write(T2CH, 0);
  a.tick(11);
  StateWriter w;
  a.save(w);
  const auto bytes = w.take();

  Via6522 b;
  StateReader r(bytes);
  b.load(r);
  CHECK(r.atEnd());
  for (int i = 0; i < 40; ++i) {
    a.tick(3);
    b.tick(3);
    REQUIRE(a.pb7() == b.pb7());
    REQUIRE(a.read(IFR) == b.read(IFR));
    REQUIRE(a.read(T2CL) == b.read(T2CL));
  }
  StateWriter wa, wb;
  a.save(wa);
  b.save(wb);
  CHECK(wa.take() == wb.take());
}

TEST_CASE("via load leaves the chip unchanged on error") {
  Via6522 a;
  a.write(DDRB, 0x1F);
  StateWriter w;
  a.save(w);
  auto bytes = w.take();
  bytes.resize(bytes.size() - 2);

  Via6522 b;
  b.write(DDRB, 0x03);
  StateReader r(bytes);
  CHECK_THROWS_AS(b.load(r), StateError);
  CHECK(b.read(DDRB) == 0x03);
}
