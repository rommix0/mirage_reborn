#pragma once
#include <cstdint>

#include "mirage/bus.h"
#include "mirage/state_io.h"

namespace mirage {

enum class AddrMode : uint8_t;

// Condition code register bits.
namespace cc {
constexpr uint8_t E = 0x80, F = 0x40, H = 0x20, I = 0x10, N = 0x08, Z = 0x04, V = 0x02,
                  C = 0x01;
}

struct Regs {
  uint8_t a = 0, b = 0, dp = 0, cc = 0;
  uint16_t x = 0, y = 0, u = 0, s = 0, pc = 0;
  uint16_t d() const { return uint16_t(a << 8 | b); }
  void setD(uint16_t v) {
    a = uint8_t(v >> 8);
    b = uint8_t(v);
  }
};

// Motorola MC6809E. step() runs one instruction (or one interrupt entry) and
// returns the E-clock cycles it took. Illegal opcodes are counted and run as
// 2-cycle no-ops; nothing here throws except load() on a malformed state.
class Cpu6809 {
 public:
  explicit Cpu6809(Bus& bus) : bus_(bus) {}

  void reset();  // CC |= I|F, DP=0, PC = word at $FFFE, NMI disarmed until S loaded
  // Full power-on: every register, line latch and counter back to its
  // initial value, then reset(). reset() alone is the 6809 /RESET behaviour.
  void powerOn();
  int step();
  void setNmi(bool line);  // low->high edge is latched (once NMI is armed)
  void setIrq(bool line);  // level
  void setFirq(bool line);  // level

  Regs& regs() { return r_; }
  const Regs& regs() const { return r_; }
  uint64_t illegalCount() const { return illegalCount_; }
  uint16_t lastIllegalPc() const { return lastIllegalPc_; }

  void save(StateWriter&) const;
  void load(StateReader&);

 private:
  enum class Wait : uint8_t { None, Cwai, Sync };
  // CWAI (>= 20 cycles) and SYNC (>= 4) charge these when the wait ends; the
  // instruction itself charges the rest of the datasheet minimum.
  static constexpr int kCwaiWakeCycles = 4, kSyncWakeCycles = 2;

  // Bus helpers (cpu6809.cpp).
  uint8_t read8(uint16_t addr) { return bus_.read(addr); }
  uint16_t read16(uint16_t addr);
  void write16(uint16_t addr, uint16_t v);
  uint8_t fetch8();
  uint16_t fetch16();
  void push8(uint16_t& sp, uint8_t v);
  void push16(uint16_t& sp, uint16_t v);
  uint8_t pull8(uint16_t& sp);
  uint16_t pull16(uint16_t& sp);
  void pushEntireState();  // PC, U, Y, X, DP, B, A, CC onto S
  void setS(uint16_t v);   // any load of S arms NMI
  void noteIllegal(uint16_t pc);
  int serviceInterrupt();  // 0 if none taken
  int execute();
  uint16_t indexedEa(int& cycles, uint16_t opPc);
  uint8_t operand8();
  uint16_t operand16();

  // Instruction groups (cpu6809_ops.cpp). Each returns cycles beyond the
  // opcode table's base count.
  int execPage0(uint8_t op);
  int execPage2(uint8_t op);
  int execPage3(uint8_t op);
  void execRmw(uint8_t op);
  int execAlu(uint8_t op);
  bool branchTaken(int cond) const;
  uint16_t regByCode(int code) const;
  void setRegByCode(int code, uint16_t v);
  int pushList(bool systemStack, uint8_t mask);
  int pullList(bool systemStack, uint8_t mask);
  int swi(uint16_t vector, bool maskIrqs);

  Bus& bus_;
  Regs r_;
  Wait wait_ = Wait::None;
  bool nmiLine_ = false, nmiPending_ = false, nmiArmed_ = false;
  bool irqLine_ = false, firqLine_ = false;
  uint64_t illegalCount_ = 0;
  uint16_t lastIllegalPc_ = 0;

  // Decoded operand of the instruction being executed (not machine state).
  AddrMode mode_{};
  uint16_t ea_ = 0;
};

}  // namespace mirage
