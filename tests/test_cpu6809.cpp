#include <catch2/catch_test_macros.hpp>

#include <array>
#include <initializer_list>
#include <random>

#include "mirage/cpu6809.h"

using namespace mirage;

namespace {

struct RamBus : Bus {
  std::array<uint8_t, 65536> mem{};
  uint8_t read(uint16_t a) override { return mem[a]; }
  void write(uint16_t a, uint8_t v) override { mem[a] = v; }
  void put16(uint16_t a, uint16_t v) {
    mem[a] = uint8_t(v >> 8);
    mem[uint16_t(a + 1)] = uint8_t(v);
  }
  uint16_t get16(uint16_t a) const { return uint16_t(mem[a] << 8 | mem[uint16_t(a + 1)]); }
};

// Writes code at pc, points the reset vector at it and resets.
Cpu6809 cpuAt(RamBus& bus, uint16_t pc, std::initializer_list<uint8_t> code) {
  uint16_t a = pc;
  for (uint8_t b : code) bus.mem[a++] = b;
  bus.put16(0xFFFE, pc);
  Cpu6809 cpu(bus);
  cpu.reset();
  return cpu;
}

bool flag(const Cpu6809& cpu, uint8_t bit) { return (cpu.regs().cc & bit) != 0; }

}  // namespace

// ---- Required table ------------------------------------------------------

TEST_CASE("cpu LDA immediate") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x86, 0x80});
  REQUIRE(cpu.step() == 2);
  REQUIRE(cpu.regs().a == 0x80);
  REQUIRE(flag(cpu, cc::N));
  REQUIRE_FALSE(flag(cpu, cc::Z));
  REQUIRE_FALSE(flag(cpu, cc::V));
  REQUIRE(cpu.regs().pc == 0x1002);
}

TEST_CASE("cpu ADDA overflow and half carry") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x8B, 0x01});
  cpu.regs().a = 0x7F;
  REQUIRE(cpu.step() == 2);
  REQUIRE(cpu.regs().a == 0x80);
  REQUIRE(flag(cpu, cc::V));
  REQUIRE(flag(cpu, cc::N));
  REQUIRE(flag(cpu, cc::H));
  REQUIRE_FALSE(flag(cpu, cc::C));
}

TEST_CASE("cpu SUBA borrow") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x80, 0x01});
  cpu.regs().a = 0x00;
  REQUIRE(cpu.step() == 2);
  REQUIRE(cpu.regs().a == 0xFF);
  REQUIRE(flag(cpu, cc::C));
  REQUIRE(flag(cpu, cc::N));
  REQUIRE_FALSE(flag(cpu, cc::V));
}

TEST_CASE("cpu MUL") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x3D});
  cpu.regs().a = 0x0C;
  cpu.regs().b = 0x64;
  REQUIRE(cpu.step() == 11);
  REQUIRE(cpu.regs().d() == 0x04B0);
  REQUIRE(flag(cpu, cc::C));
  REQUIRE_FALSE(flag(cpu, cc::Z));
}

TEST_CASE("cpu ADDA then DAA") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x8B, 0x01, 0x19});
  cpu.regs().a = 0x09;
  cpu.step();
  REQUIRE(cpu.step() == 2);
  REQUIRE(cpu.regs().a == 0x10);
}

TEST_CASE("cpu SEX") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x1D});
  cpu.regs().b = 0x80;
  REQUIRE(cpu.step() == 2);
  REQUIRE(cpu.regs().a == 0xFF);
  REQUIRE(flag(cpu, cc::N));
}

TEST_CASE("cpu ABX") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x3A});
  cpu.regs().x = 0x1000;
  cpu.regs().b = 0xFF;
  const uint8_t ccBefore = cpu.regs().cc;
  REQUIRE(cpu.step() == 3);
  REQUIRE(cpu.regs().x == 0x10FF);
  REQUIRE(cpu.regs().cc == ccBefore);
}

TEST_CASE("cpu LDA ,X+") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xA6, 0x80});
  cpu.regs().x = 0x2000;
  bus.mem[0x2000] = 0x5A;
  REQUIRE(cpu.step() == 6);
  REQUIRE(cpu.regs().a == 0x5A);
  REQUIRE(cpu.regs().x == 0x2001);
}

TEST_CASE("cpu LDA [$3000] extended indirect") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xA6, 0x9F, 0x30, 0x00});
  bus.put16(0x3000, 0x4000);
  bus.mem[0x4000] = 0x77;
  REQUIRE(cpu.step() == 9);
  REQUIRE(cpu.regs().a == 0x77);
  REQUIRE(cpu.regs().pc == 0x1004);
}

TEST_CASE("cpu LDA -1,X 5-bit offset") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xA6, 0x1F});
  cpu.regs().x = 0x2001;
  bus.mem[0x2000] = 0x33;
  REQUIRE(cpu.step() == 5);
  REQUIRE(cpu.regs().a == 0x33);
}

TEST_CASE("cpu LDA D,X") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xA6, 0x8B});
  cpu.regs().setD(0x0010);
  cpu.regs().x = 0x2000;
  bus.mem[0x2010] = 0x44;
  REQUIRE(cpu.step() == 8);
  REQUIRE(cpu.regs().a == 0x44);
}

TEST_CASE("cpu LEAX 8-bit PC-relative") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x30, 0x8C, 0x10});
  REQUIRE(cpu.step() == 5);
  REQUIRE(cpu.regs().x == 0x1013);
  REQUIRE_FALSE(flag(cpu, cc::Z));
}

TEST_CASE("cpu PSHS and PULS") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x34, 0x16, 0x35, 0x16});
  auto& r = cpu.regs();
  r.s = 0x8000;
  r.a = 0x11;
  r.b = 0x22;
  r.x = 0x3344;
  REQUIRE(cpu.step() == 9);
  REQUIRE(r.s == 0x7FFC);
  REQUIRE(bus.mem[0x7FFC] == 0x11);
  REQUIRE(bus.mem[0x7FFD] == 0x22);
  REQUIRE(bus.mem[0x7FFE] == 0x33);
  REQUIRE(bus.mem[0x7FFF] == 0x44);
  r.a = 0;
  r.b = 0;
  r.x = 0;
  REQUIRE(cpu.step() == 9);
  REQUIRE(r.s == 0x8000);
  REQUIRE(r.a == 0x11);
  REQUIRE(r.b == 0x22);
  REQUIRE(r.x == 0x3344);
}

TEST_CASE("cpu JSR extended and RTS") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xBD, 0x20, 0x00});
  bus.mem[0x2000] = 0x39;
  cpu.regs().s = 0x8000;
  REQUIRE(cpu.step() == 8);
  REQUIRE(cpu.regs().pc == 0x2000);
  REQUIRE(cpu.regs().s == 0x7FFE);
  REQUIRE(bus.get16(0x7FFE) == 0x1003);
  REQUIRE(cpu.step() == 5);
  REQUIRE(cpu.regs().pc == 0x1003);
  REQUIRE(cpu.regs().s == 0x8000);
}

TEST_CASE("cpu LBEQ taken") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x10, 0x27, 0x00, 0x10});
  cpu.regs().cc |= cc::Z;
  REQUIRE(cpu.step() == 6);
  REQUIRE(cpu.regs().pc == 0x1014);
}

TEST_CASE("cpu EXG A,B") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x1E, 0x89});
  cpu.regs().a = 1;
  cpu.regs().b = 2;
  REQUIRE(cpu.step() == 8);
  REQUIRE(cpu.regs().a == 2);
  REQUIRE(cpu.regs().b == 1);
}

TEST_CASE("cpu CMPU immediate") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x11, 0x83, 0x12, 0x34});
  cpu.regs().u = 0x1234;
  REQUIRE(cpu.step() == 5);
  REQUIRE(flag(cpu, cc::Z));
  REQUIRE(cpu.regs().pc == 0x1004);
}

// ---- Interrupts ------------------------------------------------------------

TEST_CASE("cpu IRQ pushes entire state") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x12});
  bus.put16(0xFFF8, 0x3000);
  auto& r = cpu.regs();
  r.s = 0x8000;
  r.cc = 0;
  r.a = 0xAA;
  r.u = 0x1234;
  cpu.setIrq(true);
  REQUIRE(cpu.step() == 19);
  REQUIRE(r.pc == 0x3000);
  REQUIRE(r.s == 0x8000 - 12);
  REQUIRE(flag(cpu, cc::E));
  REQUIRE(flag(cpu, cc::I));
  REQUIRE_FALSE(flag(cpu, cc::F));
  REQUIRE((bus.mem[0x7FF4] & cc::E) != 0);  // stacked CC
  REQUIRE(bus.mem[0x7FF5] == 0xAA);         // A
  REQUIRE(bus.get16(0x7FFC) == 0x1234);     // U
  REQUIRE(bus.get16(0x7FFE) == 0x1000);     // return PC
  // Masked now: the next step runs code at the vector.
  bus.mem[0x3000] = 0x3B;  // RTI pops all 12 bytes because E=1
  REQUIRE(cpu.step() == 15);
  REQUIRE(r.pc == 0x1000);
  REQUIRE(r.s == 0x8000);
  REQUIRE(r.u == 0x1234);
  REQUIRE_FALSE(flag(cpu, cc::I));
}

TEST_CASE("cpu IRQ masked by I") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x12});
  cpu.regs().s = 0x8000;
  cpu.setIrq(true);  // reset set I
  REQUIRE(cpu.step() == 2);
  REQUIRE(cpu.regs().pc == 0x1001);
}

TEST_CASE("cpu FIRQ pushes PC and CC, RTI pops 3 bytes") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x12});
  bus.put16(0xFFF6, 0x3000);
  bus.mem[0x3000] = 0x3B;
  auto& r = cpu.regs();
  r.s = 0x8000;
  r.cc = cc::E;  // FIRQ must clear E before stacking CC
  cpu.setFirq(true);
  REQUIRE(cpu.step() == 10);
  REQUIRE(r.pc == 0x3000);
  REQUIRE(r.s == 0x7FFD);
  REQUIRE_FALSE(flag(cpu, cc::E));
  REQUIRE(flag(cpu, cc::I));
  REQUIRE(flag(cpu, cc::F));
  REQUIRE(bus.get16(0x7FFE) == 0x1000);
  cpu.setFirq(false);
  REQUIRE(cpu.step() == 6);
  REQUIRE(r.s == 0x8000);
  REQUIRE(r.pc == 0x1000);
}

TEST_CASE("cpu NMI ignored until S is loaded") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x10, 0xCE, 0x80, 0x00, 0x12});
  bus.put16(0xFFFC, 0x3000);
  cpu.setNmi(true);  // edge before any S load: ignored
  REQUIRE(cpu.step() == 4);  // LDS #$8000
  REQUIRE(cpu.regs().pc == 0x1004);
  REQUIRE(cpu.regs().s == 0x8000);
  REQUIRE(cpu.step() == 2);  // NOP: the early edge was dropped
  cpu.setNmi(false);
  cpu.setNmi(true);
  REQUIRE(cpu.step() == 19);
  REQUIRE(cpu.regs().pc == 0x3000);
  REQUIRE(cpu.regs().s == 0x8000 - 12);
  REQUIRE(flag(cpu, cc::I));
  REQUIRE(flag(cpu, cc::F));
  REQUIRE(flag(cpu, cc::E));
}

TEST_CASE("cpu NMI is edge-triggered and outranks FIRQ and IRQ") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x10, 0xCE, 0x80, 0x00, 0x12, 0x12, 0x12});
  bus.put16(0xFFFC, 0x3000);
  bus.put16(0xFFF6, 0x4000);
  bus.mem[0x3000] = 0x3B;  // RTI
  cpu.step();
  cpu.regs().cc = 0;
  cpu.setIrq(true);
  cpu.setFirq(true);
  cpu.setNmi(true);
  REQUIRE(cpu.step() == 19);
  REQUIRE(cpu.regs().pc == 0x3000);
  cpu.step();  // RTI (I and F restored clear)
  REQUIRE(cpu.regs().pc == 0x1004);
  REQUIRE(cpu.step() == 10);  // line still high, but no new edge: FIRQ next
  REQUIRE(cpu.regs().pc == 0x4000);
}

TEST_CASE("cpu TFR to S and PULU into S arm NMI") {
  RamBus bus;
  bus.put16(0xFFFC, 0x3000);
  SECTION("TFR X,S") {
    auto cpu = cpuAt(bus, 0x1000, {0x1F, 0x14, 0x12});
    cpu.regs().x = 0x8000;
    REQUIRE(cpu.step() == 6);
    REQUIRE(cpu.regs().s == 0x8000);
    cpu.setNmi(true);
    REQUIRE(cpu.step() == 19);
    REQUIRE(cpu.regs().pc == 0x3000);
  }
  SECTION("PULU S") {
    auto cpu = cpuAt(bus, 0x1000, {0x37, 0x40, 0x12});
    cpu.regs().u = 0x6000;
    bus.put16(0x6000, 0x8000);
    REQUIRE(cpu.step() == 7);
    REQUIRE(cpu.regs().s == 0x8000);
    REQUIRE(cpu.regs().u == 0x6002);
    cpu.setNmi(true);
    REQUIRE(cpu.step() == 19);
  }
}

TEST_CASE("cpu CWAI waits then vectors without re-pushing") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x3C, 0xEF});
  bus.put16(0xFFF8, 0x3000);
  bus.mem[0x3000] = 0x3B;
  auto& r = cpu.regs();
  r.s = 0x8000;
  REQUIRE(cpu.step() == 16);  // 20 total; 4 more when the interrupt arrives
  REQUIRE(r.s == 0x8000 - 12);
  REQUIRE_FALSE(flag(cpu, cc::I));
  REQUIRE(flag(cpu, cc::E));
  REQUIRE(bus.get16(0x7FFE) == 0x1002);
  for (int i = 0; i < 3; ++i) {
    REQUIRE(cpu.step() == 1);
    REQUIRE(r.pc == 0x1002);
  }
  cpu.setIrq(true);
  REQUIRE(cpu.step() == 4);
  REQUIRE(r.pc == 0x3000);
  REQUIRE(r.s == 0x8000 - 12);
  REQUIRE(flag(cpu, cc::I));
  cpu.setIrq(false);
  REQUIRE(cpu.step() == 15);  // RTI pops the entire state CWAI stacked
  REQUIRE(r.pc == 0x1002);
  REQUIRE(r.s == 0x8000);
}

TEST_CASE("cpu SYNC resumes on masked IRQ without vectoring") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x13, 0x12});
  bus.put16(0xFFF8, 0x3000);
  cpu.regs().s = 0x8000;
  REQUIRE(flag(cpu, cc::I));
  REQUIRE(cpu.step() == 2);
  REQUIRE(cpu.regs().pc == 0x1001);
  REQUIRE(cpu.step() == 1);
  REQUIRE(cpu.step() == 1);
  REQUIRE(cpu.regs().pc == 0x1001);
  cpu.setIrq(true);
  REQUIRE(cpu.step() == 2);  // leaves the sync state
  REQUIRE(cpu.step() == 2);  // NOP: no vectoring because I=1
  REQUIRE(cpu.regs().pc == 0x1002);
  REQUIRE(cpu.regs().s == 0x8000);
}

TEST_CASE("cpu SYNC with unmasked IRQ vectors after resuming") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x13, 0x12});
  bus.put16(0xFFF8, 0x3000);
  cpu.regs().s = 0x8000;
  cpu.regs().cc = 0;
  cpu.step();
  cpu.setIrq(true);
  REQUIRE(cpu.step() == 2);
  REQUIRE(cpu.step() == 19);
  REQUIRE(cpu.regs().pc == 0x3000);
  REQUIRE(bus.get16(0x7FFE) == 0x1001);
}

TEST_CASE("cpu SWI, SWI2, SWI3") {
  RamBus bus;
  bus.put16(0xFFFA, 0x3000);
  bus.put16(0xFFF4, 0x3100);
  bus.put16(0xFFF2, 0x3200);
  SECTION("SWI masks I and F") {
    auto cpu = cpuAt(bus, 0x1000, {0x3F});
    cpu.regs().s = 0x8000;
    cpu.regs().cc = 0;
    REQUIRE(cpu.step() == 19);
    REQUIRE(cpu.regs().pc == 0x3000);
    REQUIRE(cpu.regs().s == 0x7FF4);
    REQUIRE(cpu.regs().cc == (cc::E | cc::I | cc::F));
    REQUIRE(bus.get16(0x7FFE) == 0x1001);
  }
  SECTION("SWI2 leaves masks") {
    auto cpu = cpuAt(bus, 0x1000, {0x10, 0x3F});
    cpu.regs().s = 0x8000;
    cpu.regs().cc = 0;
    REQUIRE(cpu.step() == 20);
    REQUIRE(cpu.regs().pc == 0x3100);
    REQUIRE(cpu.regs().cc == cc::E);
    REQUIRE(bus.get16(0x7FFE) == 0x1002);
  }
  SECTION("SWI3 leaves masks") {
    auto cpu = cpuAt(bus, 0x1000, {0x11, 0x3F});
    cpu.regs().s = 0x8000;
    cpu.regs().cc = 0;
    REQUIRE(cpu.step() == 20);
    REQUIRE(cpu.regs().pc == 0x3200);
    REQUIRE(cpu.regs().cc == cc::E);
  }
}

// ---- Illegal opcodes, runaway, save/load -----------------------------------

TEST_CASE("cpu illegal opcode is a counted 2-cycle no-op") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x01, 0x10, 0x01});
  int cycles = 0;
  REQUIRE_NOTHROW(cycles = cpu.step());
  REQUIRE(cycles == 2);
  REQUIRE(cpu.illegalCount() == 1);
  REQUIRE(cpu.lastIllegalPc() == 0x1000);
  REQUIRE(cpu.regs().pc == 0x1001);
  REQUIRE(cpu.step() == 2);  // illegal page-2 opcode
  REQUIRE(cpu.illegalCount() == 2);
  REQUIRE(cpu.lastIllegalPc() == 0x1001);
  REQUIRE(cpu.regs().pc == 0x1003);
}

TEST_CASE("cpu illegal indexed postbyte is counted") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xA6, 0x87});
  REQUIRE_NOTHROW(cpu.step());
  REQUIRE(cpu.illegalCount() == 1);
  REQUIRE(cpu.lastIllegalPc() == 0x1000);
  REQUIRE(cpu.regs().pc == 0x1002);
}

TEST_CASE("cpu runs random memory without throwing or stalling") {
  RamBus bus;
  std::mt19937 rng(6809);
  for (auto& b : bus.mem) b = uint8_t(rng());
  Cpu6809 cpu(bus);
  cpu.reset();
  uint64_t cycles = 0;
  for (int i = 0; i < 200000; ++i) {
    if (i % 1000 == 0) {  // wiggle the interrupt lines too
      cpu.setIrq((i / 1000) & 1);
      cpu.setFirq((i / 3000) & 1);
      cpu.setNmi((i / 7000) & 1);
    }
    const int c = cpu.step();
    if (c < 1 || c > 30) FAIL("step " << i << " returned " << c);
    cycles += uint64_t(c);
  }
  REQUIRE(cycles >= 200000);
}

TEST_CASE("cpu save/load round-trips registers and pending NMI") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x10, 0xCE, 0x80, 0x00, 0x12});
  bus.put16(0xFFFC, 0x3000);
  cpu.step();  // LDS arms NMI
  auto& r = cpu.regs();
  r.a = 1; r.b = 2; r.dp = 3; r.cc = 0x2A; r.x = 0x1111; r.y = 0x2222; r.u = 0x3333;
  cpu.setNmi(true);
  cpu.setIrq(true);
  StateWriter w;
  cpu.save(w);
  auto buf = w.take();

  RamBus bus2 = bus;
  Cpu6809 cpu2(bus2);
  StateReader rd(buf);
  cpu2.load(rd);
  REQUIRE(rd.atEnd());
  const auto& r2 = cpu2.regs();
  REQUIRE(r2.a == 1); REQUIRE(r2.b == 2); REQUIRE(r2.dp == 3); REQUIRE(r2.cc == 0x2A);
  REQUIRE(r2.x == 0x1111); REQUIRE(r2.y == 0x2222); REQUIRE(r2.u == 0x3333);
  REQUIRE(r2.s == 0x8000); REQUIRE(r2.pc == 0x1004);
  REQUIRE(cpu2.step() == 19);  // the pending NMI survived
  REQUIRE(r2.pc == 0x3000);
}

TEST_CASE("cpu save/load keeps the CWAI wait state") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x3C, 0xFF});
  cpu.regs().s = 0x8000;
  cpu.step();
  StateWriter w;
  cpu.save(w);
  auto buf = w.take();
  Cpu6809 cpu2(bus);
  StateReader rd(buf);
  cpu2.load(rd);
  REQUIRE(cpu2.step() == 1);
  REQUIRE(cpu2.regs().pc == 0x1002);
}

// ---- Flag rules --------------------------------------------------------------

TEST_CASE("cpu ADDD flags") {
  RamBus bus;
  SECTION("signed overflow") {
    auto cpu = cpuAt(bus, 0x1000, {0xC3, 0x00, 0x01});
    cpu.regs().setD(0x7FFF);
    cpu.regs().cc = cc::H;  // H is not affected by 16-bit adds
    REQUIRE(cpu.step() == 4);
    REQUIRE(cpu.regs().d() == 0x8000);
    REQUIRE(cpu.regs().cc == (cc::H | cc::N | cc::V));
  }
  SECTION("carry to zero") {
    auto cpu = cpuAt(bus, 0x1000, {0xC3, 0x00, 0x01});
    cpu.regs().setD(0xFFFF);
    cpu.regs().cc = 0;
    cpu.step();
    REQUIRE(cpu.regs().d() == 0);
    REQUIRE(cpu.regs().cc == (cc::Z | cc::C));
  }
}

TEST_CASE("cpu SUBD and CMPX flags") {
  RamBus bus;
  SECTION("SUBD borrow") {
    auto cpu = cpuAt(bus, 0x1000, {0x83, 0x00, 0x01});
    cpu.regs().setD(0x0000);
    cpu.regs().cc = 0;
    REQUIRE(cpu.step() == 4);
    REQUIRE(cpu.regs().d() == 0xFFFF);
    REQUIRE(cpu.regs().cc == (cc::N | cc::C));
  }
  SECTION("SUBD overflow") {
    auto cpu = cpuAt(bus, 0x1000, {0x83, 0x00, 0x01});
    cpu.regs().setD(0x8000);
    cpu.regs().cc = 0;
    cpu.step();
    REQUIRE(cpu.regs().d() == 0x7FFF);
    REQUIRE(cpu.regs().cc == cc::V);
  }
  SECTION("CMPX extended") {
    auto cpu = cpuAt(bus, 0x1000, {0xBC, 0x30, 0x00});
    bus.put16(0x3000, 0x2000);
    cpu.regs().x = 0x1000;
    cpu.regs().cc = 0;
    REQUIRE(cpu.step() == 7);
    REQUIRE(cpu.regs().x == 0x1000);
    REQUIRE(cpu.regs().cc == (cc::N | cc::C));
  }
  SECTION("CMPD direct") {
    auto cpu = cpuAt(bus, 0x1000, {0x10, 0x93, 0x40});
    cpu.regs().dp = 0x30;
    bus.put16(0x3040, 0x8000);
    cpu.regs().setD(0x7FFF);
    cpu.regs().cc = 0;
    REQUIRE(cpu.step() == 7);
    REQUIRE(cpu.regs().cc == (cc::N | cc::V | cc::C));
  }
}

TEST_CASE("cpu ADC and SBC use carry") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x89, 0x0F, 0xC2, 0x01});
  cpu.regs().a = 0x00;
  cpu.regs().b = 0x00;
  cpu.regs().cc = cc::C;
  cpu.step();
  REQUIRE(cpu.regs().a == 0x10);
  REQUIRE(flag(cpu, cc::H));
  REQUIRE_FALSE(flag(cpu, cc::C));
  cpu.regs().cc = cc::C;
  cpu.step();
  REQUIRE(cpu.regs().b == 0xFE);
  REQUIRE(flag(cpu, cc::C));
  REQUIRE(flag(cpu, cc::N));
}

TEST_CASE("cpu NEG flags") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x40, 0x40, 0x40});
  cpu.regs().cc = cc::C;
  cpu.regs().a = 0x00;
  cpu.step();
  REQUIRE(cpu.regs().a == 0x00);
  REQUIRE(cpu.regs().cc == cc::Z);  // C cleared when operand is 0
  cpu.regs().a = 0x80;
  cpu.step();
  REQUIRE(cpu.regs().a == 0x80);
  REQUIRE(cpu.regs().cc == (cc::N | cc::V | cc::C));
  cpu.regs().a = 0x01;
  cpu.step();
  REQUIRE(cpu.regs().a == 0xFF);
  REQUIRE(cpu.regs().cc == (cc::N | cc::C));
}

TEST_CASE("cpu COM, LSR, ROR, ASR, ASL, ROL flags") {
  RamBus bus;
  SECTION("COMA sets C, clears V") {
    auto cpu = cpuAt(bus, 0x1000, {0x43});
    cpu.regs().a = 0xFF;
    cpu.regs().cc = cc::V;
    cpu.step();
    REQUIRE(cpu.regs().a == 0x00);
    REQUIRE(cpu.regs().cc == (cc::Z | cc::C));
  }
  SECTION("LSRA clears N, keeps V") {
    auto cpu = cpuAt(bus, 0x1000, {0x44});
    cpu.regs().a = 0x81;
    cpu.regs().cc = cc::N | cc::V;
    cpu.step();
    REQUIRE(cpu.regs().a == 0x40);
    REQUIRE(cpu.regs().cc == (cc::V | cc::C));
  }
  SECTION("RORA rotates carry into bit 7") {
    auto cpu = cpuAt(bus, 0x1000, {0x46});
    cpu.regs().a = 0x02;
    cpu.regs().cc = cc::C;
    cpu.step();
    REQUIRE(cpu.regs().a == 0x81);
    REQUIRE(cpu.regs().cc == cc::N);
  }
  SECTION("ASRA keeps sign") {
    auto cpu = cpuAt(bus, 0x1000, {0x47});
    cpu.regs().a = 0x81;
    cpu.regs().cc = 0;
    cpu.step();
    REQUIRE(cpu.regs().a == 0xC0);
    REQUIRE(cpu.regs().cc == (cc::N | cc::C));
  }
  SECTION("ASLB overflow is b7 xor b6") {
    auto cpu = cpuAt(bus, 0x1000, {0x58});
    cpu.regs().b = 0x40;
    cpu.regs().cc = 0;
    cpu.step();
    REQUIRE(cpu.regs().b == 0x80);
    REQUIRE(cpu.regs().cc == (cc::N | cc::V));
  }
  SECTION("ROLA carry in and out") {
    auto cpu = cpuAt(bus, 0x1000, {0x49});
    cpu.regs().a = 0xC0;
    cpu.regs().cc = cc::C;
    cpu.step();
    REQUIRE(cpu.regs().a == 0x81);
    REQUIRE(cpu.regs().cc == (cc::N | cc::C));
  }
  SECTION("INC/DEC overflow, C untouched") {
    auto cpu = cpuAt(bus, 0x1000, {0x4C, 0x5A});
    cpu.regs().a = 0x7F;
    cpu.regs().b = 0x80;
    cpu.regs().cc = cc::C;
    cpu.step();
    REQUIRE(cpu.regs().a == 0x80);
    REQUIRE(cpu.regs().cc == (cc::N | cc::V | cc::C));
    cpu.step();
    REQUIRE(cpu.regs().b == 0x7F);
    REQUIRE(cpu.regs().cc == (cc::V | cc::C));
  }
  SECTION("CLR memory extended") {
    auto cpu = cpuAt(bus, 0x1000, {0x7F, 0x20, 0x00});
    bus.mem[0x2000] = 0x55;
    cpu.regs().cc = cc::N | cc::V | cc::C;
    REQUIRE(cpu.step() == 7);
    REQUIRE(bus.mem[0x2000] == 0);
    REQUIRE(cpu.regs().cc == cc::Z);
  }
  SECTION("INC direct") {
    auto cpu = cpuAt(bus, 0x1000, {0x0C, 0x10});
    cpu.regs().dp = 0x20;
    bus.mem[0x2010] = 0x41;
    REQUIRE(cpu.step() == 6);
    REQUIRE(bus.mem[0x2010] == 0x42);
  }
}

// ---- Addressing modes --------------------------------------------------------

TEST_CASE("cpu indexed auto-decrement ,--X") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xEC, 0x83});  // LDD ,--X
  cpu.regs().x = 0x2002;
  bus.put16(0x2000, 0xBEEF);
  REQUIRE(cpu.step() == 8);
  REQUIRE(cpu.regs().x == 0x2000);
  REQUIRE(cpu.regs().d() == 0xBEEF);
}

TEST_CASE("cpu indexed ,-U and ,Y++") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xA7, 0xC2, 0xE6, 0xA1});  // STA ,-U ; LDB ,Y++
  cpu.regs().u = 0x2001;
  cpu.regs().a = 0x66;
  cpu.regs().y = 0x3000;
  bus.mem[0x3000] = 0x12;
  REQUIRE(cpu.step() == 6);
  REQUIRE(cpu.regs().u == 0x2000);
  REQUIRE(bus.mem[0x2000] == 0x66);
  REQUIRE(cpu.step() == 7);
  REQUIRE(cpu.regs().b == 0x12);
  REQUIRE(cpu.regs().y == 0x3002);
}

TEST_CASE("cpu indexed 16-bit offset") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xA6, 0xA9, 0xF0, 0x00});  // LDA -$1000,Y
  cpu.regs().y = 0x3000;
  bus.mem[0x2000] = 0x5E;
  REQUIRE(cpu.step() == 8);
  REQUIRE(cpu.regs().a == 0x5E);
  REQUIRE(cpu.regs().pc == 0x1004);
}

TEST_CASE("cpu indexed 8-bit offset on S and B,U") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xA6, 0xE8, 0xFE, 0xA6, 0xC5});  // LDA -2,S ; LDA B,U
  cpu.regs().s = 0x2002;
  cpu.regs().u = 0x2100;
  cpu.regs().b = 0xF0;  // negative 8-bit accumulator offset
  bus.mem[0x2000] = 0x21;
  bus.mem[0x20F0] = 0x22;
  REQUIRE(cpu.step() == 5);
  REQUIRE(cpu.regs().a == 0x21);
  REQUIRE(cpu.step() == 5);
  REQUIRE(cpu.regs().a == 0x22);
}

TEST_CASE("cpu indexed indirect [,X++]") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xA6, 0x91});
  cpu.regs().x = 0x2000;
  bus.put16(0x2000, 0x3000);
  bus.mem[0x3000] = 0x99;
  REQUIRE(cpu.step() == 10);
  REQUIRE(cpu.regs().a == 0x99);
  REQUIRE(cpu.regs().x == 0x2002);
}

TEST_CASE("cpu indexed indirect [n,X] and [,X]") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xA6, 0x98, 0x04, 0xA6, 0x94});  // LDA [4,X] ; LDA [,X]
  cpu.regs().x = 0x2000;
  bus.put16(0x2004, 0x3000);
  bus.put16(0x2000, 0x3001);
  bus.mem[0x3000] = 0x0A;
  bus.mem[0x3001] = 0x0B;
  REQUIRE(cpu.step() == 8);
  REQUIRE(cpu.regs().a == 0x0A);
  REQUIRE(cpu.step() == 7);
  REQUIRE(cpu.regs().a == 0x0B);
}

TEST_CASE("cpu indexed PC-relative 16-bit") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0xA6, 0x8D, 0x10, 0x00});  // LDA $1000,PCR
  bus.mem[0x2004] = 0x7E;
  REQUIRE(cpu.step() == 9);
  REQUIRE(cpu.regs().a == 0x7E);

  auto cpu2 = cpuAt(bus, 0x1000, {0xA6, 0x9D, 0x10, 0x00});  // LDA [$1000,PCR]
  bus.put16(0x2004, 0x2100);
  bus.mem[0x2100] = 0x6D;
  REQUIRE(cpu2.step() == 12);
  REQUIRE(cpu2.regs().a == 0x6D);
}

TEST_CASE("cpu LEAS/LEAU/LEAY flags") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x31, 0x3F, 0x33, 0x5F});  // LEAY -1,Y ; LEAU -1,U
  cpu.regs().y = 1;
  cpu.regs().u = 1;
  cpu.regs().cc = 0;
  REQUIRE(cpu.step() == 5);
  REQUIRE(cpu.regs().y == 0);
  REQUIRE(flag(cpu, cc::Z));
  cpu.regs().cc = 0;
  cpu.step();
  REQUIRE(cpu.regs().u == 0);
  REQUIRE_FALSE(flag(cpu, cc::Z));  // LEAU does not touch flags
}

// ---- Branches, stack, transfers ----------------------------------------------

TEST_CASE("cpu short branches and BSR") {
  RamBus bus;
  SECTION("BNE backward taken") {
    auto cpu = cpuAt(bus, 0x1000, {0x26, 0xFE});
    cpu.regs().cc = 0;
    REQUIRE(cpu.step() == 3);
    REQUIRE(cpu.regs().pc == 0x1000);
  }
  SECTION("BGT not taken when N!=V") {
    auto cpu = cpuAt(bus, 0x1000, {0x2E, 0x10});
    cpu.regs().cc = cc::N;
    REQUIRE(cpu.step() == 3);
    REQUIRE(cpu.regs().pc == 0x1002);
  }
  SECTION("BHI taken when C=0 and Z=0") {
    auto cpu = cpuAt(bus, 0x1000, {0x22, 0x10});
    cpu.regs().cc = cc::N;
    cpu.step();
    REQUIRE(cpu.regs().pc == 0x1012);
  }
  SECTION("BSR") {
    auto cpu = cpuAt(bus, 0x1000, {0x8D, 0x10});
    cpu.regs().s = 0x8000;
    REQUIRE(cpu.step() == 7);
    REQUIRE(cpu.regs().pc == 0x1012);
    REQUIRE(bus.get16(0x7FFE) == 0x1002);
  }
}

TEST_CASE("cpu long branches") {
  RamBus bus;
  SECTION("LBNE not taken is 5 cycles") {
    auto cpu = cpuAt(bus, 0x1000, {0x10, 0x26, 0x00, 0x10});
    cpu.regs().cc = cc::Z;
    REQUIRE(cpu.step() == 5);
    REQUIRE(cpu.regs().pc == 0x1004);
  }
  SECTION("LBRA backward") {
    auto cpu = cpuAt(bus, 0x1000, {0x16, 0xFF, 0xFD});
    REQUIRE(cpu.step() == 5);
    REQUIRE(cpu.regs().pc == 0x1000);
  }
  SECTION("LBSR") {
    auto cpu = cpuAt(bus, 0x1000, {0x17, 0x01, 0x00});
    cpu.regs().s = 0x8000;
    REQUIRE(cpu.step() == 9);
    REQUIRE(cpu.regs().pc == 0x1103);
    REQUIRE(bus.get16(0x7FFE) == 0x1003);
  }
}

TEST_CASE("cpu PSHU/PULU all registers and PULS PC") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x36, 0xFF, 0x37, 0xFF});
  auto& r = cpu.regs();
  r.u = 0x6000;
  r.s = 0x7000;
  r.a = 1; r.b = 2; r.dp = 3; r.cc = 0x04; r.x = 0x0506; r.y = 0x0708;
  REQUIRE(cpu.step() == 5 + 12);
  REQUIRE(r.u == 0x6000 - 12);
  REQUIRE(bus.mem[0x5FF4] == 0x04);           // CC lowest
  REQUIRE(bus.get16(0x5FFC) == 0x7000);       // S
  REQUIRE(bus.get16(0x5FFE) == 0x1002);       // PC highest
  r.a = 0; r.x = 0; r.s = 0;
  REQUIRE(cpu.step() == 5 + 12);              // PULU pulls PC = 1002 again
  REQUIRE(r.u == 0x6000);
  REQUIRE(r.a == 1);
  REQUIRE(r.x == 0x0506);
  REQUIRE(r.s == 0x7000);
  REQUIRE(r.pc == 0x1002);
}

TEST_CASE("cpu TFR and EXG 16-bit and CC") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x1F, 0x12, 0x1E, 0x01, 0x1F, 0x8A});
  auto& r = cpu.regs();
  r.x = 0x1234;
  r.setD(0x00FF);
  REQUIRE(cpu.step() == 6);
  REQUIRE(r.y == 0x1234);
  REQUIRE(cpu.step() == 8);
  REQUIRE(r.x == 0x00FF);
  REQUIRE(r.d() == 0x1234);
  r.a = 0x0F;
  cpu.step();  // TFR A,CC
  REQUIRE(r.cc == 0x0F);
}

TEST_CASE("cpu ORCC, ANDCC, TST, BIT, store flags") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000,
                   {0x1C, 0x00, 0x1A, 0x05, 0x4D, 0x85, 0x80, 0xDD, 0x40, 0x10, 0xBF, 0x30, 0x00});
  auto& r = cpu.regs();
  REQUIRE(cpu.step() == 3);
  REQUIRE(r.cc == 0);
  REQUIRE(cpu.step() == 3);
  REQUIRE(r.cc == (cc::Z | cc::C));
  r.a = 0x80;
  REQUIRE(cpu.step() == 2);  // TSTA
  REQUIRE(r.cc == (cc::N | cc::C));
  REQUIRE(cpu.step() == 2);  // BITA #$80
  REQUIRE(r.a == 0x80);
  REQUIRE(r.cc == (cc::N | cc::C));
  r.setD(0x0000);
  r.dp = 0x20;
  REQUIRE(cpu.step() == 5);  // STD <$40
  REQUIRE(bus.get16(0x2040) == 0);
  REQUIRE(r.cc == (cc::Z | cc::C));
  r.y = 0xABCD;
  REQUIRE(cpu.step() == 7);  // STY $3000
  REQUIRE(bus.get16(0x3000) == 0xABCD);
  REQUIRE(r.cc == (cc::N | cc::C));
}

TEST_CASE("cpu 16-bit loads on pages 2 and 3") {
  RamBus bus;
  auto cpu = cpuAt(bus, 0x1000, {0x10, 0x8E, 0x12, 0x34, 0xCE, 0x80, 0x00, 0x11, 0x8C, 0x80, 0x00,
                                 0xCC, 0xFF, 0x00});
  auto& r = cpu.regs();
  REQUIRE(cpu.step() == 4);  // LDY #
  REQUIRE(r.y == 0x1234);
  REQUIRE(cpu.step() == 3);  // LDU #
  REQUIRE(r.u == 0x8000);
  REQUIRE(flag(cpu, cc::N));
  r.s = 0x8000;
  REQUIRE(cpu.step() == 5);  // CMPS #
  REQUIRE(flag(cpu, cc::Z));
  REQUIRE(cpu.step() == 3);  // LDD #
  REQUIRE(r.d() == 0xFF00);
}
