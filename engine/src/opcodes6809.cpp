#include "opcodes6809.h"

#include <array>

namespace mirage {
namespace {

using M = AddrMode;
using Page = std::array<OpInfo, 256>;

constexpr OpInfo kIllegal{"???", M::Illegal, 2};

struct Tables {
  Page p0, p2, p3;
};

void set(Page& p, int op, const char* mnemonic, M mode, int cycles) {
  p[size_t(op)] = OpInfo{mnemonic, mode, uint8_t(cycles)};
}

// Rows $8x-$Fx use the same layout: immediate, direct, indexed, extended.
constexpr M kRowMode[4] = {M::Immediate8, M::Direct, M::Indexed, M::Extended};

// Sets a 4-row group (imm, dir, idx, ext) at base + 0x00/0x10/0x20/0x30.
// cycles[0] < 0 means there is no immediate form.
void setGroup(Page& p, int base, const char* mnemonic, bool wide, const int (&cycles)[4]) {
  for (int row = 0; row < 4; ++row) {
    if (cycles[row] < 0) continue;
    M mode = kRowMode[row];
    if (mode == M::Immediate8 && wide) mode = M::Immediate16;
    set(p, base + row * 0x10, mnemonic, mode, cycles[row]);
  }
}

Tables build() {
  Tables t;
  t.p0.fill(kIllegal);
  t.p2.fill(kIllegal);
  t.p3.fill(kIllegal);
  Page& p = t.p0;

  // Read-modify-write group: $0x direct, $4x A, $5x B, $6x indexed, $7x extended.
  static const char* const rmw[16] = {"NEG", nullptr, nullptr, "COM", "LSR", nullptr,
                                      "ROR", "ASR", "ASL", "ROL", "DEC", nullptr,
                                      "INC", "TST", "JMP", "CLR"};
  static const char* const rmwA[16] = {"NEGA", nullptr, nullptr, "COMA", "LSRA", nullptr,
                                       "RORA", "ASRA", "ASLA", "ROLA", "DECA", nullptr,
                                       "INCA", "TSTA", nullptr, "CLRA"};
  static const char* const rmwB[16] = {"NEGB", nullptr, nullptr, "COMB", "LSRB", nullptr,
                                       "RORB", "ASRB", "ASLB", "ROLB", "DECB", nullptr,
                                       "INCB", "TSTB", nullptr, "CLRB"};
  for (int n = 0; n < 16; ++n) {
    if (!rmw[n]) continue;
    const bool jmp = n == 0x0E;
    set(p, 0x00 + n, rmw[n], M::Direct, jmp ? 3 : 6);
    set(p, 0x60 + n, rmw[n], M::Indexed, jmp ? 3 : 6);
    set(p, 0x70 + n, rmw[n], M::Extended, jmp ? 4 : 7);
    if (rmwA[n]) set(p, 0x40 + n, rmwA[n], M::Inherent, 2);
    if (rmwB[n]) set(p, 0x50 + n, rmwB[n], M::Inherent, 2);
  }

  set(p, 0x12, "NOP", M::Inherent, 2);
  set(p, 0x13, "SYNC", M::Inherent, 4);
  set(p, 0x16, "LBRA", M::Relative16, 5);
  set(p, 0x17, "LBSR", M::Relative16, 9);
  set(p, 0x19, "DAA", M::Inherent, 2);
  set(p, 0x1A, "ORCC", M::Immediate8, 3);
  set(p, 0x1C, "ANDCC", M::Immediate8, 3);
  set(p, 0x1D, "SEX", M::Inherent, 2);
  set(p, 0x1E, "EXG", M::Register, 8);
  set(p, 0x1F, "TFR", M::Register, 6);

  static const char* const br[16] = {"BRA", "BRN", "BHI", "BLS", "BCC", "BCS", "BNE", "BEQ",
                                     "BVC", "BVS", "BPL", "BMI", "BGE", "BLT", "BGT", "BLE"};
  static const char* const lbr[16] = {nullptr, "LBRN", "LBHI", "LBLS", "LBCC", "LBCS",
                                      "LBNE",  "LBEQ", "LBVC", "LBVS", "LBPL", "LBMI",
                                      "LBGE",  "LBLT", "LBGT", "LBLE"};
  for (int n = 0; n < 16; ++n) {
    set(p, 0x20 + n, br[n], M::Relative8, 3);
    if (lbr[n]) set(t.p2, 0x20 + n, lbr[n], M::Relative16, 5);
  }

  set(p, 0x30, "LEAX", M::Indexed, 4);
  set(p, 0x31, "LEAY", M::Indexed, 4);
  set(p, 0x32, "LEAS", M::Indexed, 4);
  set(p, 0x33, "LEAU", M::Indexed, 4);
  set(p, 0x34, "PSHS", M::StackList, 5);
  set(p, 0x35, "PULS", M::StackList, 5);
  set(p, 0x36, "PSHU", M::StackList, 5);
  set(p, 0x37, "PULU", M::StackList, 5);
  set(p, 0x39, "RTS", M::Inherent, 5);
  set(p, 0x3A, "ABX", M::Inherent, 3);
  set(p, 0x3B, "RTI", M::Inherent, 6);
  set(p, 0x3C, "CWAI", M::Immediate8, 20);
  set(p, 0x3D, "MUL", M::Inherent, 11);
  set(p, 0x3F, "SWI", M::Inherent, 19);

  // Accumulator group, $8x-$Bx (A side) and $Cx-$Fx (B side).
  constexpr int k8[4] = {2, 4, 4, 5};
  constexpr int kSt8[4] = {-1, 4, 4, 5};
  static const char* const accA[12] = {"SUBA", "CMPA", "SBCA", nullptr, "ANDA", "BITA",
                                       "LDA",  "STA",  "EORA", "ADCA",  "ORA",  "ADDA"};
  static const char* const accB[12] = {"SUBB", "CMPB", "SBCB", nullptr, "ANDB", "BITB",
                                       "LDB",  "STB",  "EORB", "ADCB",  "ORB",  "ADDB"};
  for (int n = 0; n < 12; ++n) {
    if (!accA[n]) continue;
    setGroup(p, 0x80 + n, accA[n], false, n == 7 ? kSt8 : k8);
    setGroup(p, 0xC0 + n, accB[n], false, n == 7 ? kSt8 : k8);
  }
  setGroup(p, 0x83, "SUBD", true, {4, 6, 6, 7});
  setGroup(p, 0xC3, "ADDD", true, {4, 6, 6, 7});
  setGroup(p, 0x8C, "CMPX", true, {4, 6, 6, 7});
  setGroup(p, 0xCC, "LDD", true, {3, 5, 5, 6});
  set(p, 0x8D, "BSR", M::Relative8, 7);
  setGroup(p, 0x8D, "JSR", false, {-1, 7, 7, 8});
  setGroup(p, 0xCD, "STD", true, {-1, 5, 5, 6});
  setGroup(p, 0x8E, "LDX", true, {3, 5, 5, 6});
  setGroup(p, 0xCE, "LDU", true, {3, 5, 5, 6});
  setGroup(p, 0x8F, "STX", true, {-1, 5, 5, 6});
  setGroup(p, 0xCF, "STU", true, {-1, 5, 5, 6});

  // Page 2 ($10 prefix).
  set(t.p2, 0x3F, "SWI2", M::Inherent, 20);
  setGroup(t.p2, 0x83, "CMPD", true, {5, 7, 7, 8});
  setGroup(t.p2, 0x8C, "CMPY", true, {5, 7, 7, 8});
  setGroup(t.p2, 0x8E, "LDY", true, {4, 6, 6, 7});
  setGroup(t.p2, 0x8F, "STY", true, {-1, 6, 6, 7});
  setGroup(t.p2, 0xCE, "LDS", true, {4, 6, 6, 7});
  setGroup(t.p2, 0xCF, "STS", true, {-1, 6, 6, 7});

  // Page 3 ($11 prefix).
  set(t.p3, 0x3F, "SWI3", M::Inherent, 20);
  setGroup(t.p3, 0x83, "CMPU", true, {5, 7, 7, 8});
  setGroup(t.p3, 0x8C, "CMPS", true, {5, 7, 7, 8});
  return t;
}

const Tables& tables() {
  static const Tables t = build();
  return t;
}

}  // namespace

const OpInfo& opInfo(int page, uint8_t opcode) {
  const Tables& t = tables();
  switch (page) {
    case 0x00: return t.p0[opcode];
    case 0x10: return t.p2[opcode];
    case 0x11: return t.p3[opcode];
    default: return kIllegal;
  }
}

}  // namespace mirage
