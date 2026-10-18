#include "mirage/doc5503.h"

#include <algorithm>
#include <utility>

#include "mirage/clocks.h"

namespace mirage {

namespace {
constexpr uint32_t kClocksPerSlot = 8;
constexpr int kRefreshSlots = 2;
constexpr uint32_t kAccMask = 0xFFFFFF;  // 24-bit accumulator
// Control register bits.
constexpr uint8_t kH = 0x01, kM0 = 0x02, kIe = 0x08;
constexpr int kModeOneShot = 1, kModeSyncAm = 2, kModeSwap = 3;
// Size register bits.
constexpr uint8_t kBs = 0x40;

int modeOf(uint8_t control) { return (control >> 1) & 3; }
}  // namespace

Doc5503::Doc5503(std::function<uint8_t(uint32_t)> waveRead) : waveRead_(std::move(waveRead)) {
  reset();
}

void Doc5503::reset() {
  osc_.fill(Osc{});
  for (Osc& o : osc_) o.control = kH;  // all oscillators halted
  oer_ = 0;  // one oscillator, the datasheet's reset default
  fifo_.fill(0);
  fifoCount_ = 0;
  lastIrqOsc_ = 0x1F;
  clockRem_ = 0;
  slot_ = 0;
  frame_.fill(0);
}

// OER = 2 x count (datasheet: "to enable all 32 oscillators load the register
// with 64"), at least one.
int Doc5503::enabledOscillators() const { return std::clamp(oer_ >> 1, 1, kOscillators); }

double Doc5503::sampleRate() const {
  return double(kMasterClockHz) / (kClocksPerSlot * double(enabledOscillators() + kRefreshSlots));
}

uint8_t Doc5503::read(uint8_t reg) {
  if (reg < 0xE0) {
    const Osc& o = osc_[reg & 0x1F];
    switch (reg >> 5) {
      case 0: return uint8_t(o.fc);
      case 1: return uint8_t(o.fc >> 8);
      case 2: return o.volume;
      case 3: return o.data;
      case 4: return o.pointer;
      case 5: return o.control;
      default: return uint8_t(o.sizeReg | 0x80);  // bit 7 unused, reads 1
    }
  }
  switch (reg) {
    case 0xE0: {  // OIR: bit 7 low while an interrupt is pending; bits 6 and 0 read 1
      if (fifoCount_ == 0) return uint8_t(0xC1 | (lastIrqOsc_ << 1));
      lastIrqOsc_ = fifo_[0];
      std::move(fifo_.begin() + 1, fifo_.begin() + fifoCount_, fifo_.begin());
      --fifoCount_;
      return uint8_t(0x41 | (lastIrqOsc_ << 1));
    }
    case 0xE1: return uint8_t(oer_ | 0xC1);  // bits 7, 6 and 0 unused, read 1
    case 0xE2: return adcRead ? adcRead() : 0xFF;
    default: return 0xFF;
  }
}

void Doc5503::write(uint8_t reg, uint8_t v) {
  if (reg < 0xE0) {
    const int n = reg & 0x1F;
    Osc& o = osc_[n];
    switch (reg >> 5) {
      case 0: o.fc = uint16_t((o.fc & 0xFF00) | v); break;
      case 1: o.fc = uint16_t((o.fc & 0x00FF) | (v << 8)); break;
      case 2: o.volume = v; break;
      case 3: break;  // wave data: read only
      case 4: o.pointer = v; break;
      case 5: {
        const bool ieRaised = (v & kIe) && !(o.control & kIe);
        o.control = v;
        if ((v & kH) && (v & kM0)) o.acc = 0;  // datasheet: M0 = 1 and H = 1 reset A
        if (ieRaised && o.latent) {
          o.latent = false;
          queue(n);
        }
        break;
      }
      default: o.sizeReg = v; break;
    }
    return;
  }
  if (reg == 0xE1) oer_ = v;
  // $E0 (OIR) and $E2 (A/D) are read only.
}

void Doc5503::tickMasterClocks(uint32_t n) {
  clockRem_ += n;
  while (clockRem_ >= kClocksPerSlot) {
    clockRem_ -= kClocksPerSlot;
    slot();
  }
}

void Doc5503::slot() {
  const int enabled = enabledOscillators();
  if (slot_ < enabled) updateOscillator(slot_);  // later slots refresh DRAM
  if (++slot_ >= enabled + kRefreshSlots) {
    if (onFrame) onFrame(frame_);
    frame_.fill(0);
    slot_ = 0;
  }
}

void Doc5503::updateOscillator(int n) {
  Osc& o = osc_[n];
  if (o.control & kH) return;
  const int size = (o.sizeReg >> 3) & 7, res = o.sizeReg & 7;
  const int mode = modeOf(o.control);
  const uint32_t period = 1u << (17 + res);  // one pass of the table

  // A = A + FC first; the carry out of the top address bit ends the table.
  const uint32_t sum = o.acc + o.fc;
  o.acc = sum & (period - 1);
  if (sum >= period) {
    if (mode == kModeOneShot || mode == kModeSwap) {
      halt(n);
      interrupt(n);
      return;
    }
    // Free run and sync/AM wrap. An even oscillator in sync mode resets its
    // odd partner.
    if (mode == kModeSyncAm && (n & 1) == 0) osc_[n + 1].acc = 0;
    interrupt(n);
  }

  const uint32_t mask = (256u << size) - 1;
  const uint32_t addr = ((uint32_t(o.pointer) << 8) & ~mask) | ((o.acc >> (9 + res - size)) & mask);
  o.data = waveRead_(((o.sizeReg & kBs) ? 0x10000u : 0u) | addr);
  if (o.data == 0) {  // a zero byte halts in every mode
    halt(n);
    interrupt(n);
    return;
  }

  int volume = o.volume;
  if (mode == kModeSyncAm && (n & 1)) volume = osc_[n - 1].data;  // AM
  frame_[(o.control >> 4) & (kChannels - 1)] += (int32_t(o.data) - 128) * volume;
}

// The DOC halts oscillator n: H = 1 (A = 0 when M0 = 1); in swap mode the
// partner is started from the top of its table.
void Doc5503::halt(int n) {
  Osc& o = osc_[n];
  o.control |= kH;
  if (o.control & kM0) o.acc = 0;
  if (modeOf(o.control) == kModeSwap) {
    Osc& p = osc_[n ^ 1];
    p.control &= uint8_t(~kH);
    p.acc = 0;
  }
}

// Oscillator n completed its table (or hit a zero byte).
void Doc5503::interrupt(int n) {
  if (osc_[n].control & kIe) queue(n);
  else osc_[n].latent = true;
}

void Doc5503::queue(int n) {
  for (int i = 0; i < fifoCount_; ++i)
    if (fifo_[size_t(i)] == n) return;  // already pending
  fifo_[size_t(fifoCount_++)] = uint8_t(n);
}

void Doc5503::save(StateWriter& w) const {
  w.tag(fourcc("DOC5"));
  w.u8(oer_);
  w.u8(uint8_t(clockRem_));
  w.u8(uint8_t(slot_));
  w.u8(lastIrqOsc_);
  for (const Osc& o : osc_) {
    w.u16(o.fc);
    w.u8(o.volume);
    w.u8(o.data);
    w.u8(o.pointer);
    w.u8(o.control);
    w.u8(o.sizeReg);
    w.u32(o.acc);
    w.u8(o.latent);
  }
  for (int32_t s : frame_) w.i32(s);
  w.u8(uint8_t(fifoCount_));
  for (int i = 0; i < fifoCount_; ++i) w.u8(fifo_[size_t(i)]);
}

void Doc5503::load(StateReader& r) {
  r.tag(fourcc("DOC5"));
  const uint8_t oer = r.u8(), clockRem = r.u8(), slot = r.u8(), lastIrq = r.u8();
  if (clockRem >= kClocksPerSlot || slot >= kOscillators + kRefreshSlots || lastIrq >= kOscillators)
    throw StateError("doc5503: bad timing state");
  std::array<Osc, kOscillators> osc{};
  for (Osc& o : osc) {
    o.fc = r.u16();
    o.volume = r.u8();
    o.data = r.u8();
    o.pointer = r.u8();
    o.control = r.u8();
    o.sizeReg = r.u8();
    o.acc = r.u32();
    const uint8_t latent = r.u8();
    if (o.acc > kAccMask || latent > 1) throw StateError("doc5503: bad oscillator");
    o.latent = latent != 0;
  }
  std::array<int32_t, kChannels> frame{};
  for (int32_t& s : frame) s = r.i32();
  const uint8_t count = r.u8();
  if (count > kOscillators) throw StateError("doc5503: bad interrupt FIFO");
  std::array<uint8_t, kOscillators> fifo{};
  uint32_t seen = 0;
  for (int i = 0; i < count; ++i) {
    const uint8_t n = r.u8();
    if (n >= kOscillators || (seen >> n) & 1) throw StateError("doc5503: bad interrupt FIFO");
    seen |= 1u << n;
    fifo[size_t(i)] = n;
  }
  oer_ = oer;
  clockRem_ = clockRem;
  slot_ = slot;
  lastIrqOsc_ = lastIrq;
  osc_ = osc;
  frame_ = frame;
  fifo_ = fifo;
  fifoCount_ = count;
}

}  // namespace mirage
