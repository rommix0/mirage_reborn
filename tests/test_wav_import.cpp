// WAV reader and the converter that turns a WAV into Mirage wave RAM bytes.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "mirage/wav_import.h"

using namespace mirage;
using Catch::Approx;

namespace {

constexpr double kPi = 3.14159265358979323846;

void put(std::vector<uint8_t>& b, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void tag(std::vector<uint8_t>& b, const char (&t)[5]) {
  for (int i = 0; i < 4; ++i) b.push_back(uint8_t(t[i]));
}

struct WavSpec {
  int format = 1;  // 1 PCM, 3 float, 0xFFFE extensible
  int subFormat = 1;
  int channels = 1;
  int rate = 44100;
  int bits = 16;
  bool listFirst = false;      // an odd-sized LIST chunk (so padded) before "fmt "
  bool oddChunkAfter = false;  // an odd-sized chunk between "fmt " and "data"
  uint32_t overclaim = 0;      // bytes the data chunk claims beyond the file
  bool omitFmt = false;
  bool omitData = false;
  const char* riff = "RIFF";
};

// `data` is the interleaved samples, already encoded, little endian.
std::vector<uint8_t> makeWav(const WavSpec& s, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> b;
  b.insert(b.end(), s.riff, s.riff + 4);
  put(b, 0, 4);  // the RIFF size is not used by the reader
  tag(b, "WAVE");
  if (s.listFirst) {
    tag(b, "LIST");
    put(b, 5, 4);
    for (int i = 0; i < 5; ++i) b.push_back(uint8_t('a' + i));
    b.push_back(0);  // pad byte
  }
  if (!s.omitFmt) {
    tag(b, "fmt ");
    const bool ext = s.format == 0xFFFE;
    put(b, ext ? 40 : 16, 4);
    put(b, uint32_t(s.format), 2);
    put(b, uint32_t(s.channels), 2);
    put(b, uint32_t(s.rate), 4);
    put(b, uint32_t(s.rate) * uint32_t(s.channels) * uint32_t(s.bits / 8), 4);
    put(b, uint32_t(s.channels * (s.bits / 8)), 2);
    put(b, uint32_t(s.bits), 2);
    if (ext) {
      put(b, 22, 2);
      put(b, uint32_t(s.bits), 2);
      put(b, 0, 4);  // channel mask
      put(b, uint32_t(s.subFormat), 2);
      const uint8_t guid[14] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
      b.insert(b.end(), guid, guid + 14);
    }
  }
  if (s.oddChunkAfter) {
    tag(b, "junk");
    put(b, 3, 4);
    b.push_back(1);
    b.push_back(2);
    b.push_back(3);
    b.push_back(0);  // pad byte
  }
  if (!s.omitData) {
    tag(b, "data");
    put(b, data.size() + s.overclaim, 4);
    b.insert(b.end(), data.begin(), data.end());
  }
  return b;
}

// One sample of `v` (-1..1) in the given encoding.
void encode(std::vector<uint8_t>& d, double v, int bits, bool isFloat) {
  if (isFloat) {
    const float f = float(v);
    uint32_t u;
    static_assert(sizeof f == sizeof u);
    std::memcpy(&u, &f, 4);
    put(d, u, 4);
  } else if (bits == 8) {
    put(d, uint64_t(std::clamp(std::lround(v * 128.0) + 128, 0L, 255L)), 1);
  } else {
    const double full = std::ldexp(1.0, bits - 1);
    const double q = std::clamp(std::nearbyint(v * full), -full, full - 1.0);
    put(d, uint64_t(int64_t(q)), bits / 8);
  }
}

std::vector<uint8_t> sineData(int bits, bool isFloat, int channels, int n, double freq = 440.0,
                              int rate = 44100) {
  std::vector<uint8_t> d;
  for (int i = 0; i < n; ++i) {
    const double v = 0.8 * std::sin(2 * kPi * freq * i / rate);
    for (int c = 0; c < channels; ++c) encode(d, v, bits, isFloat);
  }
  return d;
}

std::span<const uint8_t> sp(const std::vector<uint8_t>& v) { return {v.data(), v.size()}; }

std::string wavError(const std::vector<uint8_t>& file) {
  try {
    readWav(sp(file));
  } catch (const WavError& e) {
    return e.what();
  }
  return "(no error)";
}

WavAudio sineAudio(double freq, double rate, double seconds, double amp = 0.9) {
  WavAudio a;
  a.rate = rate;
  a.channels = 1;
  a.bits = 16;
  const size_t n = size_t(seconds * rate);
  for (size_t i = 0; i < n; ++i)
    a.mono.push_back(float(amp * std::sin(2 * kPi * freq * double(i) / rate + 0.3)));
  return a;
}

double rmsOf(const std::vector<uint8_t>& bytes, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to; ++i) {
    const double v = (double(bytes[i]) - 128.0) / 127.0;
    s += v * v;
  }
  return std::sqrt(s / double(to - from));
}

std::vector<size_t> upCrossings(const std::vector<uint8_t>& out) {
  std::vector<size_t> up;
  for (size_t i = 1; i < out.size(); ++i)
    if (out[i - 1] < 128 && out[i] >= 128) up.push_back(i);
  return up;
}

}  // namespace

TEST_CASE("wav: every accepted format decodes to the same signal") {
  const int n = 2000;
  struct Fmt {
    int tag;
    int bits;
    bool isFloat;
    double lsb;
  };
  const Fmt fmts[] = {{1, 8, false, 1.0 / 128.0},
                      {1, 16, false, 1.0 / 32768.0},
                      {1, 24, false, 1.0 / 8388608.0},
                      {1, 32, false, 1e-6},
                      {3, 32, true, 1e-6}};
  for (const Fmt& f : fmts) {
    DYNAMIC_SECTION(f.bits << "-bit " << (f.isFloat ? "float" : "PCM")) {
      WavSpec s;
      s.format = f.tag;
      s.bits = f.bits;
      const WavAudio a = readWav(sp(makeWav(s, sineData(f.bits, f.isFloat, 1, n))));
      CHECK(a.rate == 44100);
      CHECK(a.channels == 1);
      CHECK(a.bits == f.bits);
      CHECK(a.isFloat == f.isFloat);
      REQUIRE(a.mono.size() == size_t(n));
      double worst = 0;
      for (int i = 0; i < n; ++i)
        worst = std::max(worst, std::abs(double(a.mono[size_t(i)]) -
                                         0.8 * std::sin(2 * kPi * 440.0 * i / 44100.0)));
      CHECK(worst <= f.lsb);
    }
  }
}

TEST_CASE("wav: WAVE_FORMAT_EXTENSIBLE with a PCM or float subformat") {
  for (int sub : {1, 3}) {
    WavSpec s;
    s.format = 0xFFFE;
    s.subFormat = sub;
    s.bits = sub == 3 ? 32 : 24;
    const WavAudio a = readWav(sp(makeWav(s, sineData(s.bits, sub == 3, 1, 500))));
    CHECK(a.isFloat == (sub == 3));
    CHECK(a.bits == s.bits);
    REQUIRE(a.mono.size() == 500);
    CHECK(a.mono[10] == Approx(0.8 * std::sin(2 * kPi * 440.0 * 10 / 44100.0)).margin(1e-4));
  }
}

TEST_CASE("wav: stereo is averaged to mono") {
  WavSpec s;
  s.channels = 2;
  std::vector<uint8_t> d;
  for (int i = 0; i < 4; ++i) {
    put(d, uint16_t(int16_t(16384)), 2);  // left  +0.5
    put(d, uint16_t(int16_t(-8192)), 2);  // right -0.25
  }
  const WavAudio a = readWav(sp(makeWav(s, d)));
  CHECK(a.channels == 2);
  REQUIRE(a.mono.size() == 4);
  for (float v : a.mono) CHECK(v == Approx(0.125f).margin(1e-6));
}

TEST_CASE("wav: chunk layout tolerance") {
  WavSpec s;
  s.listFirst = true;
  s.oddChunkAfter = true;
  const WavAudio a = readWav(sp(makeWav(s, sineData(16, false, 1, 300))));
  CHECK(a.mono.size() == 300);
  CHECK(a.rate == 44100);
}

TEST_CASE("wav: refusals carry the exact message") {
  const auto good = sineData(16, false, 1, 100);
  WavSpec s;

  CHECK(wavError({}) == "Not a WAV file.");
  CHECK(wavError({'R', 'I', 'F', 'F'}) == "Not a WAV file.");
  s.riff = "RIFX";
  CHECK(wavError(makeWav(s, good)) == "Not a WAV file.");
  auto notWave = makeWav({}, good);
  notWave[8] = 'A';
  CHECK(wavError(notWave) == "Not a WAV file.");

  const std::string unsupported =
      "Unsupported WAV format: only PCM (8, 16, 24 or 32-bit) and 32-bit float are supported.";
  s = {};
  s.format = 2;  // ADPCM
  CHECK(wavError(makeWav(s, good)) == unsupported);
  s = {};
  s.bits = 12;
  CHECK(wavError(makeWav(s, good)) == unsupported);
  s = {};
  s.format = 3;
  s.bits = 64;
  CHECK(wavError(makeWav(s, good)) == unsupported);
  s = {};
  s.format = 0xFFFE;
  s.subFormat = 6;  // A-law
  CHECK(wavError(makeWav(s, good)) == unsupported);

  s = {};
  s.channels = 6;
  CHECK(wavError(makeWav(s, good)) ==
        "Unsupported WAV file: only mono and stereo are supported.");

  s = {};
  CHECK(wavError(makeWav(s, {})) == "The WAV file has no audio.");

  s = {};
  s.overclaim = 1000;
  CHECK(wavError(makeWav(s, good)) == "The WAV file is damaged (the audio data is cut short).");
  s = {};
  s.omitFmt = true;
  CHECK(wavError(makeWav(s, good)) == "The WAV file is damaged (there is no format chunk).");
  s = {};
  s.omitData = true;
  CHECK(wavError(makeWav(s, good)) == "The WAV file is damaged (there is no audio data chunk).");
  s = {};
  s.rate = 0;
  CHECK(wavError(makeWav(s, good)) == "The WAV file is damaged (the sample rate is zero).");
}

TEST_CASE("wav: readWavFile reports a missing file as a WavError") {
  CHECK_THROWS_AS(readWavFile("no-such-folder/no-such.wav"), WavError);
}

TEST_CASE("import plan: sizes and addresses") {
  const WavAudio a = sineAudio(440, 44100, 5.0);
  ImportSettings s;
  s.rate = 20000;
  s.startSeconds = 0;
  s.endSeconds = 1.0;

  SECTION("one second at 20 kHz from page 0") {
    const ImportPlan p = planImport(a, s);
    CHECK(p.problem.empty());
    CHECK(p.address == 0);
    CHECK(p.bytes == 20000);
    CHECK(p.lastPage == 78);
    CHECK(p.seconds == Approx(1.0));
    CHECK(p.fractionOfHalf == Approx(20000.0 / 65536.0));
    CHECK_FALSE(p.truncated);
  }
  SECTION("four seconds at 30 kHz is cut off at the end of the half") {
    s.rate = 30000;
    s.endSeconds = 4.0;
    const ImportPlan p = planImport(a, s);
    CHECK(p.problem.empty());
    CHECK(p.bytes == 65536);
    CHECK(p.lastPage == 255);
    CHECK(p.truncated);
    CHECK(p.fractionOfHalf == Approx(1.0));
    CHECK(p.seconds == Approx(65536.0 / 30000.0));
  }
  SECTION("starting at page 250 leaves room for 1536 bytes") {
    s.startPage = 250;
    s.endSeconds = 2.0;
    const ImportPlan p = planImport(a, s);
    CHECK(p.bytes == 1536);
    CHECK(p.lastPage == 255);
    CHECK(p.truncated);
    CHECK(p.address == 250u * 256u);
  }
  SECTION("the upper half starts at $10000") {
    s.half = WaveHalf::Upper;
    s.startPage = 3;
    const ImportPlan p = planImport(a, s);
    CHECK(p.address == 0x10000u + 3u * 256u);
    CHECK(p.bytes == 20000);
    CHECK(p.lastPage == 3 + 78);
  }
  SECTION("an end point of 0 means the end of the file") {
    s.endSeconds = 0;
    s.startSeconds = 4.5;
    const ImportPlan p = planImport(a, s);
    CHECK(p.problem.empty());
    CHECK(p.bytes == 10000);
  }
  SECTION("an end point past the file is clamped to the file") {
    s.startSeconds = 4.0;
    s.endSeconds = 99.0;
    CHECK(planImport(a, s).bytes == 20000);
  }
  SECTION("the range limits themselves are accepted") {
    s.rate = kMinImportRate;
    CHECK(planImport(a, s).problem.empty());
    s.rate = kMaxImportRate;
    CHECK(planImport(a, s).problem.empty());
  }
}

TEST_CASE("import plan: refusals carry the exact message") {
  const WavAudio a = sineAudio(440, 44100, 2.0);
  ImportSettings s;
  s.rate = 20000;

  s.startSeconds = 1.0;
  s.endSeconds = 0.5;
  CHECK(planImport(a, s).problem == "Choose an end point after the start point.");
  s.endSeconds = 1.0;
  CHECK(planImport(a, s).problem == "Choose an end point after the start point.");

  s.startSeconds = 2.5;
  s.endSeconds = 0;
  CHECK(planImport(a, s).problem == "The start point is past the end of the file.");
  s.startSeconds = 2.0;
  CHECK(planImport(a, s).problem == "The start point is past the end of the file.");

  s.startSeconds = 0;
  s.endSeconds = 1.0;
  const std::string rateMsg = "The sample rate must be between " +
                              std::to_string(int(kMinImportRate)) + " and " +
                              std::to_string(int(kMaxImportRate)) + " Hz.";
  s.rate = 5000;
  CHECK(planImport(a, s).problem == rateMsg);
  s.rate = 50000;
  CHECK(planImport(a, s).problem == rateMsg);
  s.rate = std::nan("");
  CHECK(planImport(a, s).problem == rateMsg);

  s.rate = 20000;
  s.startPage = 256;
  CHECK(planImport(a, s).problem == "The start page must be between 0 and 255.");
  s.startPage = -1;
  CHECK(planImport(a, s).problem == "The start page must be between 0 and 255.");

  // A refused plan has nothing to write.
  const ImportPlan p = planImport(a, s);
  CHECK(p.bytes == 0);
  CHECK(convertForWaveRam(a, s).empty());
}

TEST_CASE("import convert: output is wave RAM safe") {
  ImportSettings s;
  s.rate = 20000;

  SECTION("the DOC-halting byte $00 never appears, even for a full-scale square wave") {
    WavAudio a;
    a.rate = 20000;
    a.channels = 1;
    for (int i = 0; i < 4000; ++i) a.mono.push_back((i / 10) % 2 ? 1.0f : -1.0f);
    for (bool norm : {true, false}) {
      s.normalize = norm;
      const auto out = convertForWaveRam(a, s);
      REQUIRE(out.size() == 4000);
      CHECK(std::count(out.begin(), out.end(), uint8_t(0)) == 0);
    }
    // Over-range input clips to the same limits.
    for (float& v : a.mono) v *= 3.0f;
    s.normalize = false;
    const auto out = convertForWaveRam(a, s);
    CHECK(*std::min_element(out.begin(), out.end()) == 1);
    CHECK(*std::max_element(out.begin(), out.end()) == 255);
  }
  SECTION("normalising puts the peak at $01 or $FF") {
    const WavAudio a = sineAudio(300, 20000, 0.5, 0.1);
    s.normalize = true;
    const auto out = convertForWaveRam(a, s);
    REQUIRE(out.size() == 10000);
    const int hi = *std::max_element(out.begin(), out.end());
    const int lo = *std::min_element(out.begin(), out.end());
    CHECK(std::max(hi - 128, 128 - lo) == 127);
    CHECK((hi == 255 || lo == 1));
    s.normalize = false;
    const auto quiet = convertForWaveRam(a, s);
    CHECK(*std::max_element(quiet.begin(), quiet.end()) < 160);
  }
  SECTION("a silent selection stays at $80 with normalising on") {
    WavAudio a;
    a.rate = 20000;
    a.channels = 1;
    a.mono.assign(5000, 0.0f);
    s.normalize = true;
    const auto out = convertForWaveRam(a, s);
    REQUIRE(out.size() == 5000);
    CHECK(std::all_of(out.begin(), out.end(), [](uint8_t b) { return b == 0x80; }));
  }
  SECTION("the size matches the plan") {
    const WavAudio a = sineAudio(440, 44100, 5.0);
    s.rate = 30000;
    s.endSeconds = 4.0;
    s.startPage = 10;
    s.half = WaveHalf::Upper;
    const ImportPlan p = planImport(a, s);
    CHECK(convertForWaveRam(a, s).size() == p.bytes);
    CHECK(p.truncated);
  }
}

TEST_CASE("import convert: resampling keeps pitch and removes what cannot be represented") {
  ImportSettings s;
  s.rate = 20000;
  s.normalize = true;

  SECTION("a 1 kHz sine from 44.1 kHz has its zero crossings 20 bytes apart") {
    const WavAudio a = sineAudio(1000, 44100, 0.5);
    const auto out = convertForWaveRam(a, s);
    REQUIRE(out.size() == 10000);
    const auto up = upCrossings(out);
    REQUIRE(up.size() > 100);
    for (size_t i = 1; i < up.size(); ++i)
      CHECK(std::abs(long(up[i]) - long(up[i - 1]) - 20) <= 1);
    // Over the whole run the spacing is exact to a fraction of a byte.
    CHECK(double(up.back() - up.front()) / double(up.size() - 1) == Approx(20.0).margin(0.02));
  }
  SECTION("a 5 kHz tone passes, a 15 kHz tone is more than 30 dB down") {
    s.normalize = false;
    const double inRms = 0.9 / std::sqrt(2.0);
    const auto passed = convertForWaveRam(sineAudio(5000, 44100, 0.5), s);
    const double passDb = 20 * std::log10(rmsOf(passed, 500, passed.size() - 500) / inRms);
    CHECK(passDb > -1.0);
    CHECK(passDb < 1.0);

    const auto stopped = convertForWaveRam(sineAudio(15000, 44100, 0.5), s);
    const double stopDb = 20 * std::log10(rmsOf(stopped, 500, stopped.size() - 500) / inRms);
    CHECK(stopDb < -30.0);
  }
  SECTION("upsampling from a lower rate keeps the pitch") {
    const auto out = convertForWaveRam(sineAudio(500, 8000, 1.0), s);
    REQUIRE(out.size() == 20000);
    const auto up = upCrossings(out);
    REQUIRE(up.size() > 100);
    CHECK(double(up.back() - up.front()) / double(up.size() - 1) == Approx(40.0).margin(0.05));
  }
  SECTION("the start and end points select the right part of the file") {
    // 1 kHz for the first half second, 2 kHz after; take 0.6 s to 0.8 s.
    WavAudio a = sineAudio(1000, 44100, 0.5);
    const WavAudio b = sineAudio(2000, 44100, 0.5);
    a.mono.insert(a.mono.end(), b.mono.begin(), b.mono.end());
    s.startSeconds = 0.6;
    s.endSeconds = 0.8;
    const auto out = convertForWaveRam(a, s);
    REQUIRE(out.size() == 4000);
    const size_t crossings = upCrossings(out).size();
    CHECK(crossings >= 398);  // 2 kHz for 0.2 s is 400 cycles
    CHECK(crossings <= 401);
  }
}

TEST_CASE("import pitch hint: the key that plays an import at its original pitch") {
  // 29,412 Hz is the measured reference (docs/app.md): MIDI 47, B2 with C4 = 60.
  CHECK(originalPitchKey(kPitchHintRate) == 47);
  CHECK(kPitchHintRate == kDefaultImportRate);
  // An octave in rate is an octave of keys.
  CHECK(originalPitchKey(2 * kPitchHintRate) == 59);
  CHECK(originalPitchKey(kPitchHintRate / 2) == 35);
  // The Mirage's own rate range (rounded to the nearest key).
  CHECK(originalPitchKey(kMaxImportRate) == 49);  // 47.3 + 2.14
  CHECK(originalPitchKey(kMinImportRate) == 29);  // 47.3 - 18.5
  // 22,050 Hz: 47.3 - 4.99 = 42.3.
  CHECK(originalPitchKey(22050.0) == 42);
}
