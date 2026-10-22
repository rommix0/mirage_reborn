#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "mirage/state_io.h"

namespace mirage {

// Streaming sample-rate converter for the host audio path: windowed-sinc
// (Kaiser, 64 taps at unity ratio) with the kernel table linearly
// interpolated. Input samples arrive at an input rate that can change at run
// time (the DOC's scan rate follows its oscillator-enable count); the output
// is at a fixed output rate. A rate change takes effect where the new-rate
// samples begin in the stream, and nothing is reset, so it does not click.
// When the input rate exceeds the output rate the kernel is stretched, which
// band-limits to the output Nyquist. Deterministic; no allocation after
// construction.
//
// Group delay is 32 input samples: pop() returns false until that many input
// samples beyond the output position have been pushed.
class StreamResampler {
 public:
  // Kernel half-width in input samples at unity ratio, and the lowest
  // out/in ratio the kernel is stretched to.
  static constexpr int kHalfWidth = 32;
  static constexpr double kMinStretchRatio = 0.04;

  explicit StreamResampler(double outRate);
  // How many input samples pop() needs on each side of the output position for
  // these rates: kHalfWidth, stretched by in/out when the input is faster.
  // pop() returns false until reach() + 1 samples are buffered().
  static int reach(double inHz, double outHz);
  int reach() const { return reach(inRate_, outRate_); }
  // Input samples pushed beyond the output position (negative once the
  // position has passed everything pushed).
  int64_t buffered() const { return int64_t(count_) - posInt_; }

  void setInputRate(double hz);   // from the next push() on
  void setOutputRate(double hz);  // from the next pop() on
  double inputRate() const { return inRate_; }
  double outputRate() const { return outRate_; }
  void push(float s);
  // Produces the next output sample if enough input has been pushed.
  bool pop(float& out);
  // Back to the start-up state (silence history, position 0); keeps the rates.
  void reset();

  void save(StateWriter&) const;
  void load(StateReader&);  // commits only on success

 private:
  static constexpr int kRing = 4096;  // input history (power of two)
  struct RateChange {
    int64_t index;  // first input sample at the new rate
    double hz;
  };

  void applyRateChanges();

  double outRate_, inRate_;
  std::array<float, kRing> ring_{};
  uint64_t count_ = 0;     // input samples pushed
  int64_t posInt_ = 0;     // next output sits at posInt_ + posFrac_ (input samples)
  double posFrac_ = 0.0;
  std::vector<RateChange> changes_;  // not yet reached by the output position
  double lastRequested_;             // the most recent setInputRate argument
};

}  // namespace mirage
