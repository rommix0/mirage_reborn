#include "mirage/disasm6809.h"

#include <cstdio>

#include "opcodes6809.h"

namespace mirage {
namespace {

std::string hex8(unsigned v) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "$%02X", v & 0xFF);
  return buf;
}

std::string hex16(unsigned v) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "$%04X", v & 0xFFFF);
  return buf;
}

// Signed 8-bit style offset: "$05" / "-$01".
std::string signedHex8(int v) { return v < 0 ? "-" + hex8(unsigned(-v)) : hex8(unsigned(v)); }

// Reads operand bytes sequentially from the instruction stream.
struct Reader {
  const std::function<uint8_t(uint16_t)>& peek;
  uint16_t pc;
  int length = 0;
  uint8_t u8() {
    ++length;
    return peek(uint16_t(pc + length - 1));
  }
  uint16_t u16() {
    const uint8_t hi = u8();
    return uint16_t(hi << 8 | u8());
  }
  uint16_t next() const { return uint16_t(pc + length); }  // address after what was read
};

// Mirrors Cpu6809::indexedEa: same bytes consumed for every postbyte.
std::string indexed(Reader& r) {
  static const char* const kReg[4] = {"X", "Y", "U", "S"};
  const uint8_t pb = r.u8();
  const std::string reg = kReg[(pb >> 5) & 3];
  if (!(pb & 0x80)) {
    const int off = (pb & 0x10) ? int(pb & 0x1F) - 32 : int(pb & 0x0F);
    return signedHex8(off) + "," + reg;
  }
  const bool indirect = (pb & 0x10) != 0;
  std::string s;
  bool valid = true;
  switch (pb & 0x0F) {
    case 0x0: s = "," + reg + "+"; valid = !indirect; break;
    case 0x1: s = "," + reg + "++"; break;
    case 0x2: s = ",-" + reg; valid = !indirect; break;
    case 0x3: s = ",--" + reg; break;
    case 0x4: s = "," + reg; break;
    case 0x5: s = "B," + reg; break;
    case 0x6: s = "A," + reg; break;
    case 0x8: s = signedHex8(int8_t(r.u8())) + "," + reg; break;
    case 0x9: s = hex16(r.u16()) + "," + reg; break;
    case 0xB: s = "D," + reg; break;
    case 0xC: {
      const int8_t off = int8_t(r.u8());
      s = hex16(unsigned(r.next() + off)) + ",PCR";
      break;
    }
    case 0xD: {
      const uint16_t off = r.u16();
      s = hex16(unsigned(r.next() + off)) + ",PCR";
      break;
    }
    case 0xF: s = hex16(r.u16()); valid = indirect; break;
    default: valid = false; break;  // 0x7, 0xA, 0xE
  }
  if (!valid) return "?" + hex8(pb);  // undefined postbyte (the CPU flags it illegal)
  return indirect ? "[" + s + "]" : s;
}

std::string registerPair(uint8_t pb) {
  static const char* const kReg[16] = {"D", "X", "Y", "U", "S", "PC", "?", "?",
                                       "A", "B", "CC", "DP", "?", "?", "?", "?"};
  return std::string(kReg[pb >> 4]) + "," + kReg[pb & 15];
}

std::string stackList(uint8_t mask, bool userStack) {
  static const char* const kBit[8] = {"CC", "A", "B", "DP", "X", "Y", nullptr, "PC"};
  std::string s;
  for (int bit = 0; bit < 8; ++bit) {
    if (!(mask & (1 << bit))) continue;
    if (!s.empty()) s += ",";
    s += kBit[bit] ? kBit[bit] : (userStack ? "S" : "U");
  }
  return s;
}

}  // namespace

Disasm disassemble6809(const std::function<uint8_t(uint16_t)>& peek, uint16_t pc) {
  Reader r{peek, pc};
  int page = 0;
  uint8_t op = r.u8();
  if (op == 0x10 || op == 0x11) {
    page = op;
    op = r.u8();
  }
  const OpInfo& info = opInfo(page, op);
  if (info.mode == AddrMode::Illegal) {
    std::string text = "FCB   " + hex8(page ? unsigned(page) : op);
    if (page) text += "," + hex8(op);
    return {text, r.length};
  }

  std::string operand;
  switch (info.mode) {
    case AddrMode::Inherent: break;
    case AddrMode::Immediate8: operand = "#" + hex8(r.u8()); break;
    case AddrMode::Immediate16: operand = "#" + hex16(r.u16()); break;
    case AddrMode::Direct: operand = "<" + hex8(r.u8()); break;
    case AddrMode::Extended: operand = hex16(r.u16()); break;
    case AddrMode::Indexed: operand = indexed(r); break;
    case AddrMode::Relative8: {
      const int8_t off = int8_t(r.u8());
      operand = hex16(unsigned(r.next() + off));
      break;
    }
    case AddrMode::Relative16: {
      const uint16_t off = r.u16();
      operand = hex16(unsigned(r.next() + off));
      break;
    }
    case AddrMode::Register: operand = registerPair(r.u8()); break;
    case AddrMode::StackList: operand = stackList(r.u8(), op == 0x36 || op == 0x37); break;
    case AddrMode::Illegal: break;
  }

  std::string text = info.mnemonic;
  if (!operand.empty()) {
    text.resize(6, ' ');
    text += operand;
  }
  return {text, r.length};
}

}  // namespace mirage
