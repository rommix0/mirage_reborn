#pragma once
#include <cstdint>
#include <functional>

#include "mirage/state_io.h"

namespace mirage {

// Motorola MC6850 ACIA as wired in the Mirage (MIDI). Register select is RS:
// reg 0 = status (read) / control (write), reg 1 = receive / transmit data.
//
// Modelled: RDRF, TDRE, OVRN, the interrupt-enable bits (CR7 = RX IRQ,
// CR6:5 = 01 TX IRQ) and master reset (CR1:0 = 11). Not modelled: the bit
// clock (Machine paces received bytes at the MIDI byte time instead), framing
// and parity errors (FE/PE read 0). The MIDI interface ties CTS and DCD
// active, so status bits 2 and 3 read 0.
//
// Transmit is double-buffered and paced. A data write goes to the holding
// register (TDRE=0). Whenever the shift register is idle the held byte moves
// into it at once (TDRE=1 again) and onTransmit(byte, cycle) fires: the byte is
// then on the wire for kByteCycles. A write while TDRE=0 replaces the held byte,
// which is lost, as on hardware. The TX interrupt (CR6:5 = 01) follows TDRE.
// tick() advances the shifter; `now` is the host's E-cycle count at the end of
// the ticked span and stamps each onTransmit (a write is stamped with the `now`
// of the last tick). Master reset empties both registers.
class Acia6850 {
 public:
  // One MIDI byte (10 bits at 31,250 baud) is 320 us = 640 E cycles.
  static constexpr uint32_t kByteCycles = 640;

  void tick(uint32_t eCycles, uint64_t now);
  uint8_t read(uint8_t reg);           // reg 0 = status, 1 = data (low bit used)
  void write(uint8_t reg, uint8_t v);  // reg 0 = control, 1 = data
  // A byte arrives from the line. If the previous byte has not been read it is
  // kept, the new one is lost and OVRN is set (as hardware).
  void receiveByte(uint8_t b);
  bool irq() const;
  void reset();  // master reset; the callback is kept

  std::function<void(uint8_t byte, uint64_t cycle)> onTransmit;

  void save(StateWriter&) const;
  void load(StateReader&);  // commits only on success

 private:
  uint8_t status() const;
  void startShift(uint8_t byte, uint64_t cycle);

  uint8_t control_ = 0;
  uint8_t rxData_ = 0;
  bool rdrf_ = false, ovrn_ = false;
  uint8_t holdData_ = 0;
  bool holdFull_ = false;         // TDRE = !holdFull_
  uint32_t shiftLeft_ = 0;        // E cycles until the shift register is idle
  uint64_t now_ = 0;              // the host cycle at the last tick; not saved
};

}  // namespace mirage
