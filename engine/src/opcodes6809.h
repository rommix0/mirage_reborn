#pragma once
#include <cstdint>

namespace mirage {

// How an opcode's operand bytes are encoded.
enum class AddrMode : uint8_t {
  Inherent,
  Immediate8,
  Immediate16,
  Direct,
  Extended,
  Indexed,     // postbyte (+ 0..2 offset bytes); see the MC6809E indexed table
  Relative8,
  Relative16,
  Register,    // TFR/EXG register postbyte
  StackList,   // PSH/PUL register-mask postbyte
  Illegal,
};

// One row of the MC6809E opcode map. `cycles` is the datasheet base count
// including any $10/$11 prefix byte; it excludes indexed-mode extra cycles,
// the +1 of a taken long conditional branch, PSH/PUL per-byte cycles and the
// +9 of RTI with E set. SYNC and CWAI list the datasheet minimum.
struct OpInfo {
  const char* mnemonic;
  AddrMode mode;
  uint8_t cycles;
};

// page is 0, 0x10 or 0x11. On page 0, opcodes 0x10 and 0x11 are prefixes
// (reported as Illegal here): decode the following byte with that page.
// Unknown pages and undefined opcodes return an Illegal row.
const OpInfo& opInfo(int page, uint8_t opcode);

}  // namespace mirage
