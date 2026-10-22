#include "mirage/voice_filter.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace mirage {

namespace {

// --- The CV mappings (filters.md section 5) ----------------------------------
// The ROM's calibration $F571 accepts a self-oscillation whose four periods
// take 129-132 passes of a 50 E-cycle loop (plus 68 cycles of call overhead):
// 4 / ((130.5 * 50 + 68) / 2 MHz) = 1213 Hz. The OS stores B - $7A, so $7A is
// the CV it expects a nominal filter to need for that frequency.
constexpr double kCalibrationHz = 1213.0;
constexpr int kCalibrationCv = 0x7A;
constexpr double kCvStepsPerOctave = 24.0;
// CEM3328: oscillation needs about 3.2 V of the 0-5 V resonance range.
// resonanceFromCv is a cubic reverse-audio taper that crosses VoiceFilter's
// oscillation threshold (4 / kMaxFeedback) at CV 162.6 = 3.19 V.
constexpr double kMaxFeedback = 4.2;  // loop gain at resonance 1
// Input gain 1 + kComp * k: the CEM3328's passband loss at full resonance is
// about 12 dB (data sheet curve); a plain ladder would lose 1 / (1 + k).
constexpr double kComp = 0.08;
// The compressed input: a soft limiter in front of the 8-bit A/D.
constexpr float kAdcGain = 1.0f;
// Analog noise at each filter input (a few hundred uV in a 5 V swing): what
// starts a self-oscillation when the DOC is silent.
constexpr float kNoiseLevel = 1.0e-4f;

// tanh approximation (Pade 3/2): exact +-1 at |x| = 3 with zero slope there,
// monotonic, within 2.5% of tanh.
inline float softClip(float x) {
  if (x >= 3.0f) return 1.0f;
  if (x <= -3.0f) return -1.0f;
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

void putF32(StateWriter& w, float v) { w.u32(std::bit_cast<uint32_t>(v)); }
float getF32(StateReader& r) { return std::bit_cast<float>(r.u32()); }

}  // namespace

double cutoffHzFromCv(uint8_t cv) {
  return kCalibrationHz * std::exp2((int(cv) - kCalibrationCv) / kCvStepsPerOctave);
}

double resonanceFromCv(uint8_t cv) {
  const double x = 1.0 - cv / 255.0;
  return 1.0 - x * x * x;
}

uint8_t compressedAdcCode(float level) {
  const float v = 128.0f + 127.0f * softClip(level * kAdcGain);
  return uint8_t(std::clamp(std::lround(v), 1L, 255L));
}

// --- VoiceFilter ----------------------------------------------------------------

VoiceFilter::VoiceFilter() { update(); }

void VoiceFilter::setSampleRate(double hz) {
  rate_ = std::max(hz, 1000.0);
  update();
}

void VoiceFilter::setCutoffHz(double hz) {
  cutoff_ = hz;
  update();
}

void VoiceFilter::setResonance(double r01) {
  res_ = std::clamp(r01, 0.0, 1.0);
  update();
}

void VoiceFilter::reset() { s_.fill(0.0f); }

void VoiceFilter::update() {
  const double fc = std::clamp(cutoff_, 1.0, 0.45 * rate_);
  const double g = std::tan(std::numbers::pi * fc / rate_);  // prewarped
  const double G = g / (1.0 + g);
  const double k = kMaxFeedback * res_;
  g_ = float(G);
  b_ = float(1.0 - G);
  gb1_ = float(G * (1.0 - G));
  gb2_ = float(G * G * (1.0 - G));
  gb3_ = float(G * G * G * (1.0 - G));
  g4_ = float(G * G * G * G);
  k_ = float(k);
  comp_ = float(1.0 + kComp * k);
}

// TPT (zero-delay feedback) ladder of four one-poles y = G x + (1 - G) s. The
// feedback k * clip(y4) is linearised per sample as k * T * y4 with
// T = clip(e) / e at the linear estimate e, which keeps the loop delay-free.
float VoiceFilter::process(float in) {
  float* s = s_.data();  // raw pointer: this runs 8 x 29.4 kHz, also in Debug builds
  const float x = comp_ * in;
  const float S = gb3_ * s[0] + gb2_ * s[1] + gb1_ * s[2] + b_ * s[3];
  const float lin = (g4_ * x + S) / (1.0f + k_ * g4_);
  const float t = std::fabs(lin) > 1e-6f ? softClip(lin) / lin : 1.0f;
  const float y4 = (g4_ * x + S) / (1.0f + k_ * t * g4_);
  float u = x - k_ * t * y4;
  for (int i = 0; i < 4; ++i) {
    const float v = g_ * (u - s[i]);
    const float y = v + s[i];
    s[i] = y + v;
    u = y;
  }
  return u;
}

void VoiceFilter::save(StateWriter& w) const {
  w.tag(fourcc("VFLT"));
  w.f64(rate_);
  w.f64(cutoff_);
  w.f64(res_);
  for (float s : s_) putF32(w, s);
}

void VoiceFilter::load(StateReader& r) {
  r.tag(fourcc("VFLT"));
  const double rate = r.f64(), cutoff = r.f64(), res = r.f64();
  std::array<float, 4> s{};
  for (float& v : s) v = getF32(r);
  if (!(rate >= 1000.0 && rate < 1e7) || !std::isfinite(cutoff) || !(res >= 0.0 && res <= 1.0))
    throw StateError("voice filter: bad parameters");
  for (float v : s)
    if (!std::isfinite(v)) throw StateError("voice filter: bad state");
  rate_ = rate;
  cutoff_ = cutoff;
  res_ = res;
  s_ = s;
  update();
}

// --- FilterBank -------------------------------------------------------------------

FilterBank::FilterBank() { reset(); }

void FilterBank::reset() {
  filters_.fill(VoiceFilter{});
  cutoff_.fill(0);
  res_.fill(0);
  dac_ = 0;
  dirty_ = 0xFF;
  rate_ = 0.0;
  noise_ = 0x2545F491u;
  mix_ = 0.0f;
  out_.fill(0.0f);
  external_ = 0.0f;
}

// A write loads the DAC latch at the end of the cycle; the sample-and-holds
// it strobes are open during the cycle, so they take the value the DAC held
// before it: the OS's preset at $E418+ch (filters.md section 2).
void FilterBank::cvWrite(uint8_t offset, uint8_t value) {
  const int ch = offset & 7;
  if (!(offset & 0x08) && cutoff_[size_t(ch)] != dac_) {
    cutoff_[size_t(ch)] = dac_;
    dirty_ |= uint8_t(1 << ch);
  }
  if (!(offset & 0x10) && res_[size_t(ch)] != dac_) {
    res_[size_t(ch)] = dac_;
    dirty_ |= uint8_t(1 << ch);
  }
  dac_ = value;
}

void FilterBank::process(const std::array<float, kVoices>& voiceIn, double rateHz, float& mixOut) {
  if (rateHz != rate_) {
    rate_ = rateHz;
    for (VoiceFilter& f : filters_) f.setSampleRate(rateHz);
    dirty_ = 0xFF;
  }
  float mix = 0.0f;
  VoiceFilter* f = filters_.data();
  const float* in = voiceIn.data();
  uint32_t noise = noise_;
  for (int ch = 0; ch < kVoices; ++ch) {
    if (dirty_ & (1 << ch)) {
      f[ch].setCutoffHz(cutoffHzFromCv(cutoff_[size_t(ch)]));
      f[ch].setResonance(resonanceFromCv(res_[size_t(ch)]));
    }
    noise ^= noise << 13;  // xorshift32
    noise ^= noise >> 17;
    noise ^= noise << 5;
    const float n = (float(noise >> 8) * (1.0f / 8388608.0f) - 1.0f) * kNoiseLevel;
    float x = in[ch] + n;
    // Only a real jack signal is added: x + 0.0f is x for every x but -0.0f,
    // and skipping it makes "nothing plugged in" exact by construction.
    if (ch == kVoices - 1 && external_ != 0.0f) x += external_;
    const float y = f[ch].process(x);
    out_[size_t(ch)] = y;
    mix += y;
  }
  noise_ = noise;
  dirty_ = 0;
  mix_ = mix;
  mixOut = mix;
}

void FilterBank::save(StateWriter& w) const {
  w.tag(fourcc("FBNK"));
  for (const VoiceFilter& f : filters_) f.save(w);
  w.bytes(cutoff_);
  w.bytes(res_);
  w.u8(dac_);
  w.u8(dirty_);
  w.f64(rate_);
  w.u32(noise_);
  putF32(w, mix_);
  for (float o : out_) putF32(w, o);
  putF32(w, external_);
}

void FilterBank::load(StateReader& r) {
  r.tag(fourcc("FBNK"));
  std::array<VoiceFilter, kVoices> filters = filters_;
  for (VoiceFilter& f : filters) f.load(r);
  std::array<uint8_t, kVoices> cutoff{}, res{};
  r.bytes(cutoff);
  r.bytes(res);
  const uint8_t dac = r.u8(), dirty = r.u8();
  const double rate = r.f64();
  const uint32_t noise = r.u32();
  const float mix = getF32(r);
  std::array<float, kVoices> out{};
  for (float& o : out) o = getF32(r);
  const float external = getF32(r);
  if (noise == 0) throw StateError("filter bank: bad noise state");
  if (!(rate == 0.0 || (rate >= 1000.0 && rate < 1e7)) || !std::isfinite(mix) || !std::isfinite(external))
    throw StateError("filter bank: bad state");
  for (float o : out)
    if (!std::isfinite(o)) throw StateError("filter bank: bad state");
  filters_ = filters;
  cutoff_ = cutoff;
  res_ = res;
  dac_ = dac;
  dirty_ = dirty;
  rate_ = rate;
  noise_ = noise;
  mix_ = mix;
  out_ = out;
  external_ = external;
}

}  // namespace mirage
