#pragma once
#include <cstdint>
#include <functional>

#include "mirage/state_io.h"

namespace mirage {

// MOS 6522 VIA, clocked at the CPU E clock (Phi2).
//
// Timers use the datasheet timing rounded to whole E cycles: a T1/T2 one-shot
// sets its flag N+2 cycles after the counter load, and a T1 free-run period is
// N+2 cycles (so latch 0 gives a PB7 square wave at Phi2/4).
//
// The shift register, CA1/CA2/CB1/CB2 and pulse counting are not emulated (on
// the Mirage they go to the keyboard controller, which the rack unit lacks).
// SR, ACR, PCR, IFR and IER are stored faithfully so software reads back what
// it wrote.
class Via6522 {
 public:
  uint8_t read(uint8_t reg);  // reg 0..15 (only the low 4 bits are used)
  void write(uint8_t reg, uint8_t v);
  void tick(uint32_t eCycles);
  bool irq() const { return (ifr_ & ier_ & 0x7F) != 0; }
  bool pb7() const { return pb7_; }  // T1 PB7 output level
  void reset();                      // chip reset; callbacks are kept

  // Pin levels as driven by the outside world. Only the bits whose DDR bit is
  // 0 are used. An unset callback reads as $FF (pulled up).
  std::function<uint8_t()> portAPins, portBPins;
  // Called with the port's output pins, (ORx & DDRx) | ~DDRx, after every
  // write to ORx/DDRx (and ACR for port B) and whenever PB7 changes. It may
  // fire when nothing changed; receivers must be idempotent.
  std::function<void(uint8_t)> portAOut, portBOut;

  uint8_t outA() const { return uint8_t(ora_ | ~ddra_); }
  // Includes the T1 output on PB7 while ACR bit 7 is set.
  uint8_t outB() const;

  void save(StateWriter&) const;
  void load(StateReader&);  // commits only on success

 private:
  void tickT1(uint32_t n);
  void tickT2(uint32_t n);
  void emitA() const;
  void emitB() const;

  uint8_t orb_ = 0, ora_ = 0, ddrb_ = 0, ddra_ = 0;
  uint16_t t1Latch_ = 0, t2Latch_ = 0;
  // Cycles until time-out; the visible counter is (rem - 2) & $FFFF. After a
  // one-shot time-out the value wraps modulo 2^32 so the counter keeps running.
  uint32_t t1Rem_ = 0, t2Rem_ = 0;
  bool t1Armed_ = false, t2Armed_ = false;
  uint8_t sr_ = 0, acr_ = 0, pcr_ = 0, ifr_ = 0, ier_ = 0;
  bool pb7_ = true;
};

}  // namespace mirage
