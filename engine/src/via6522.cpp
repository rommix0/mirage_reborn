#include "mirage/via6522.h"

namespace mirage {

namespace {
constexpr uint8_t kIfrT1 = 0x40, kIfrT2 = 0x20, kIfrSr = 0x04;
constexpr uint8_t kIfrCb = 0x18, kIfrCa = 0x03;  // CB1/CB2 and CA1/CA2 flags
constexpr uint8_t kAcrT1Free = 0x40, kAcrPb7 = 0x80, kAcrT2Count = 0x20;
// Timers time out N+2 E cycles after the load (datasheet N+1.5, rounded).
constexpr uint32_t kTimerOverhead = 2;
}  // namespace

void Via6522::reset() {
  orb_ = ora_ = ddrb_ = ddra_ = 0;
  t1Latch_ = t2Latch_ = 0;
  t1Rem_ = t2Rem_ = 0;
  t1Armed_ = t2Armed_ = false;
  sr_ = acr_ = pcr_ = ifr_ = ier_ = 0;
  pb7_ = true;
}

uint8_t Via6522::outB() const {
  uint8_t v = uint8_t(orb_ | ~ddrb_);
  if (acr_ & kAcrPb7) v = uint8_t((v & 0x7F) | (pb7_ ? 0x80 : 0));
  return v;
}

void Via6522::emitA() const {
  if (portAOut) portAOut(outA());
}

void Via6522::emitB() const {
  if (portBOut) portBOut(outB());
}

uint8_t Via6522::read(uint8_t reg) {
  switch (reg & 15) {
    case 0: {  // ORB
      ifr_ &= uint8_t(~kIfrCb);
      const uint8_t pins = portBPins ? portBPins() : uint8_t(0xFF);
      uint8_t v = uint8_t((pins & ~ddrb_) | (orb_ & ddrb_));
      if (acr_ & kAcrPb7) v = uint8_t((v & 0x7F) | (pb7_ ? 0x80 : 0));
      return v;
    }
    case 1:  // ORA
      ifr_ &= uint8_t(~kIfrCa);
      [[fallthrough]];
    case 15: {  // ORA, no handshake
      const uint8_t pins = portAPins ? portAPins() : uint8_t(0xFF);
      return uint8_t((pins & ~ddra_) | (ora_ & ddra_));
    }
    case 2: return ddrb_;
    case 3: return ddra_;
    case 4:  // T1C-L
      ifr_ &= uint8_t(~kIfrT1);
      return uint8_t((t1Rem_ - kTimerOverhead) & 0xFF);
    case 5: return uint8_t(((t1Rem_ - kTimerOverhead) >> 8) & 0xFF);
    case 6: return uint8_t(t1Latch_ & 0xFF);
    case 7: return uint8_t(t1Latch_ >> 8);
    case 8:  // T2C-L
      ifr_ &= uint8_t(~kIfrT2);
      return uint8_t((t2Rem_ - kTimerOverhead) & 0xFF);
    case 9: return uint8_t(((t2Rem_ - kTimerOverhead) >> 8) & 0xFF);
    case 10:
      ifr_ &= uint8_t(~kIfrSr);
      return sr_;
    case 11: return acr_;
    case 12: return pcr_;
    case 13: return uint8_t((ifr_ & 0x7F) | (irq() ? 0x80 : 0));
    default: return uint8_t(ier_ | 0x80);  // 14
  }
}

void Via6522::write(uint8_t reg, uint8_t v) {
  switch (reg & 15) {
    case 0:
      orb_ = v;
      ifr_ &= uint8_t(~kIfrCb);
      emitB();
      break;
    case 1:
      ifr_ &= uint8_t(~kIfrCa);
      [[fallthrough]];
    case 15:
      ora_ = v;
      emitA();
      break;
    case 2:
      ddrb_ = v;
      emitB();
      break;
    case 3:
      ddra_ = v;
      emitA();
      break;
    case 4:
    case 6:  // low latch byte
      t1Latch_ = uint16_t((t1Latch_ & 0xFF00) | v);
      break;
    case 5:  // T1C-H: latch high, load the counter and start
      t1Latch_ = uint16_t((t1Latch_ & 0x00FF) | (v << 8));
      t1Rem_ = uint32_t(t1Latch_) + kTimerOverhead;
      t1Armed_ = true;
      ifr_ &= uint8_t(~kIfrT1);
      if (acr_ & kAcrPb7) {
        pb7_ = false;
        emitB();
      }
      break;
    case 7:
      t1Latch_ = uint16_t((t1Latch_ & 0x00FF) | (v << 8));
      ifr_ &= uint8_t(~kIfrT1);
      break;
    case 8:
      t2Latch_ = v;
      break;
    case 9:  // T2C-H: load the counter from {v, latch low} and start
      t2Rem_ = uint32_t((v << 8) | t2Latch_) + kTimerOverhead;
      t2Armed_ = true;
      ifr_ &= uint8_t(~kIfrT2);
      break;
    case 10:
      sr_ = v;
      ifr_ &= uint8_t(~kIfrSr);
      break;
    case 11:
      acr_ = v;
      emitB();
      break;
    case 12: pcr_ = v; break;
    case 13: ifr_ &= uint8_t(~(v & 0x7F)); break;
    default:  // 14
      if (v & 0x80) ier_ |= uint8_t(v & 0x7F);
      else ier_ &= uint8_t(~v);
      break;
  }
}

void Via6522::tickT1(uint32_t n) {
  if (!t1Armed_ || n < t1Rem_) {
    t1Rem_ -= n;  // wraps modulo 2^32 once past a one-shot time-out
    return;
  }
  const uint32_t after = n - t1Rem_;  // cycles elapsed since the first time-out
  ifr_ |= kIfrT1;
  if (!(acr_ & kAcrT1Free)) {
    t1Armed_ = false;
    t1Rem_ = 0u - after;
    if (acr_ & kAcrPb7) pb7_ = true;
    return;
  }
  const uint32_t period = uint32_t(t1Latch_) + kTimerOverhead;
  const uint32_t timeouts = 1 + after / period;
  t1Rem_ = period - after % period;
  if ((acr_ & kAcrPb7) && (timeouts & 1)) pb7_ = !pb7_;
}

void Via6522::tickT2(uint32_t n) {
  if (acr_ & kAcrT2Count) return;  // pulse counting on PB6: not emulated
  if (!t2Armed_ || n < t2Rem_) {
    t2Rem_ -= n;
    return;
  }
  t2Armed_ = false;
  ifr_ |= kIfrT2;
  t2Rem_ = 0u - (n - t2Rem_);
}

void Via6522::tick(uint32_t eCycles) {
  const bool pb7Before = pb7_;
  tickT1(eCycles);
  tickT2(eCycles);
  if (pb7_ != pb7Before) emitB();
}

void Via6522::save(StateWriter& w) const {
  w.tag(fourcc("VIA6"));
  w.u8(orb_);
  w.u8(ora_);
  w.u8(ddrb_);
  w.u8(ddra_);
  w.u16(t1Latch_);
  w.u16(t2Latch_);
  w.u32(t1Rem_);
  w.u32(t2Rem_);
  w.u8(t1Armed_);
  w.u8(t2Armed_);
  w.u8(sr_);
  w.u8(acr_);
  w.u8(pcr_);
  w.u8(ifr_);
  w.u8(ier_);
  w.u8(pb7_);
}

void Via6522::load(StateReader& r) {
  r.tag(fourcc("VIA6"));
  const uint8_t orb = r.u8(), ora = r.u8(), ddrb = r.u8(), ddra = r.u8();
  const uint16_t t1l = r.u16(), t2l = r.u16();
  const uint32_t t1r = r.u32(), t2r = r.u32();
  const uint8_t t1a = r.u8(), t2a = r.u8();
  const uint8_t sr = r.u8(), acr = r.u8(), pcr = r.u8(), ifr = r.u8(), ier = r.u8();
  const uint8_t pb7 = r.u8();
  if (t1a > 1 || t2a > 1 || pb7 > 1 || ((ifr | ier) & 0x80))
    throw StateError("via6522: bad state");
  orb_ = orb; ora_ = ora; ddrb_ = ddrb; ddra_ = ddra;
  t1Latch_ = t1l; t2Latch_ = t2l;
  t1Rem_ = t1r; t2Rem_ = t2r;
  t1Armed_ = t1a; t2Armed_ = t2a;
  sr_ = sr; acr_ = acr; pcr_ = pcr; ifr_ = ifr; ier_ = ier;
  pb7_ = pb7;
}

}  // namespace mirage
