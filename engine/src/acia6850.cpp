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
  holdData_ = 0;
  holdFull_ = false;
  shiftLeft_ = 0;
  now_ = 0;
}

// DCD and CTS read 0.
bool Acia6850::irq() const {
  const bool rx = (control_ & kCrRxIrq) && (rdrf_ || ovrn_);
  const bool tx = (control_ & kCrTxIrqMask) == kCrTxIrqOn && !holdFull_;
  return rx || tx;
}

uint8_t Acia6850::status() const {
  uint8_t s = holdFull_ ? 0 : kTdre;
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
      holdFull_ = false;
      shiftLeft_ = 0;
    }
    return;
  }
  if (shiftLeft_ == 0)
    startShift(v, now_);
  else {
    holdData_ = v;  // replaces a byte already held
    holdFull_ = true;
  }
}

void Acia6850::startShift(uint8_t byte, uint64_t cycle) {
  shiftLeft_ = kByteCycles;
  holdFull_ = false;
  if (onTransmit) onTransmit(byte, cycle);
}

void Acia6850::tick(uint32_t eCycles, uint64_t now) {
  now_ = now;
  uint64_t t = now - eCycles;  // the cycle at which the span began
  uint32_t left = eCycles;
  while (shiftLeft_ != 0) {
    if (left < shiftLeft_) {
      shiftLeft_ -= left;
      return;
    }
    left -= shiftLeft_;
    t += shiftLeft_;
    shiftLeft_ = 0;
    if (holdFull_) startShift(holdData_, t);
  }
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
  w.u8(holdData_);
  w.u8(holdFull_);
  w.u32(shiftLeft_);
}

void Acia6850::load(StateReader& r) {
  r.tag(fourcc("ACIA"));
  const uint8_t control = r.u8(), data = r.u8(), rdrf = r.u8(), ovrn = r.u8();
  const uint8_t hold = r.u8(), holdFull = r.u8();
  const uint32_t shiftLeft = r.u32();
  if (rdrf > 1 || ovrn > 1 || holdFull > 1 || shiftLeft > kByteCycles ||
      (holdFull && shiftLeft == 0))
    throw StateError("acia6850: bad state");
  control_ = control;
  rxData_ = data;
  rdrf_ = rdrf;
  ovrn_ = ovrn;
  holdData_ = hold;
  holdFull_ = holdFull;
  shiftLeft_ = shiftLeft;
}

}  // namespace mirage
