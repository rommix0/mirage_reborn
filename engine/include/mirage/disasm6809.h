#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace mirage {

// One disassembled MC6809 instruction. `text` is the mnemonic padded to 6
// columns followed by the operand ("LDA   ,X+"), or the bare mnemonic for
// inherent instructions ("RTS"). Bytes the CPU would treat as an illegal
// opcode come back as "FCB   $xx" (with the prefix byte too, if any), with
// the same length the CPU consumes for them.
struct Disasm {
  std::string text;
  int length;
};

// Decodes the instruction at `pc` using the CPU's opcode table, so lengths
// and addressing modes always agree with Cpu6809. Branch targets and
// PC-relative indexed operands are shown as absolute addresses.
Disasm disassemble6809(const std::function<uint8_t(uint16_t)>& peek, uint16_t pc);

}  // namespace mirage
