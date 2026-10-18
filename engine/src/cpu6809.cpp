// MC6809E core: bus access, addressing, interrupts, wait states, save/load.
// Instruction semantics live in cpu6809_ops.cpp.
#include "mirage/cpu6809.h"

#include "opcodes6809.h"

namespace mirage {
namespace {

constexpr uint16_t kVecFirq = 0xFFF6, kVecIrq = 0xFFF8, kVecNmi = 0xFFFC, kVecReset = 0xFFFE;
constexpr int kIrqCycles = 19, kNmiCycles = 19, kFirqCycles = 10;
constexpr int kIllegalCycles = 2;

}  // namespace

// ---- Bus helpers --------------------------------------------------------------

uint16_t Cpu6809::read16(uint16_t addr) {
  const uint8_t hi = read8(addr);
  const uint8_t lo = read8(uint16_t(addr + 1));
  return uint16_t(hi << 8 | lo);
}

void Cpu6809::write16(uint16_t addr, uint16_t v) {
  bus_.write(addr, uint8_t(v >> 8));
  bus_.write(uint16_t(addr + 1), uint8_t(v));
}

uint8_t Cpu6809::fetch8() {
  const uint8_t v = read8(r_.pc);
  r_.pc = uint16_t(r_.pc + 1);
  return v;
}

uint16_t Cpu6809::fetch16() {
  const uint8_t hi = fetch8();
  const uint8_t lo = fetch8();
  return uint16_t(hi << 8 | lo);
}

void Cpu6809::push8(uint16_t& sp, uint8_t v) {
  sp = uint16_t(sp - 1);
  bus_.write(sp, v);
}

void Cpu6809::push16(uint16_t& sp, uint16_t v) {
  push8(sp, uint8_t(v));
  push8(sp, uint8_t(v >> 8));
}

uint8_t Cpu6809::pull8(uint16_t& sp) {
  const uint8_t v = read8(sp);
  sp = uint16_t(sp + 1);
  return v;
}

uint16_t Cpu6809::pull16(uint16_t& sp) {
  const uint8_t hi = pull8(sp);
  const uint8_t lo = pull8(sp);
  return uint16_t(hi << 8 | lo);
}

void Cpu6809::pushEntireState() {
  push16(r_.s, r_.pc);
  push16(r_.s, r_.u);
  push16(r_.s, r_.y);
  push16(r_.s, r_.x);
  push8(r_.s, r_.dp);
  push8(r_.s, r_.b);
  push8(r_.s, r_.a);
  push8(r_.s, r_.cc);
}

void Cpu6809::setS(uint16_t v) {
  r_.s = v;
  nmiArmed_ = true;
}

void Cpu6809::noteIllegal(uint16_t pc) {
  ++illegalCount_;
  lastIllegalPc_ = pc;
}

// ---- Lifecycle and interrupt lines ------------------------------------------

void Cpu6809::reset() {
  r_.cc |= cc::I | cc::F;
  r_.dp = 0;
  wait_ = Wait::None;
  nmiArmed_ = false;
  nmiPending_ = false;
  r_.pc = read16(kVecReset);
}

void Cpu6809::powerOn() {
  r_ = Regs{};
  wait_ = Wait::None;
  nmiLine_ = nmiPending_ = nmiArmed_ = false;
  irqLine_ = firqLine_ = false;
  illegalCount_ = 0;
  lastIllegalPc_ = 0;
  reset();
}

void Cpu6809::setNmi(bool line) {
  // Edges arriving before the first load of S are dropped, not deferred.
  if (line && !nmiLine_ && nmiArmed_) nmiPending_ = true;
  nmiLine_ = line;
}

void Cpu6809::setIrq(bool line) { irqLine_ = line; }
void Cpu6809::setFirq(bool line) { firqLine_ = line; }

int Cpu6809::serviceInterrupt() {
  if (nmiPending_) {
    nmiPending_ = false;
    r_.cc |= cc::E;
    pushEntireState();
    r_.cc |= cc::I | cc::F;
    r_.pc = read16(kVecNmi);
    return kNmiCycles;
  }
  if (firqLine_ && !(r_.cc & cc::F)) {
    r_.cc &= uint8_t(~cc::E);
    push16(r_.s, r_.pc);
    push8(r_.s, r_.cc);
    r_.cc |= cc::I | cc::F;
    r_.pc = read16(kVecFirq);
    return kFirqCycles;
  }
  if (irqLine_ && !(r_.cc & cc::I)) {
    r_.cc |= cc::E;
    pushEntireState();
    r_.cc |= cc::I;
    r_.pc = read16(kVecIrq);
    return kIrqCycles;
  }
  return 0;
}

int Cpu6809::step() {
  if (wait_ == Wait::Cwai) {
    // The entire state (E=1) was stacked by CWAI; just mask and vector.
    uint16_t vector;
    if (nmiPending_) {
      nmiPending_ = false;
      r_.cc |= cc::I | cc::F;
      vector = kVecNmi;
    } else if (firqLine_ && !(r_.cc & cc::F)) {
      r_.cc |= cc::I | cc::F;
      vector = kVecFirq;
    } else if (irqLine_ && !(r_.cc & cc::I)) {
      r_.cc |= cc::I;
      vector = kVecIrq;
    } else {
      return 1;
    }
    wait_ = Wait::None;
    r_.pc = read16(vector);
    return kCwaiWakeCycles;
  }
  if (wait_ == Wait::Sync) {
    // Any interrupt line ends SYNC, masked or not; a masked one just resumes.
    if (!nmiPending_ && !firqLine_ && !irqLine_) return 1;
    wait_ = Wait::None;
    return kSyncWakeCycles;
  }
  if (const int cycles = serviceInterrupt()) return cycles;
  return execute();
}

// ---- Decode -------------------------------------------------------------------

int Cpu6809::execute() {
  const uint16_t opPc = r_.pc;
  int page = 0;
  uint8_t op = fetch8();
  if (op == 0x10 || op == 0x11) {
    page = op;
    op = fetch8();
  }
  const OpInfo& info = opInfo(page, op);
  if (info.mode == AddrMode::Illegal) {
    noteIllegal(opPc);
    return kIllegalCycles;
  }
  int cycles = info.cycles;
  mode_ = info.mode;
  switch (info.mode) {
    case AddrMode::Direct: ea_ = uint16_t(r_.dp << 8 | fetch8()); break;
    case AddrMode::Extended: ea_ = fetch16(); break;
    case AddrMode::Indexed: ea_ = indexedEa(cycles, opPc); break;
    default: ea_ = 0; break;  // other operands are fetched by the instruction
  }
  switch (page) {
    case 0x10: cycles += execPage2(op); break;
    case 0x11: cycles += execPage3(op); break;
    default: cycles += execPage0(op); break;
  }
  return cycles;
}

// Indexed postbyte decode; adds the datasheet's extra cycles to `cycles`.
// Undefined postbytes are counted as illegal and given a best-effort address.
uint16_t Cpu6809::indexedEa(int& cycles, uint16_t opPc) {
  const uint8_t pb = fetch8();
  uint16_t* regs[4] = {&r_.x, &r_.y, &r_.u, &r_.s};
  uint16_t& reg = *regs[(pb >> 5) & 3];

  if (!(pb & 0x80)) {  // 5-bit signed offset, never indirect
    const int off = (pb & 0x10) ? int(pb & 0x1F) - 32 : int(pb & 0x0F);
    cycles += 1;
    return uint16_t(reg + off);
  }

  const bool indirect = (pb & 0x10) != 0;
  uint16_t ea = 0;
  switch (pb & 0x0F) {
    case 0x0:  // ,R+  ([,R+] is undefined)
      ea = reg;
      reg = uint16_t(reg + 1);
      cycles += 2;
      if (indirect) noteIllegal(opPc);
      break;
    case 0x1:  // ,R++
      ea = reg;
      reg = uint16_t(reg + 2);
      cycles += 3;
      break;
    case 0x2:  // ,-R  ([,-R] is undefined)
      reg = uint16_t(reg - 1);
      ea = reg;
      cycles += 2;
      if (indirect) noteIllegal(opPc);
      break;
    case 0x3:  // ,--R
      reg = uint16_t(reg - 2);
      ea = reg;
      cycles += 3;
      break;
    case 0x4:  // ,R
      ea = reg;
      break;
    case 0x5:  // B,R
      ea = uint16_t(reg + int8_t(r_.b));
      cycles += 1;
      break;
    case 0x6:  // A,R
      ea = uint16_t(reg + int8_t(r_.a));
      cycles += 1;
      break;
    case 0x8: {  // n8,R
      const int8_t off = int8_t(fetch8());
      ea = uint16_t(reg + off);
      cycles += 1;
      break;
    }
    case 0x9: {  // n16,R
      const uint16_t off = fetch16();
      ea = uint16_t(reg + off);
      cycles += 4;
      break;
    }
    case 0xB:  // D,R
      ea = uint16_t(reg + r_.d());
      cycles += 4;
      break;
    case 0xC: {  // n8,PCR (PC after the offset byte)
      const int8_t off = int8_t(fetch8());
      ea = uint16_t(r_.pc + off);
      cycles += 1;
      break;
    }
    case 0xD: {  // n16,PCR
      const uint16_t off = fetch16();
      ea = uint16_t(r_.pc + off);
      cycles += 5;
      break;
    }
    case 0xF:  // [n16] (only defined with the indirect bit)
      ea = fetch16();
      cycles += 2;
      if (!indirect) noteIllegal(opPc);
      break;
    default:  // 0x7, 0xA, 0xE: undefined
      noteIllegal(opPc);
      ea = reg;
      cycles += 1;
      break;
  }
  if (indirect) {
    ea = read16(ea);
    cycles += 3;
  }
  return ea;
}

uint8_t Cpu6809::operand8() { return mode_ == AddrMode::Immediate8 ? fetch8() : read8(ea_); }

uint16_t Cpu6809::operand16() {
  return mode_ == AddrMode::Immediate16 ? fetch16() : read16(ea_);
}

// ---- Save / load ----------------------------------------------------------------

void Cpu6809::save(StateWriter& w) const {
  w.tag(fourcc("6809"));
  w.u8(r_.a);
  w.u8(r_.b);
  w.u8(r_.dp);
  w.u8(r_.cc);
  w.u16(r_.x);
  w.u16(r_.y);
  w.u16(r_.u);
  w.u16(r_.s);
  w.u16(r_.pc);
  w.u8(uint8_t(wait_));
  w.u8(nmiLine_);
  w.u8(nmiPending_);
  w.u8(nmiArmed_);
  w.u8(irqLine_);
  w.u8(firqLine_);
  w.u64(illegalCount_);
  w.u16(lastIllegalPc_);
}

void Cpu6809::load(StateReader& rd) {
  rd.tag(fourcc("6809"));
  Regs r;
  r.a = rd.u8();
  r.b = rd.u8();
  r.dp = rd.u8();
  r.cc = rd.u8();
  r.x = rd.u16();
  r.y = rd.u16();
  r.u = rd.u16();
  r.s = rd.u16();
  r.pc = rd.u16();
  const uint8_t wait = rd.u8();
  if (wait > uint8_t(Wait::Sync)) throw StateError("cpu6809: bad wait state");
  const bool nmiLine = rd.u8() != 0, nmiPending = rd.u8() != 0, nmiArmed = rd.u8() != 0;
  const bool irqLine = rd.u8() != 0, firqLine = rd.u8() != 0;
  const uint64_t illegalCount = rd.u64();
  const uint16_t lastIllegalPc = rd.u16();
  // Commit only after everything was read, so a failed load changes nothing.
  r_ = r;
  wait_ = Wait(wait);
  nmiLine_ = nmiLine;
  nmiPending_ = nmiPending;
  nmiArmed_ = nmiArmed;
  irqLine_ = irqLine;
  firqLine_ = firqLine;
  illegalCount_ = illegalCount;
  lastIllegalPc_ = lastIllegalPc;
}

}  // namespace mirage
