// MC6809E instruction semantics. Operand addressing and base cycle counts come
// from the shared opcode table (opcodes6809.cpp) via Cpu6809::execute(); each
// exec function returns only the cycles the table cannot know in advance.
#include "mirage/cpu6809.h"

#include "opcodes6809.h"

namespace mirage {
namespace {

constexpr uint16_t kVecSwi3 = 0xFFF2, kVecSwi2 = 0xFFF4, kVecSwi = 0xFFFA;

// ---- Flag helpers ---------------------------------------------------------------

void setFlag(uint8_t& ccr, uint8_t bit, bool on) {
  if (on) {
    ccr |= bit;
  } else {
    ccr &= uint8_t(~bit);
  }
}

void nz8(uint8_t& ccr, uint8_t v) {
  setFlag(ccr, cc::N, (v & 0x80) != 0);
  setFlag(ccr, cc::Z, v == 0);
}

void nz16(uint8_t& ccr, uint16_t v) {
  setFlag(ccr, cc::N, (v & 0x8000) != 0);
  setFlag(ccr, cc::Z, v == 0);
}

// LD/ST/AND/OR/EOR/BIT/TST: N, Z; V cleared; C untouched.
uint8_t logic8(uint8_t& ccr, uint8_t v) {
  nz8(ccr, v);
  ccr &= uint8_t(~cc::V);
  return v;
}

uint16_t logic16(uint8_t& ccr, uint16_t v) {
  nz16(ccr, v);
  ccr &= uint8_t(~cc::V);
  return v;
}

// ADD/ADC: H N Z V C.
uint8_t add8(uint8_t& ccr, uint8_t a, uint8_t m, bool withCarry) {
  const unsigned c = (withCarry && (ccr & cc::C)) ? 1u : 0u;
  const unsigned r = unsigned(a) + m + c;
  setFlag(ccr, cc::H, ((a ^ m ^ r) & 0x10) != 0);
  setFlag(ccr, cc::V, ((a ^ r) & (m ^ r) & 0x80) != 0);
  setFlag(ccr, cc::C, (r & 0x100) != 0);
  nz8(ccr, uint8_t(r));
  return uint8_t(r);
}

// SUB/SBC/CMP/NEG: N Z V C (C = borrow); H is undefined and left alone.
uint8_t sub8(uint8_t& ccr, uint8_t a, uint8_t m, bool withCarry) {
  const unsigned c = (withCarry && (ccr & cc::C)) ? 1u : 0u;
  const unsigned r = unsigned(a) - m - c;
  setFlag(ccr, cc::V, ((a ^ m) & (a ^ r) & 0x80) != 0);
  setFlag(ccr, cc::C, (r & 0x100) != 0);
  nz8(ccr, uint8_t(r));
  return uint8_t(r);
}

// ADDD: N Z V C; H not affected.
uint16_t add16(uint8_t& ccr, uint16_t a, uint16_t m) {
  const uint32_t r = uint32_t(a) + m;
  setFlag(ccr, cc::V, ((a ^ r) & (m ^ r) & 0x8000) != 0);
  setFlag(ccr, cc::C, (r & 0x10000) != 0);
  nz16(ccr, uint16_t(r));
  return uint16_t(r);
}

// SUBD/CMPD/CMPX/CMPY/CMPU/CMPS: N Z V C.
uint16_t sub16(uint8_t& ccr, uint16_t a, uint16_t m) {
  const uint32_t r = uint32_t(a) - m;
  setFlag(ccr, cc::V, ((a ^ m) & (a ^ r) & 0x8000) != 0);
  setFlag(ccr, cc::C, (r & 0x10000) != 0);
  nz16(ccr, uint16_t(r));
  return uint16_t(r);
}

// The read-modify-write unary ops, selected by the opcode's low nibble.
uint8_t unary(uint8_t& ccr, int op, uint8_t v) {
  switch (op) {
    case 0x0:  // NEG: V iff v==$80, C iff v!=0
      return sub8(ccr, 0, v, false);
    case 0x3:  // COM
      setFlag(ccr, cc::C, true);
      return logic8(ccr, uint8_t(~v));
    case 0x4: {  // LSR: N cleared, V untouched
      setFlag(ccr, cc::C, (v & 1) != 0);
      const uint8_t r = uint8_t(v >> 1);
      nz8(ccr, r);
      return r;
    }
    case 0x6: {  // ROR: V untouched
      const uint8_t r = uint8_t((v >> 1) | ((ccr & cc::C) ? 0x80 : 0));
      setFlag(ccr, cc::C, (v & 1) != 0);
      nz8(ccr, r);
      return r;
    }
    case 0x7: {  // ASR: V untouched
      const uint8_t r = uint8_t((v >> 1) | (v & 0x80));
      setFlag(ccr, cc::C, (v & 1) != 0);
      nz8(ccr, r);
      return r;
    }
    case 0x8: {  // ASL/LSL: V = b7 ^ b6
      const uint8_t r = uint8_t(v << 1);
      setFlag(ccr, cc::C, (v & 0x80) != 0);
      setFlag(ccr, cc::V, ((v ^ r) & 0x80) != 0);
      nz8(ccr, r);
      return r;
    }
    case 0x9: {  // ROL: V = b7 ^ b6
      const uint8_t r = uint8_t((v << 1) | ((ccr & cc::C) ? 1 : 0));
      setFlag(ccr, cc::C, (v & 0x80) != 0);
      setFlag(ccr, cc::V, ((v ^ (v << 1)) & 0x80) != 0);
      nz8(ccr, r);
      return r;
    }
    case 0xA: {  // DEC: C untouched
      const uint8_t r = uint8_t(v - 1);
      setFlag(ccr, cc::V, v == 0x80);
      nz8(ccr, r);
      return r;
    }
    case 0xC: {  // INC: C untouched
      const uint8_t r = uint8_t(v + 1);
      setFlag(ccr, cc::V, v == 0x7F);
      nz8(ccr, r);
      return r;
    }
    case 0xD:  // TST
      return logic8(ccr, v);
    default:  // 0xF CLR
      ccr &= uint8_t(~(cc::N | cc::V | cc::C));
      ccr |= cc::Z;
      return 0;
  }
}

}  // namespace

// ---- Groups -----------------------------------------------------------------------

bool Cpu6809::branchTaken(int cond) const {
  const bool n = (r_.cc & cc::N) != 0, z = (r_.cc & cc::Z) != 0;
  const bool v = (r_.cc & cc::V) != 0, c = (r_.cc & cc::C) != 0;
  switch (cond & 0x0F) {
    case 0x0: return true;          // BRA
    case 0x1: return false;         // BRN
    case 0x2: return !(c || z);     // BHI
    case 0x3: return c || z;        // BLS
    case 0x4: return !c;            // BCC/BHS
    case 0x5: return c;             // BCS/BLO
    case 0x6: return !z;            // BNE
    case 0x7: return z;             // BEQ
    case 0x8: return !v;            // BVC
    case 0x9: return v;             // BVS
    case 0xA: return !n;            // BPL
    case 0xB: return n;             // BMI
    case 0xC: return n == v;        // BGE
    case 0xD: return n != v;        // BLT
    case 0xE: return !z && n == v;  // BGT
    default: return z || n != v;    // BLE
  }
}

// TFR/EXG register codes. Undefined codes read $FFFF and ignore writes; an
// 8-bit register read as 16 bits is $FF:value, a 16-bit one written to an
// 8-bit register keeps the low byte (mixed sizes are undefined on silicon).
uint16_t Cpu6809::regByCode(int code) const {
  switch (code) {
    case 0x0: return r_.d();
    case 0x1: return r_.x;
    case 0x2: return r_.y;
    case 0x3: return r_.u;
    case 0x4: return r_.s;
    case 0x5: return r_.pc;
    case 0x8: return uint16_t(0xFF00 | r_.a);
    case 0x9: return uint16_t(0xFF00 | r_.b);
    case 0xA: return uint16_t(0xFF00 | r_.cc);
    case 0xB: return uint16_t(0xFF00 | r_.dp);
    default: return 0xFFFF;
  }
}

void Cpu6809::setRegByCode(int code, uint16_t v) {
  switch (code) {
    case 0x0: r_.setD(v); break;
    case 0x1: r_.x = v; break;
    case 0x2: r_.y = v; break;
    case 0x3: r_.u = v; break;
    case 0x4: setS(v); break;
    case 0x5: r_.pc = v; break;
    case 0x8: r_.a = uint8_t(v); break;
    case 0x9: r_.b = uint8_t(v); break;
    case 0xA: r_.cc = uint8_t(v); break;
    case 0xB: r_.dp = uint8_t(v); break;
    default: break;
  }
}

// Push order PC, U/S, Y, X, DP, B, A, CC (CC ends at the lowest address).
int Cpu6809::pushList(bool systemStack, uint8_t mask) {
  uint16_t& sp = systemStack ? r_.s : r_.u;
  const uint16_t other = systemStack ? r_.u : r_.s;
  int bytes = 0;
  if (mask & 0x80) { push16(sp, r_.pc); bytes += 2; }
  if (mask & 0x40) { push16(sp, other); bytes += 2; }
  if (mask & 0x20) { push16(sp, r_.y); bytes += 2; }
  if (mask & 0x10) { push16(sp, r_.x); bytes += 2; }
  if (mask & 0x08) { push8(sp, r_.dp); bytes += 1; }
  if (mask & 0x04) { push8(sp, r_.b); bytes += 1; }
  if (mask & 0x02) { push8(sp, r_.a); bytes += 1; }
  if (mask & 0x01) { push8(sp, r_.cc); bytes += 1; }
  return bytes;
}

int Cpu6809::pullList(bool systemStack, uint8_t mask) {
  uint16_t& sp = systemStack ? r_.s : r_.u;
  int bytes = 0;
  if (mask & 0x01) { r_.cc = pull8(sp); bytes += 1; }
  if (mask & 0x02) { r_.a = pull8(sp); bytes += 1; }
  if (mask & 0x04) { r_.b = pull8(sp); bytes += 1; }
  if (mask & 0x08) { r_.dp = pull8(sp); bytes += 1; }
  if (mask & 0x10) { r_.x = pull16(sp); bytes += 2; }
  if (mask & 0x20) { r_.y = pull16(sp); bytes += 2; }
  if (mask & 0x40) {
    const uint16_t v = pull16(sp);
    if (systemStack) {
      r_.u = v;
    } else {
      setS(v);  // PULU S loads S
    }
    bytes += 2;
  }
  if (mask & 0x80) { r_.pc = pull16(sp); bytes += 2; }
  return bytes;
}

int Cpu6809::swi(uint16_t vector, bool maskIrqs) {
  r_.cc |= cc::E;
  pushEntireState();
  if (maskIrqs) r_.cc |= cc::I | cc::F;
  r_.pc = read16(vector);
  return 0;
}

// $0x, $4x-$7x: NEG COM LSR ROR ASR ASL ROL DEC INC TST JMP CLR.
void Cpu6809::execRmw(uint8_t op) {
  const int fn = op & 0x0F;
  if (fn == 0x0E) {  // JMP
    r_.pc = ea_;
    return;
  }
  const int group = op >> 4;
  if (group == 0x4 || group == 0x5) {
    uint8_t& acc = group == 0x4 ? r_.a : r_.b;
    acc = unary(r_.cc, fn, acc);
    return;
  }
  // Memory forms read first, CLR included, as the silicon does.
  const uint8_t r = unary(r_.cc, fn, read8(ea_));
  if (fn != 0x0D) bus_.write(ea_, r);  // TST only reads
}

// $8x-$Fx accumulator and 16-bit register group.
int Cpu6809::execAlu(uint8_t op) {
  const bool bSide = (op & 0x40) != 0;
  uint8_t& acc = bSide ? r_.b : r_.a;
  uint8_t& ccr = r_.cc;
  switch (op & 0x0F) {
    case 0x0: acc = sub8(ccr, acc, operand8(), false); break;   // SUB
    case 0x1: sub8(ccr, acc, operand8(), false); break;         // CMP
    case 0x2: acc = sub8(ccr, acc, operand8(), true); break;    // SBC
    case 0x3:                                                   // SUBD / ADDD
      r_.setD(bSide ? add16(ccr, r_.d(), operand16()) : sub16(ccr, r_.d(), operand16()));
      break;
    case 0x4: acc = logic8(ccr, uint8_t(acc & operand8())); break;  // AND
    case 0x5: logic8(ccr, uint8_t(acc & operand8())); break;        // BIT
    case 0x6: acc = logic8(ccr, operand8()); break;                 // LD
    case 0x7: bus_.write(ea_, logic8(ccr, acc)); break;             // ST
    case 0x8: acc = logic8(ccr, uint8_t(acc ^ operand8())); break;  // EOR
    case 0x9: acc = add8(ccr, acc, operand8(), true); break;        // ADC
    case 0xA: acc = logic8(ccr, uint8_t(acc | operand8())); break;  // OR
    case 0xB: acc = add8(ccr, acc, operand8(), false); break;       // ADD
    case 0xC:                                                       // LDD / CMPX
      if (bSide) {
        r_.setD(logic16(ccr, operand16()));
      } else {
        sub16(ccr, r_.x, operand16());
      }
      break;
    case 0xD:
      if (bSide) {  // STD
        write16(ea_, logic16(ccr, r_.d()));
      } else if (mode_ == AddrMode::Relative8) {  // BSR
        const int8_t off = int8_t(fetch8());
        push16(r_.s, r_.pc);
        r_.pc = uint16_t(r_.pc + off);
      } else {  // JSR
        push16(r_.s, r_.pc);
        r_.pc = ea_;
      }
      break;
    case 0xE:  // LDU / LDX
      (bSide ? r_.u : r_.x) = logic16(ccr, operand16());
      break;
    default:  // 0xF STU / STX
      write16(ea_, logic16(ccr, bSide ? r_.u : r_.x));
      break;
  }
  return 0;
}

int Cpu6809::execPage0(uint8_t op) {
  const int group = op >> 4;
  if (group == 0x0 || (group >= 0x4 && group <= 0x7)) {
    execRmw(op);
    return 0;
  }
  if (group >= 0x8) return execAlu(op);
  if (group == 0x2) {
    const int8_t off = int8_t(fetch8());
    if (branchTaken(op)) r_.pc = uint16_t(r_.pc + off);
    return 0;
  }
  switch (op) {
    case 0x12: return 0;  // NOP
    case 0x13:            // SYNC
      wait_ = Wait::Sync;
      return -kSyncWakeCycles;
    case 0x16: {  // LBRA
      const uint16_t off = fetch16();
      r_.pc = uint16_t(r_.pc + off);
      return 0;
    }
    case 0x17: {  // LBSR
      const uint16_t off = fetch16();
      push16(r_.s, r_.pc);
      r_.pc = uint16_t(r_.pc + off);
      return 0;
    }
    case 0x19: {  // DAA (V cleared; C only ever set)
      const unsigned a = r_.a, lsn = a & 0x0F, msn = a >> 4;
      unsigned fix = 0;
      if ((r_.cc & cc::H) || lsn > 9) fix |= 0x06;
      if ((r_.cc & cc::C) || msn > 9 || (msn > 8 && lsn > 9)) fix |= 0x60;
      const unsigned r = a + fix;
      if (r & 0x100) r_.cc |= cc::C;
      r_.a = logic8(r_.cc, uint8_t(r));
      return 0;
    }
    case 0x1A: r_.cc |= fetch8(); return 0;   // ORCC
    case 0x1C: r_.cc &= fetch8(); return 0;   // ANDCC
    case 0x1D:                                // SEX (V, C untouched)
      r_.a = (r_.b & 0x80) ? 0xFF : 0x00;
      nz16(r_.cc, r_.d());
      return 0;
    case 0x1E: {  // EXG
      const uint8_t pb = fetch8();
      const uint16_t v1 = regByCode(pb >> 4), v2 = regByCode(pb & 0x0F);
      setRegByCode(pb >> 4, v2);
      setRegByCode(pb & 0x0F, v1);
      return 0;
    }
    case 0x1F: {  // TFR
      const uint8_t pb = fetch8();
      setRegByCode(pb & 0x0F, regByCode(pb >> 4));
      return 0;
    }
    case 0x30:  // LEAX
      r_.x = ea_;
      setFlag(r_.cc, cc::Z, ea_ == 0);
      return 0;
    case 0x31:  // LEAY
      r_.y = ea_;
      setFlag(r_.cc, cc::Z, ea_ == 0);
      return 0;
    case 0x32: setS(ea_); return 0;   // LEAS
    case 0x33: r_.u = ea_; return 0;  // LEAU
    case 0x34: return pushList(true, fetch8());   // PSHS
    case 0x35: return pullList(true, fetch8());   // PULS
    case 0x36: return pushList(false, fetch8());  // PSHU
    case 0x37: return pullList(false, fetch8());  // PULU
    case 0x39: r_.pc = pull16(r_.s); return 0;    // RTS
    case 0x3A: r_.x = uint16_t(r_.x + r_.b); return 0;  // ABX
    case 0x3B: {                                         // RTI
      r_.cc = pull8(r_.s);
      if (r_.cc & cc::E) {
        r_.a = pull8(r_.s);
        r_.b = pull8(r_.s);
        r_.dp = pull8(r_.s);
        r_.x = pull16(r_.s);
        r_.y = pull16(r_.s);
        r_.u = pull16(r_.s);
        r_.pc = pull16(r_.s);
        return 9;
      }
      r_.pc = pull16(r_.s);
      return 0;
    }
    case 0x3C:  // CWAI
      r_.cc &= fetch8();
      r_.cc |= cc::E;
      pushEntireState();
      wait_ = Wait::Cwai;
      return -kCwaiWakeCycles;
    case 0x3D: {  // MUL
      r_.setD(uint16_t(r_.a * r_.b));
      setFlag(r_.cc, cc::Z, r_.d() == 0);
      setFlag(r_.cc, cc::C, (r_.b & 0x80) != 0);
      return 0;
    }
    case 0x3F: return swi(kVecSwi, true);
    default: return 0;  // unreachable: the table marks everything else illegal
  }
}

int Cpu6809::execPage2(uint8_t op) {
  if ((op >> 4) == 0x2) {  // long conditional branches: +1 cycle when taken
    const uint16_t off = fetch16();
    if (!branchTaken(op)) return 0;
    r_.pc = uint16_t(r_.pc + off);
    return 1;
  }
  if (op == 0x3F) return swi(kVecSwi2, false);
  const bool sSide = (op & 0x40) != 0;
  switch (op & 0x0F) {
    case 0x3: sub16(r_.cc, r_.d(), operand16()); break;  // CMPD
    case 0xC: sub16(r_.cc, r_.y, operand16()); break;    // CMPY
    case 0xE: {                                          // LDS / LDY
      const uint16_t v = logic16(r_.cc, operand16());
      if (sSide) {
        setS(v);
      } else {
        r_.y = v;
      }
      break;
    }
    default:  // 0xF STS / STY
      write16(ea_, logic16(r_.cc, sSide ? r_.s : r_.y));
      break;
  }
  return 0;
}

int Cpu6809::execPage3(uint8_t op) {
  if (op == 0x3F) return swi(kVecSwi3, false);
  sub16(r_.cc, (op & 0x0F) == 0x3 ? r_.u : r_.s, operand16());  // CMPU / CMPS
  return 0;
}

}  // namespace mirage
