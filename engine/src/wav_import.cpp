#include "mirage/wav_import.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace mirage {

namespace {

constexpr size_t kHalfBytes = 0x10000;
constexpr size_t kPageBytes = 256;

uint32_t le(const uint8_t* p, int bytes) {
  uint32_t v = 0;
  for (int i = 0; i < bytes; ++i) v |= uint32_t(p[i]) << (8 * i);
  return v;
}

bool tagIs(const uint8_t* p, const char (&t)[5]) { return std::memcmp(p, t, 4) == 0; }

[[noreturn]] void damaged(const char* what) {
  throw WavError(std::string("The WAV file is damaged (") + what + ").");
}

}  // namespace

WavAudio readWav(std::span<const uint8_t> file) {
  if (file.size() < 12 || !tagIs(file.data(), "RIFF") || !tagIs(file.data() + 8, "WAVE"))
    throw WavError("Not a WAV file.");

  bool haveFmt = false, haveData = false, cutShort = false;
  int formatTag = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  size_t dataAt = 0, dataSize = 0;

  size_t pos = 12;
  while (pos + 8 <= file.size()) {
    const uint8_t* h = file.data() + pos;
    const size_t size = le(h + 4, 4);
    const size_t body = pos + 8;
    const size_t avail = file.size() - body;
    if (tagIs(h, "fmt ") && !haveFmt) {
      if (size < 16 || avail < 16) damaged("the format chunk is too short");
      const uint8_t* f = file.data() + body;
      formatTag = int(le(f, 2));
      channels = int(le(f + 2, 2));
      rate = le(f + 4, 4);
      bits = int(le(f + 14, 2));
      if (formatTag == 0xFFFE) {
        // The real format is the first two bytes of the subformat GUID.
        if (size < 40 || avail < 40) damaged("the format chunk is too short");
        formatTag = int(le(f + 24, 2));
      }
      haveFmt = true;
    } else if (tagIs(h, "data") && !haveData) {
      haveData = true;
      dataAt = body;
      if (size > avail) {
        cutShort = true;
        break;
      }
      dataSize = size;
    }
    if (size > avail) break;  // a chunk running off the end is the last one we can see
    pos = body + size + (size & 1);
  }

  if (!haveFmt) damaged("there is no format chunk");
  const bool isFloat = formatTag == 3;
  if (!((formatTag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
        (isFloat && bits == 32)))
    throw WavError(
        "Unsupported WAV format: only PCM (8, 16, 24 or 32-bit) and 32-bit float are supported.");
  if (channels != 1 && channels != 2)
    throw WavError("Unsupported WAV file: only mono and stereo are supported.");
  if (rate == 0) damaged("the sample rate is zero");
  if (!haveData) damaged("there is no audio data chunk");
  if (cutShort) damaged("the audio data is cut short");

  const size_t sampleBytes = size_t(bits / 8);
  const size_t frameBytes = sampleBytes * size_t(channels);
  const size_t frames = dataSize / frameBytes;
  if (frames == 0) throw WavError("The WAV file has no audio.");

  auto sampleAt = [&](const uint8_t* p) -> double {
    if (isFloat) {
      const uint32_t u = le(p, 4);
      float f;
      std::memcpy(&f, &u, 4);
      return std::isfinite(f) ? double(f) : 0.0;
    }
    switch (bits) {
      case 8: return (double(p[0]) - 128.0) / 128.0;
      case 16: return double(int16_t(le(p, 2))) / 32768.0;
      case 24: return double(int32_t(le(p, 3) << 8) >> 8) / 8388608.0;
      default: return double(int32_t(le(p, 4))) / 2147483648.0;
    }
  };

  WavAudio a;
  a.rate = double(rate);
  a.channels = channels;
  a.bits = bits;
  a.isFloat = isFloat;
  a.mono.resize(frames);
  const uint8_t* p = file.data() + dataAt;
  for (size_t i = 0; i < frames; ++i, p += frameBytes) {
    double sum = 0;
    for (int c = 0; c < channels; ++c) sum += sampleAt(p + size_t(c) * sampleBytes);
    a.mono[i] = float(sum / channels);
  }
  return a;
}

WavAudio readWavFile(const std::filesystem::path& file) {
  std::ifstream f(file, std::ios::binary);
  if (!f) throw WavError("The WAV file could not be opened.");
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
  if (f.bad()) throw WavError("The WAV file could not be read.");
  return readWav({bytes.data(), bytes.size()});
}

int originalPitchKey(double rate) {
  return int(std::lround(kPitchHintKey + 12.0 * std::log2(rate / kPitchHintRate)));
}

ImportPlan planImport(const WavAudio& audio, const ImportSettings& s) {
  ImportPlan p;
  const double fileSeconds = audio.rate > 0 ? double(audio.mono.size()) / audio.rate : 0.0;
  const double start = std::max(s.startSeconds, 0.0);
  const bool toEnd = !(s.endSeconds > 0);
  if (std::isnan(s.startSeconds) || std::isnan(s.endSeconds) ||
      (!toEnd && s.endSeconds <= start)) {
    p.problem = "Choose an end point after the start point.";
    return p;
  }
  if (!(start < fileSeconds)) {
    p.problem = "The start point is past the end of the file.";
    return p;
  }
  if (!(s.rate >= kMinImportRate && s.rate <= kMaxImportRate)) {
    p.problem = "The sample rate must be between " + std::to_string(int(kMinImportRate)) +
                " and " + std::to_string(int(kMaxImportRate)) + " Hz.";
    return p;
  }
  if (s.startPage < 0 || s.startPage > 255) {
    p.problem = "The start page must be between 0 and 255.";
    return p;
  }

  const double end = toEnd ? fileSeconds : std::min(s.endSeconds, fileSeconds);
  const size_t room = kHalfBytes - size_t(s.startPage) * kPageBytes;
  size_t want = size_t(std::max<long long>(1, std::llround((end - start) * s.rate)));
  if (want > room) {
    want = room;
    p.truncated = true;
  }
  p.address = (s.half == WaveHalf::Upper ? uint32_t(kHalfBytes) : 0u) +
              uint32_t(size_t(s.startPage) * kPageBytes);
  p.bytes = want;
  p.lastPage = int((size_t(s.startPage) * kPageBytes + want - 1) / kPageBytes);
  p.seconds = double(want) / s.rate;
  p.fractionOfHalf = double(want) / double(kHalfBytes);
  return p;
}

namespace {

// Kaiser-windowed sinc, tabulated once per conversion so each tap is a lookup
// and a lerp. The kernel is a function of x = (distance in source samples) *
// 2 * (cutoff / source rate): sinc(x) * kaiser(x / kLobes), a zero crossing at
// every whole x.
constexpr int kLobes = 32;  // zero crossings per side (so at least 32 taps per side)
constexpr int kStepsPerLobe = 512;
constexpr double kKaiserBeta = 9.0;  // about -90 dB stopband

double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 60; ++k) {
    const double h = x / (2.0 * k);
    term *= h * h;
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

std::vector<double> kernelTable() {
  constexpr double kPi = 3.14159265358979323846;
  std::vector<double> t(size_t(kLobes) * kStepsPerLobe + 2, 0.0);  // zero past the end
  const double norm = besselI0(kKaiserBeta);
  for (size_t i = 0; i + 2 < t.size(); ++i) {
    const double x = double(i) / kStepsPerLobe;
    const double sinc = i == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
    const double r = x / kLobes;
    t[i] = sinc * besselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / norm;
  }
  return t;
}

}  // namespace

std::vector<uint8_t> convertForWaveRam(const WavAudio& audio, const ImportSettings& s) {
  const ImportPlan plan = planImport(audio, s);
  if (!plan.problem.empty()) return {};

  const std::vector<double> table = kernelTable();
  const double step = audio.rate / s.rate;  // source samples per output byte
  // Cutoff in cycles per source sample; the kernel stretches as it falls.
  const double scale = 2.0 * 0.45 * std::min(audio.rate, s.rate) / audio.rate;
  const double reach = kLobes / scale;  // kernel half-width in source samples
  const double origin = std::max(s.startSeconds, 0.0) * audio.rate;
  const long long count = (long long)audio.mono.size();

  std::vector<double> v(plan.bytes);
  for (size_t k = 0; k < v.size(); ++k) {
    const double pos = origin + double(k) * step;
    const long long first = (long long)std::ceil(pos - reach);
    const long long last = (long long)std::floor(pos + reach);
    double acc = 0, wsum = 0;
    for (long long j = first; j <= last; ++j) {
      const double f = std::abs(pos - double(j)) * scale * kStepsPerLobe;
      const size_t i = std::min(size_t(f), table.size() - 2);
      const double w = table[i] + (table[i + 1] - table[i]) * (f - double(i));
      wsum += w;  // dividing by the kernel's own sum keeps the DC gain at 1 for every phase
      if (j >= 0 && j < count) acc += w * double(audio.mono[size_t(j)]);
    }
    v[k] = wsum > 1e-9 ? acc / wsum : 0.0;
  }

  double gain = 1.0;
  if (s.normalize) {
    double peak = 0;
    for (double x : v) peak = std::max(peak, std::abs(x));
    if (peak > 1e-9) gain = 1.0 / peak;
  }

  std::vector<uint8_t> out(v.size());
  for (size_t k = 0; k < v.size(); ++k) {
    const double x = std::clamp(v[k] * gain, -1.0, 1.0);
    // $80 is zero and -1..+1 spans $01..$FF; the clamp keeps a zero byte (which halts the DOC) out.
    out[k] = uint8_t(std::clamp<long>(128 + std::lround(x * 127.0), 1, 255));
  }
  return out;
}

}  // namespace mirage
