#include <catch2/catch_test_macros.hpp>

#include <array>
#include <initializer_list>
#include <string>
#include <string_view>

#include "../engine/src/opcodes6809.h"
#include "mirage/cpu6809.h"
#include "mirage/disasm6809.h"

using namespace mirage;

namespace {

struct Mem : Bus {
  std::array<uint8_t, 65536> mem{};
  uint8_t read(uint16_t a) override { return mem[a]; }
  void write(uint16_t a, uint8_t v) override { mem[a] = v; }
};

Disasm dis(std::initializer_list<uint8_t> code, uint16_t pc = 0x1000) {
  Mem m;
  uint16_t a = pc;
  for (uint8_t b : code) m.mem[a++] = b;
  return disassemble6809([&](uint16_t addr) { return m.mem[addr]; }, pc);
}

}  // namespace

TEST_CASE("disasm immediate") {
  const Disasm d = dis({0x86, 0x80});
  REQUIRE(d.text == "LDA   #$80");
  REQUIRE(d.length == 2);
  REQUIRE(dis({0x8E, 0x12, 0x34}).text == "LDX   #$1234");
}

TEST_CASE("disasm long branch shows the absolute target") {
  const Disasm d = dis({0x10, 0x27, 0x00, 0x10}, 0x1000);
  REQUIRE(d.text == "LBEQ  $1014");
  REQUIRE(d.length == 4);
  REQUIRE(dis({0x20, 0xFE}, 0x2000).text == "BRA   $2000");
  REQUIRE(dis({0x16, 0xFF, 0xFD}, 0x2000).text == "LBRA  $2000");
}

TEST_CASE("disasm extended indirect") {
  const Disasm d = dis({0xA6, 0x9F, 0x30, 0x00});
  REQUIRE(d.text == "LDA   [$3000]");
  REQUIRE(d.length == 4);
}

TEST_CASE("disasm illegal opcode is a data byte") {
  const Disasm d = dis({0x01});
  REQUIRE(d.text == "FCB   $01");
  REQUIRE(d.length == 1);
  const Disasm p = dis({0x10, 0x01});
  REQUIRE(p.text == "FCB   $10,$01");
  REQUIRE(p.length == 2);
}

TEST_CASE("disasm indexed forms") {
  REQUIRE(dis({0xA6, 0x80}).text == "LDA   ,X+");
  REQUIRE(dis({0xA6, 0xA1}).text == "LDA   ,Y++");
  REQUIRE(dis({0xA6, 0xC2}).text == "LDA   ,-U");
  REQUIRE(dis({0xA6, 0xE3}).text == "LDA   ,--S");
  REQUIRE(dis({0xA6, 0x84}).text == "LDA   ,X");
  REQUIRE(dis({0xA6, 0x85}).text == "LDA   B,X");
  REQUIRE(dis({0xA6, 0x86}).text == "LDA   A,X");
  REQUIRE(dis({0xA6, 0x8B}).text == "LDA   D,X");
  REQUIRE(dis({0xA6, 0x05}).text == "LDA   $05,X");
  REQUIRE(dis({0xA6, 0x1F}).text == "LDA   -$01,X");
  REQUIRE(dis({0xA6, 0x88, 0x80}).text == "LDA   -$80,X");
  REQUIRE(dis({0xA6, 0x89, 0x12, 0x34}).text == "LDA   $1234,X");
  REQUIRE(dis({0xA6, 0x94}).text == "LDA   [,X]");
  REQUIRE(dis({0x30, 0x8C, 0x0E}, 0x1000).text == "LEAX  $1011,PCR");
  REQUIRE(dis({0x30, 0x8D, 0xFF, 0xFC}, 0x1000).text == "LEAX  $1000,PCR");
  REQUIRE(dis({0xA6, 0x89, 0x12, 0x34}).length == 4);
}

TEST_CASE("disasm direct, extended, register and stack operands") {
  REQUIRE(dis({0x96, 0x12}).text == "LDA   <$12");
  REQUIRE(dis({0xB7, 0xE2, 0x00}).text == "STA   $E200");
  REQUIRE(dis({0x1F, 0x12}).text == "TFR   X,Y");
  REQUIRE(dis({0x1E, 0x89}).text == "EXG   A,B");
  REQUIRE(dis({0x34, 0x16}).text == "PSHS  A,B,X");
  REQUIRE(dis({0x36, 0xC0}).text == "PSHU  S,PC");
  REQUIRE(dis({0x35, 0xFF}).text == "PULS  CC,A,B,DP,X,Y,U,PC");
  REQUIRE(dis({0x39}).text == "RTS");
  REQUIRE(dis({0x39}).length == 1);
  REQUIRE(dis({0x10, 0xCE, 0x80, 0x00}).text == "LDS   #$8000");
}

// The disassembler must agree with the CPU on instruction length for every
// defined opcode: run each one (non-control-flow) and compare the PC advance.
TEST_CASE("disasm length matches the CPU for every opcode") {
  static constexpr std::string_view kFlow[] = {"JMP", "JSR", "RTS", "RTI", "SWI", "SWI2",
                                               "SWI3", "CWAI", "SYNC", "BSR", "LBSR"};
  static constexpr uint8_t kPostbytes[] = {0x84, 0x05, 0x80, 0x88, 0x89, 0x8C,
                                           0x8D, 0x9F, 0x94, 0x87, 0x8F, 0x90};
  int checked = 0;
  for (int page : {0x00, 0x10, 0x11}) {
    for (int op = 0; op < 256; ++op) {
      if (page == 0 && (op == 0x10 || op == 0x11)) continue;
      const OpInfo& info = opInfo(page, uint8_t(op));
      bool flow = info.mode == AddrMode::Relative8 || info.mode == AddrMode::Relative16;
      for (auto f : kFlow) flow = flow || f == info.mnemonic;
      if (flow) continue;
      for (uint8_t pb : kPostbytes) {
        Mem m;
        uint16_t a = 0x1000;
        if (page) m.mem[a++] = uint8_t(page);
        m.mem[a++] = uint8_t(op);
        // Postbyte: indexed form, or a safe register/stack mask (no PC).
        uint8_t post = pb;
        if (info.mode == AddrMode::Register) post = 0x12;
        if (info.mode == AddrMode::StackList) post = 0x06;
        m.mem[a++] = post;
        m.mem[0xFFFE] = 0x10;
        m.mem[0xFFFF] = 0x00;
        Cpu6809 cpu(m);
        cpu.reset();
        cpu.regs().s = 0x8000;
        cpu.regs().u = 0x9000;
        const Disasm d = disassemble6809([&](uint16_t x) { return m.mem[x]; }, 0x1000);
        cpu.step();
        INFO("page " << page << " op " << op << " postbyte " << int(post) << " -> " << d.text);
        REQUIRE(int(cpu.regs().pc) - 0x1000 == d.length);
        ++checked;
        if (info.mode != AddrMode::Indexed) break;
      }
    }
  }
  REQUIRE(checked > 300);
}
