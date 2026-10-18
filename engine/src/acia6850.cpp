#include "mirage/acia6850.h"

namespace mirage {

namespace {
constexpr uint8_t kRdrf = 0x01, kTdre = 0x02, kOvrn = 0x20, kIrq = 0x80;
constexpr uint8_t kCrDivide = 0x03, kCrMasterReset = 0x03;
constexpr uint8_t kCrTxIrqMask = 0x60, kCrTxIrqOn = 0x20, kCrRxIrq = 0x80;
}  // namespace

void Acia6850::reset() {
  control_ = 0;
  rxData_ = 0;
  rdrf_ = false;
  ovrn_ = false;
}

// TDRE is always set (transmit is instantaneous); DCD and CTS read 0.
bool Acia6850::irq() const {
  const bool rx = (control_ & kCrRxIrq) && (rdrf_ || ovrn_);
  const bool tx = (control_ & kCrTxIrqMask) == kCrTxIrqOn;
  return rx || tx;
}

uint8_t Acia6850::status() const {
  uint8_t s = kTdre;
  if (rdrf_) s |= kRdrf;
  if (ovrn_) s |= kOvrn;
  if (irq()) s |= kIrq;
  return s;
}

uint8_t Acia6850::read(uint8_t reg) {
  if ((reg & 1) == 0) return status();
  rdrf_ = false;
  ovrn_ = false;
  return rxData_;
}

void Acia6850::write(uint8_t reg, uint8_t v) {
  if ((reg & 1) == 0) {
    control_ = v;
    if ((v & kCrDivide) == kCrMasterReset) {
      rdrf_ = false;
      ovrn_ = false;
    }
    return;
  }
  if (onTransmit) onTransmit(v);
}

void Acia6850::receiveByte(uint8_t b) {
  if (rdrf_) {
    ovrn_ = true;
    return;
  }
  rxData_ = b;
  rdrf_ = true;
}

void Acia6850::save(StateWriter& w) const {
  w.tag(fourcc("ACIA"));
  w.u8(control_);
  w.u8(rxData_);
  w.u8(rdrf_);
  w.u8(ovrn_);
}

void Acia6850::load(StateReader& r) {
  r.tag(fourcc("ACIA"));
  const uint8_t control = r.u8(), data = r.u8(), rdrf = r.u8(), ovrn = r.u8();
  if (rdrf > 1 || ovrn > 1) throw StateError("acia6850: bad state");
  control_ = control;
  rxData_ = data;
  rdrf_ = rdrf;
  ovrn_ = ovrn;
}

}  // namespace mirage
