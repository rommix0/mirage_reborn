// Voice filters and their CV path (Task 11). The decode and the mappings are
// documented with their evidence in docs/hardware/filters.md.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "mirage/state_io.h"
#include "mirage/voice_filter.h"

using namespace mirage;

namespace {

constexpr double kRate = 48000.0;
constexpr double kDocRate = 29411.76;  // DOC scan rate with 32 oscillators

VoiceFilter makeFilter(double cutoffHz, double res, double rate = kRate) {
  VoiceFilter f;
  f.setSampleRate(rate);
  f.setCutoffHz(cutoffHz);
  f.setResonance(res);
  return f;
}

// Gain in dB of the `freq` component of the output for a sine of amplitude
// `amp` (lock-in over the second half of 1 s, so a self-oscillation at another
// frequency does not count).
double sineGainDb(VoiceFilter& f, double freq, double amp) {
  const int n = int(kRate);
  double re = 0, im = 0;
  for (int i = 0; i < n; ++i) {
    const double ph = 2 * std::numbers::pi * freq * i / kRate;
    const float y = f.process(float(amp * std::sin(ph)));
    if (i >= n / 2) {
      re += y * std::sin(ph);
      im += y * std::cos(ph);
    }
  }
  const double a = 2 * std::hypot(re, im) / (n - n / 2);
  return 20 * std::log10(a / amp);
}

double rms(const std::vector<float>& v, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to; ++i) s += double(v[i]) * v[i];
  return std::sqrt(s / double(to - from));
}

// Frequency from rising zero crossings of v[from..to).
double zeroCrossHz(const std::vector<float>& v, size_t from, size_t to, double rate) {
  size_t first = 0, last = 0, count = 0;
  for (size_t i = from; i + 1 < to; ++i)
    if (v[i] < 0 && v[i + 1] >= 0) {
      if (count == 0) first = i;
      last = i;
      ++count;
    }
  return count < 2 ? 0.0 : double(count - 1) * rate / double(last - first);
}

// The OS's way to load a sample-and-hold: preset the DAC at $E418+ch, then
// write the same value to the strobe address ($F5DC, $F5E7, OS $9742).
void load(FilterBank& b, uint8_t offset, uint8_t value) {
  b.cvWrite(uint8_t(0x18 | (offset & 7)), value);
  b.cvWrite(offset, value);
}

}  // namespace

// ---- VoiceFilter ------------------------------------------------------------

TEST_CASE("filter: passband is flat at resonance 0") {
  VoiceFilter f = makeFilter(5000, 0.0);
  const double g = sineGainDb(f, 100, 0.5);
  CHECK(g > -1.0);
  CHECK(g < 1.0);
}

TEST_CASE("filter: 20 kHz is at least 30 dB down with a 5 kHz cutoff") {
  VoiceFilter f = makeFilter(5000, 0.0);
  CHECK(sineGainDb(f, 20000, 0.5) < -30.0);
}

TEST_CASE("filter: the passband drops as resonance rises (CEM3328)") {
  VoiceFilter f = makeFilter(1000, 1.0);
  const double g = sineGainDb(f, 100, 0.5);
  CHECK(g > -14.0);
  CHECK(g < -3.0);
}

TEST_CASE("filter: self-oscillates at resonance 1 after one impulse") {
  VoiceFilter f = makeFilter(1000, 1.0);
  std::vector<float> out(2 * size_t(kRate));
  for (size_t i = 0; i < out.size(); ++i) out[i] = f.process(i == 0 ? 1.0f : 0.0f);
  CHECK(rms(out, size_t(kRate), out.size()) > 0.01);
  // A ladder at loop gain 4 oscillates at its cutoff.
  CHECK(zeroCrossHz(out, size_t(kRate), out.size(), kRate) == Catch::Approx(1000).epsilon(0.02));
}

TEST_CASE("filter: an impulse dies away below the oscillation threshold") {
  VoiceFilter f = makeFilter(1000, 0.9);
  std::vector<float> out(2 * size_t(kRate));
  for (size_t i = 0; i < out.size(); ++i) out[i] = f.process(i == 0 ? 1.0f : 0.0f);
  CHECK(rms(out, size_t(kRate), out.size()) < 1e-6);
}

TEST_CASE("filter: stable for 10 s of full-scale square at every cutoff", "[slow]") {
  bool finite = true;
  for (double res : {0.0, 1.0}) {
    for (double fc = 20.0; fc <= 20000.0 * 1.001; fc *= std::pow(2.0, 1.0 / 3.0)) {
      VoiceFilter f = makeFilter(fc, res);
      for (int i = 0; i < 10 * int(kRate); ++i) {
        const float y = f.process((i / 109) % 2 ? 1.0f : -1.0f);  // ~220 Hz
        if (!std::isfinite(y)) finite = false;
      }
    }
  }
  CHECK(finite);
}

TEST_CASE("filter: save/load restores parameters and state exactly") {
  VoiceFilter a = makeFilter(800, 0.7, kDocRate);
  for (int i = 0; i < 1000; ++i) a.process(i % 50 < 25 ? 0.5f : -0.5f);
  StateWriter w;
  a.save(w);
  const auto blob = w.take();
  VoiceFilter b;
  StateReader r(blob);
  b.load(r);
  CHECK(r.atEnd());
  for (int i = 0; i < 1000; ++i) {
    const float in = i % 37 < 18 ? 0.3f : -0.3f;
    REQUIRE(a.process(in) == b.process(in));
  }
}

TEST_CASE("filter: a truncated state throws and leaves the filter unchanged") {
  VoiceFilter a = makeFilter(800, 0.7, kDocRate);
  for (int i = 0; i < 100; ++i) a.process(0.5f);
  StateWriter w;
  a.save(w);
  auto blob = w.take();
  REQUIRE(blob.size() > 1);
  blob.pop_back();
  VoiceFilter b = makeFilter(300, 0.2, kDocRate);
  VoiceFilter ref = makeFilter(300, 0.2, kDocRate);
  StateReader r(blob);
  CHECK_THROWS_AS(b.load(r), StateError);
  for (int i = 0; i < 100; ++i) REQUIRE(b.process(0.25f) == ref.process(0.25f));
}

// ---- CV mappings -------------------------------------------------------------

TEST_CASE("cutoff CV: exponential, 24 steps per octave, about 36 Hz to above 20 kHz") {
  for (int cv = 0; cv + 24 <= 255; ++cv)
    REQUIRE(cutoffHzFromCv(uint8_t(cv + 24)) == Catch::Approx(2 * cutoffHzFromCv(uint8_t(cv))));
  CHECK(cutoffHzFromCv(0x00) > 30.0);
  CHECK(cutoffHzFromCv(0x00) < 40.0);
  CHECK(cutoffHzFromCv(0xDB) > 19000.0);
  CHECK(cutoffHzFromCv(0xDB) < 21000.0);
  // The ROM calibration's target (filters.md 4): 1213 Hz at the OS's centre $7A.
  CHECK(cutoffHzFromCv(0x7A) == Catch::Approx(1213.0).margin(1.0));
}

TEST_CASE("resonance CV: 0 at $00, 1 at $FF, monotonic, oscillation from $A3") {
  CHECK(resonanceFromCv(0x00) == 0.0);
  CHECK(resonanceFromCv(0xFF) == 1.0);
  for (int cv = 0; cv < 255; ++cv)
    REQUIRE(resonanceFromCv(uint8_t(cv)) < resonanceFromCv(uint8_t(cv + 1)));
  // Loop gain 4 (self-oscillation) is reached at 3.2 V of the 5 V CV range.
  CHECK(resonanceFromCv(0xA2) < 4.0 / 4.2);
  CHECK(resonanceFromCv(0xA3) > 4.0 / 4.2);
}

TEST_CASE("compressed input: $80 is silence, the limiter keeps it in 8 bits") {
  CHECK(compressedAdcCode(0.0f) == 0x80);
  CHECK(compressedAdcCode(0.25f) > 0x90);
  CHECK(compressedAdcCode(-0.25f) < 0x70);
  CHECK(compressedAdcCode(100.0f) == 0xFF);
  CHECK(compressedAdcCode(-100.0f) == 0x01);
  for (int i = -300; i < 300; ++i)
    REQUIRE(compressedAdcCode(float(i) / 100) <= compressedAdcCode(float(i + 1) / 100));
}

// ---- CV decode ($E400-$E41F, filters.md 2) ---------------------------------------

TEST_CASE("CV decode: $E418-$E41F only preset the DAC") {
  FilterBank b;
  b.cvWrite(0x1E, 0x99);
  CHECK(b.dac() == 0x99);
  for (int ch = 0; ch < 8; ++ch) {
    CHECK(b.cutoffCv(ch) == 0);
    CHECK(b.resonanceCv(ch) == 0);
  }
}

TEST_CASE("CV decode: $E410-$E417 strobe the cutoff of channel addr & 7") {
  FilterBank b;
  load(b, 0x13, 0x40);
  CHECK(b.cutoffCv(3) == 0x40);
  CHECK(b.resonanceCv(3) == 0x00);
  CHECK(b.dac() == 0x40);
  for (int ch = 0; ch < 8; ++ch)
    if (ch != 3) CHECK(b.cutoffCv(ch) == 0);
}

TEST_CASE("CV decode: $E408-$E40F strobe the resonance of channel addr & 7") {
  FilterBank b;
  load(b, 0x0B, 0x40);
  CHECK(b.resonanceCv(3) == 0x40);
  CHECK(b.cutoffCv(3) == 0x00);
  CHECK(b.dac() == 0x40);
}

TEST_CASE("CV decode: $E400-$E407 strobe both") {
  FilterBank b;
  load(b, 0x05, 0x77);
  CHECK(b.cutoffCv(5) == 0x77);
  CHECK(b.resonanceCv(5) == 0x77);
}

TEST_CASE("CV decode: a strobe takes the DAC's previous value, then the DAC loads the new one") {
  FilterBank b;
  b.cvWrite(0x1B, 0x20);  // preset $20
  b.cvWrite(0x13, 0x40);  // cutoff 3 <- $20, DAC <- $40
  CHECK(b.cutoffCv(3) == 0x20);
  CHECK(b.dac() == 0x40);
  b.cvWrite(0x0B, 0x60);  // resonance 3 <- $40
  CHECK(b.resonanceCv(3) == 0x40);
  CHECK(b.cutoffCv(3) == 0x20);
  b.cvWrite(0x02, 0x00);  // both of channel 2 <- $60
  CHECK(b.cutoffCv(2) == 0x60);
  CHECK(b.resonanceCv(2) == 0x60);
  CHECK(b.dac() == 0x00);
}

// SP $97B8-$97C8 (and ROM $F0FC-$F106) clear with function 0 and no preset:
// LDX #0; LDA #0; STA $E400,X; LEAX 1,X; CMPX #8; BNE; STA $E418. With
// previous-value latching (filters.md 2) channels 1-7 clear, and channel 0
// takes the DAC value from before the loop.
TEST_CASE("CV decode: a clear loop without preset leaves channel 0 at the previous DAC value") {
  FilterBank b;
  for (int ch = 0; ch < 8; ++ch) {  // all CVs set, as after playing
    load(b, uint8_t(0x10 | ch), 0x90);
    load(b, uint8_t(0x08 | ch), 0x55);
  }
  b.cvWrite(0x1D, 0x6B);  // SP $A974 ends with the DAC at the last resonance
  b.cvWrite(0x0D, 0x6B);
  b.cvWrite(0x1D, 0x6B);
  REQUIRE(b.dac() == 0x6B);
  for (uint8_t x = 0; x < 8; ++x) b.cvWrite(x, 0x00);
  b.cvWrite(0x18, 0x00);
  CHECK(b.cutoffCv(0) == 0x6B);
  CHECK(b.resonanceCv(0) == 0x6B);
  for (int ch = 1; ch < 8; ++ch) {
    CHECK(b.cutoffCv(ch) == 0x00);
    CHECK(b.resonanceCv(ch) == 0x00);
  }
  CHECK(b.dac() == 0x00);
}

TEST_CASE("CV decode: the ROM's $F5DC sequence sets resonance $FF and cutoff B") {
  // $F5DC with X = $E41A, B = $9C: ,X <- $FF; -$10,X <- $FF; ,X <- B; -8,X <- B.
  FilterBank b;
  b.cvWrite(0x1A, 0xFF);
  b.cvWrite(0x0A, 0xFF);
  b.cvWrite(0x1A, 0x9C);
  b.cvWrite(0x12, 0x9C);
  CHECK(b.resonanceCv(2) == 0xFF);
  CHECK(b.cutoffCv(2) == 0x9C);
  CHECK(b.dac() == 0x9C);
  // $F5B1-$F5B5 after the calibration: CLRB, JSR $F5DC, STB -$10,X.
  b.cvWrite(0x1A, 0xFF);
  b.cvWrite(0x0A, 0xFF);
  b.cvWrite(0x1A, 0x00);
  b.cvWrite(0x12, 0x00);
  b.cvWrite(0x0A, 0x00);
  CHECK(b.resonanceCv(2) == 0x00);
  CHECK(b.cutoffCv(2) == 0x00);
}

TEST_CASE("CV decode: the OS's $F5E7 sets cutoff A and resonance B") {
  // $F5E7 with U = $E41D, A = $83, B = $2C: ,U <- A; -8,U <- A; ,U+ <- B; -$11,U <- B.
  FilterBank b;
  b.cvWrite(0x1D, 0x83);
  b.cvWrite(0x15, 0x83);
  b.cvWrite(0x1D, 0x2C);
  b.cvWrite(0x0D, 0x2C);
  CHECK(b.cutoffCv(5) == 0x83);
  CHECK(b.resonanceCv(5) == 0x2C);
}

// ---- FilterBank ----------------------------------------------------------------

TEST_CASE("bank: an open filter passes its voice into the mix") {
  FilterBank b;
  load(b, 0x13, 0xFF);  // channel 3 cutoff fully open, resonance 0
  std::array<float, 8> in{};
  in[3] = 0.5f;
  float mix = 0;
  for (int i = 0; i < 2000; ++i) b.process(in, kDocRate, mix);
  CHECK(mix == Catch::Approx(0.5).margin(0.01));
  CHECK(b.mix() == mix);
}

TEST_CASE("bank: a closed filter blocks its voice") {
  FilterBank b;  // power-on CVs are 0: cutoff ~36 Hz
  std::array<float, 8> in{};
  float mix = 0;
  double peak = 0;
  for (int i = 0; i < 30000; ++i) {
    in[1] = (i / 7) % 2 ? 0.5f : -0.5f;  // ~2.1 kHz square
    b.process(in, kDocRate, mix);
    if (i > 15000) peak = std::max(peak, double(std::fabs(mix)));
  }
  CHECK(peak < 0.01);
}

TEST_CASE("bank: resonance $FF self-oscillates from the input noise at the cutoff CV's frequency") {
  FilterBank b;
  load(b, 0x0C, 0xFF);  // channel 4 resonance
  load(b, 0x14, 0x7A);  // channel 4 cutoff
  const std::array<float, 8> silence{};
  std::vector<float> out(static_cast<size_t>(kDocRate));
  float mix = 0;
  for (float& o : out) {
    b.process(silence, kDocRate, mix);
    o = mix;
  }
  const size_t half = out.size() / 2;
  CHECK(rms(out, half, out.size()) > 0.1);
  CHECK(zeroCrossHz(out, half, out.size(), kDocRate) ==
        Catch::Approx(cutoffHzFromCv(0x7A)).epsilon(0.03));
}

TEST_CASE("bank: a CV change applies at the next scan") {
  FilterBank a, b;
  load(a, 0x13, 0x30);
  load(b, 0x13, 0x30);
  std::array<float, 8> in{};
  float ma = 0, mb = 0;
  for (int i = 0; i < 100; ++i) {
    in[3] = (i / 10) % 2 ? 0.5f : -0.5f;
    a.process(in, kDocRate, ma);
    b.process(in, kDocRate, mb);
  }
  REQUIRE(ma == mb);
  load(b, 0x13, 0xF0);
  a.process(in, kDocRate, ma);
  b.process(in, kDocRate, mb);
  CHECK(ma != mb);
}

TEST_CASE("bank: save/load continues bit-exactly") {
  FilterBank a;
  load(a, 0x12, 0x90);
  load(a, 0x0A, 0xC0);
  std::array<float, 8> in{};
  float mix = 0;
  for (int i = 0; i < 3000; ++i) {
    in[2] = (i / 20) % 2 ? 0.4f : -0.4f;
    a.process(in, kDocRate, mix);
  }
  load(a, 0x12, 0x55);  // a CV change not yet applied to the filter
  StateWriter w;
  a.save(w);
  const auto blob = w.take();
  FilterBank b;
  StateReader r(blob);
  b.load(r);
  CHECK(r.atEnd());
  CHECK(b.cutoffCv(2) == 0x55);
  CHECK(b.resonanceCv(2) == 0xC0);
  CHECK(b.dac() == 0x55);
  CHECK(b.mix() == a.mix());
  float ma = 0, mb = 0;
  for (int i = 0; i < 3000; ++i) {
    in[2] = (i / 13) % 2 ? 0.4f : -0.4f;
    a.process(in, kDocRate, ma);
    b.process(in, kDocRate, mb);
    REQUIRE(ma == mb);
  }
  StateWriter wa, wb;
  a.save(wa);
  b.save(wb);
  CHECK(wa.take() == wb.take());
}

TEST_CASE("bank: a bad state throws and leaves the bank unchanged") {
  FilterBank a;
  load(a, 0x01, 0x55);
  StateWriter w;
  a.save(w);
  auto blob = w.take();
  REQUIRE(blob.size() > 3);
  blob.resize(blob.size() - 3);
  FilterBank b;
  load(b, 0x13, 0x22);
  StateReader r(blob);
  CHECK_THROWS_AS(b.load(r), StateError);
  CHECK(b.cutoffCv(3) == 0x22);
  CHECK(b.cutoffCv(1) == 0x00);
  CHECK(b.dac() == 0x22);
}

TEST_CASE("bank: reset clears CVs, DAC and the signal") {
  FilterBank b;
  load(b, 0x00, 0xFF);
  std::array<float, 8> in{};
  in[0] = 1.0f;
  float mix = 0;
  for (int i = 0; i < 100; ++i) b.process(in, kDocRate, mix);
  b.reset();
  CHECK(b.cutoffCv(0) == 0);
  CHECK(b.resonanceCv(0) == 0);
  CHECK(b.dac() == 0);
  CHECK(b.mix() == 0.0f);
}
