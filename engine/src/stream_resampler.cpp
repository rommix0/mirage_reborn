#include "mirage/stream_resampler.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace mirage {

namespace {

constexpr int kHalf = StreamResampler::kHalfWidth;  // kernel half-width in input samples (64 taps)
constexpr int kPhases = 256;     // table entries per input sample
constexpr double kCutoff = 0.94;  // kernel cutoff as a fraction of the input Nyquist
constexpr double kBeta = 9.0;     // Kaiser window shape (about 90 dB stop band)
constexpr double kMinStretch = StreamResampler::kMinStretchRatio;
constexpr int kMaxTaps = int(kHalf / kMinStretch) + 1;
constexpr double kMinHz = 1.0, kMaxHz = 1e7;
constexpr size_t kMaxChanges = 64;

double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 60; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < 1e-18 * sum) break;
  }
  return sum;
}

// g(u) for u = n / kPhases, 0 <= u <= kHalf: the windowed low-pass sinc.
struct Kernel {
  std::array<float, kHalf * kPhases + 2> g{};
  Kernel() {
    const double i0b = besselI0(kBeta);
    for (size_t n = 0; n < g.size(); ++n) {
      const double u = double(n) / kPhases;
      const double x = std::numbers::pi * kCutoff * u;
      const double sinc = n == 0 ? 1.0 : std::sin(x) / x;
      const double r = std::min(u / kHalf, 1.0);
      const double w = besselI0(kBeta * std::sqrt(1.0 - r * r)) / i0b;
      g[n] = float(kCutoff * sinc * w);
    }
  }
};

const Kernel& kernel() {
  static const Kernel k;
  return k;
}

double clampRate(double hz) {
  if (!(hz >= kMinHz)) return kMinHz;  // also catches NaN
  return std::min(hz, kMaxHz);
}

}  // namespace

StreamResampler::StreamResampler(double outRate)
    : outRate_(clampRate(outRate)), inRate_(outRate_), lastRequested_(outRate_) {
  (void)kernel();
  changes_.reserve(kMaxChanges + 1);  // setInputRate never allocates afterwards
}

int StreamResampler::reach(double inHz, double outHz) {
  const double ratio = clampRate(outHz) / clampRate(inHz);
  return ratio < 1.0 ? int(std::ceil(kHalf / std::max(ratio, kMinStretch))) : kHalf;
}

void StreamResampler::setOutputRate(double hz) { outRate_ = clampRate(hz); }

void StreamResampler::setInputRate(double hz) {
  hz = clampRate(hz);
  if (hz == lastRequested_) return;
  lastRequested_ = hz;
  if (count_ == 0) {  // nothing pushed yet: the stream simply starts at this rate
    inRate_ = hz;
    changes_.clear();
    return;
  }
  changes_.push_back({int64_t(count_), hz});
  if (changes_.size() > kMaxChanges) {
    inRate_ = changes_.front().hz;
    changes_.erase(changes_.begin());
  }
}

void StreamResampler::push(float s) {
  ring_[count_ & (kRing - 1)] = s;
  ++count_;
  // A caller that never pops would overwrite history still needed: skip ahead.
  const int64_t oldestNeeded = posInt_ - kMaxTaps;
  if (int64_t(count_) - oldestNeeded > kRing) {
    posInt_ = int64_t(count_) - kRing + kMaxTaps;
    posFrac_ = 0.0;
  }
}

void StreamResampler::applyRateChanges() {
  size_t n = 0;
  while (n < changes_.size() && posInt_ >= changes_[n].index) inRate_ = changes_[n++].hz;
  if (n) changes_.erase(changes_.begin(), changes_.begin() + std::ptrdiff_t(n));
}

bool StreamResampler::pop(float& out) {
  applyRateChanges();
  const double ratio = outRate_ / inRate_;
  const bool stretch = ratio < 1.0;  // input faster than output: band-limit to the output
  const double fc = stretch ? std::max(ratio, kMinStretch) : 1.0;
  const int taps = reach();  // on each side; the same arithmetic as fc above
  if (posInt_ + taps > int64_t(count_) - 1) return false;

  const float* g = kernel().g.data();
  const auto at = [this](int64_t k) { return double(ring_[uint64_t(k) & (kRing - 1)]); };
  double acc = 0.0;
  if (!stretch) {
    // Taps at distance |frac - j| from the output; every tap shares the table
    // fraction, so the lookups are one index apart per input sample.
    const double s = posFrac_ * kPhases;
    const int i0 = int(s);
    const float a = float(s - i0);
    for (int j = -kHalf + 1; j <= 0; ++j) {  // distance frac + |j|
      const int idx = -j * kPhases + i0;
      acc += at(posInt_ + j) * double(g[idx] + (g[idx + 1] - g[idx]) * a);
    }
    for (int j = 1; j <= kHalf; ++j) {  // distance j - frac
      const int idx = j * kPhases - i0 - 1;
      acc += at(posInt_ + j) * double(g[idx] + (g[idx + 1] - g[idx]) * (1.0f - a));
    }
  } else {
    for (int j = -taps + 1; j <= taps; ++j) {
      const double u = std::fabs(posFrac_ - j) * fc;
      if (u >= kHalf) continue;
      const double x = u * kPhases;
      const int idx = int(x);
      const float a = float(x - idx);
      acc += at(posInt_ + j) * double(g[idx] + (g[idx + 1] - g[idx]) * a) * fc;
    }
  }
  out = float(acc);

  posFrac_ += inRate_ / outRate_;
  const double whole = std::floor(posFrac_);
  posInt_ += int64_t(whole);
  posFrac_ -= whole;
  return true;
}

void StreamResampler::reset() {
  ring_.fill(0.0f);
  count_ = 0;
  posInt_ = 0;
  posFrac_ = 0.0;
  changes_.clear();
  inRate_ = lastRequested_;
}

void StreamResampler::save(StateWriter& w) const {
  w.tag(fourcc("RSMP"));
  w.f64(outRate_);
  w.f64(inRate_);
  w.f64(lastRequested_);
  w.u64(count_);
  w.u64(uint64_t(posInt_));
  w.f64(posFrac_);
  w.u32(uint32_t(changes_.size()));
  for (const RateChange& c : changes_) {
    w.u64(uint64_t(c.index));
    w.f64(c.hz);
  }
  for (float f : ring_) w.u32(std::bit_cast<uint32_t>(f));
}

void StreamResampler::load(StateReader& r) {
  r.tag(fourcc("RSMP"));
  const auto rate = [&r] {
    const double v = r.f64();
    if (!(v >= kMinHz && v <= kMaxHz)) throw StateError("resampler: bad rate");
    return v;
  };
  const double outRate = rate(), inRate = rate(), lastRequested = rate();
  const uint64_t count = r.u64();
  const int64_t posInt = int64_t(r.u64());
  const double posFrac = r.f64();
  if (posInt < 0 || uint64_t(posInt) > count + 1 || !(posFrac >= 0.0 && posFrac < 1.0))
    throw StateError("resampler: bad position");
  const uint32_t n = r.u32();
  if (n > kMaxChanges) throw StateError("resampler: bad rate-change list");
  std::vector<RateChange> changes;
  for (uint32_t i = 0; i < n; ++i) {
    RateChange c;
    c.index = int64_t(r.u64());
    c.hz = rate();
    if (c.index < 0 || uint64_t(c.index) > count) throw StateError("resampler: bad rate change");
    changes.push_back(c);
  }
  std::array<float, kRing> ring;
  for (float& f : ring) f = std::bit_cast<float>(r.u32());
  outRate_ = outRate;
  inRate_ = inRate;
  lastRequested_ = lastRequested;
  count_ = count;
  posInt_ = posInt;
  posFrac_ = posFrac;
  changes_.assign(changes.begin(), changes.end());  // keeps the reserved capacity
  ring_ = ring;
}

}  // namespace mirage
