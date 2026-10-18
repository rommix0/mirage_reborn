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
// Transmit is instantaneous: a data write calls onTransmit and TDRE stays set.
class Acia6850 {
 public:
  uint8_t read(uint8_t reg);           // reg 0 = status, 1 = data (low bit used)
  void write(uint8_t reg, uint8_t v);  // reg 0 = control, 1 = data
  // A byte arrives from the line. If the previous byte has not been read it is
  // kept, the new one is lost and OVRN is set (as hardware).
  void receiveByte(uint8_t b);
  bool irq() const;
  void reset();  // master reset; the callback is kept

  std::function<void(uint8_t)> onTransmit;

  void save(StateWriter&) const;
  void load(StateReader&);  // commits only on success

 private:
  uint8_t status() const;

  uint8_t control_ = 0;
  uint8_t rxData_ = 0;
  bool rdrf_ = false, ovrn_ = false;
};

}  // namespace mirage
