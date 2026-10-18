#pragma once
#include <array>
#include <cstdint>

#include "mirage/state_io.h"

namespace mirage {

// The Mirage's analog voice section (docs/hardware/filters.md): each of the 8
// DOC channels feeds its own CEM3328 4-pole low-pass, whose cutoff and
// resonance come from sample-and-holds loaded through one 8-bit DAC at
// $E400-$E41F. The CV-byte mappings below are the only place the model's
// Hz-per-step and resonance curve live (filters.md section 5).

// Cutoff in Hz for a cutoff CV byte: exponential, 24 steps per octave, with
// CV $7A (the OS's calibration centre) at the 1213 Hz the ROM's calibration
// $F571 accepts. About 36 Hz at $00, 20 kHz at $DB.
double cutoffHzFromCv(uint8_t cv);
// Resonance 0..1 for a resonance CV byte (VoiceFilter::setResonance scale):
// a reverse-audio taper, self-oscillation from CV $A3 (3.2 V of 5 V).
double resonanceFromCv(uint8_t cv);
// The DOC A/D code for an analog level on the compressed input (ADC mux 0):
// a soft limiter (the input compressor) into 8 bits, $80 = 0 V.
uint8_t compressedAdcCode(float level);

// 4-pole resonant low-pass, CEM3328 in spirit: a TPT/ZDF ladder of four
// one-poles with a soft limiter in the feedback path. Self-oscillates from
// resonance 0.952 (loop gain 4) and loses about 12 dB of passband at
// resonance 1. Deterministic; no allocation.
class VoiceFilter {
 public:
  VoiceFilter();
  void setSampleRate(double hz);
  void setCutoffHz(double hz);    // clamped to 1 Hz .. 0.45 x sample rate
  void setResonance(double r01);  // clamped to 0..1
  float process(float in);
  void reset();  // clears the signal state, keeps the parameters

  void save(StateWriter&) const;
  void load(StateReader&);  // commits only on success

 private:
  void update();

  double rate_ = 48000.0, cutoff_ = 1000.0, res_ = 0.0;
  // Coefficients derived from the three parameters by update().
  float g_ = 0, b_ = 0, gb1_ = 0, gb2_ = 0, gb3_ = 0, g4_ = 0, k_ = 0, comp_ = 0;
  std::array<float, 4> s_{};  // one-pole integrator states
};

// The 8 voice filters and their CV path. A write to $E400+offset loads the
// DAC latch with the value; address bit 3 low strobes the cutoff
// sample-and-hold of channel offset & 7, bit 4 low its resonance one, and a
// strobed sample-and-hold takes the value the DAC held before the write:
//   $E400-$E407 both, $E408-$E40F resonance, $E410-$E417 cutoff,
//   $E418-$E41F neither (DAC preset only).
class FilterBank {
 public:
  static constexpr int kVoices = 8;

  FilterBank();
  void reset();  // power-on: CVs, DAC and filter states 0
  void cvWrite(uint8_t offset, uint8_t value);  // offset 0..0x1F
  uint8_t cutoffCv(int ch) const { return cutoff_[size_t(ch & 7)]; }
  uint8_t resonanceCv(int ch) const { return res_[size_t(ch & 7)]; }
  uint8_t dac() const { return dac_; }
  // One DOC scan: each voice (plus its filter's input noise) through its
  // filter, summed into mixOut. mix() keeps the last sum (the analog value the
  // A/D sees).
  void process(const std::array<float, kVoices>& voiceIn, double rateHz, float& mixOut);
  float mix() const { return mix_; }

  void save(StateWriter&) const;
  void load(StateReader&);  // commits only on success

 private:
  std::array<VoiceFilter, kVoices> filters_{};
  std::array<uint8_t, kVoices> cutoff_{}, res_{};
  uint8_t dac_ = 0;
  uint8_t dirty_ = 0xFF;  // channels whose CVs changed since their last process
  double rate_ = 0.0;     // rate the filters were last set to (0 = none yet)
  uint32_t noise_ = 1;    // xorshift32 state, never 0
  float mix_ = 0.0f;
};

}  // namespace mirage
