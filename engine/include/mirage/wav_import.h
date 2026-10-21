#pragma once
// WAV import: read a WAV file and convert part of it into the unsigned 8-bit
// bytes the Mirage keeps in wave RAM (two 64 KB halves of 256-byte pages).
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace mirage {

// The message is fit to show the user as it is.
struct WavError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct WavAudio {
  double rate = 0;
  int channels = 0;
  int bits = 0;
  bool isFloat = false;
  std::vector<float> mono;  // channels averaged, -1..1 nominal
};

// Accepts PCM (8-bit unsigned, 16, 24, 32-bit), 32-bit float, and
// WAVE_FORMAT_EXTENSIBLE with a PCM or float subformat; mono or stereo; any
// rate. Throws WavError otherwise.
WavAudio readWav(std::span<const uint8_t> file);
WavAudio readWavFile(const std::filesystem::path& file);

enum class WaveHalf : uint8_t { Lower, Upper };  // wave RAM $00000 / $10000

// The Mirage's own sampling range in Hz. The sample times it offers are whole
// microseconds and rate = 1,000,000 / time (Advanced Sampler's Guide, [73]
// Sample Time Adjust, p. 22; Appendix A "Mirage Sample Rates", pp. 70-72).
// Without the optional Input Sampling Filter cartridge the times run from
// 30 us (33,333 Hz) to 99 us (10,101 Hz); the table's settings below 30 (down
// to 20 us, 50 kHz) need the cartridge. The default is 34 us (29,412 Hz),
// "the default sample rate of 29.4 KHz" (p. 50). The Guide prints no range in
// Hz; these are the table's own endpoints, rounded to whole Hz.
inline constexpr double kMinImportRate = 10101.0;
inline constexpr double kMaxImportRate = 33333.0;
inline constexpr double kDefaultImportRate = 29412.0;

// The pitch hint: under OS 3.2 with SND-A01 as booted, a sine imported at
// kPitchHintRate Hz plays at its original pitch around MIDI key kPitchHintKey
// (C4 = 60; measured in tests/test_acceptance.cpp, method and result in
// docs/app.md). Another rate r moves it 12 log2(r / kPitchHintRate) semitones.
inline constexpr double kPitchHintKey = 47.3, kPitchHintRate = 29412.0;

// The MIDI note number nearest to kPitchHintKey + 12 log2(rate / kPitchHintRate).
int originalPitchKey(double rate);

struct ImportSettings {
  double rate = kDefaultImportRate;  // Hz the converted bytes are meant to play at
  double startSeconds = 0;           // into the file
  double endSeconds = 0;             // 0 (or past the end) means the end of the file
  WaveHalf half = WaveHalf::Lower;
  int startPage = 0;  // 0-255, 256-byte pages from the start of the half
  bool normalize = true;
};

struct ImportPlan {
  uint32_t address = 0;       // wave RAM address: half * 0x10000 + startPage * 256
  size_t bytes = 0;           // bytes to write, cut off at the end of the half
  int lastPage = 0;           // last 256-byte page (0-255 within the half) the sample occupies
  double seconds = 0;         // playing time of those bytes at the chosen rate
  double fractionOfHalf = 0;  // bytes / 65536
  bool truncated = false;     // the selection did not fit before the end of the half
  std::string problem;        // non-empty: refuse the import (user-facing text)
};

ImportPlan planImport(const WavAudio&, const ImportSettings&);

// Mono mix, low-pass at 0.45 x the lower of the two rates, windowed-sinc
// resample, optional peak normalise, round to unsigned 8-bit ($80 = 0, no
// dither). $00 is written as $01 because a zero byte halts the DOC. The size
// is planImport(...).bytes (empty when the plan has a problem).
std::vector<uint8_t> convertForWaveRam(const WavAudio&, const ImportSettings&);

}  // namespace mirage
