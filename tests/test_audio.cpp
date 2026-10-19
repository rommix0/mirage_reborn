// Audio output (Task 12): WAV writer, streaming resampler, Machine::render.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <numbers>
#include <vector>

#include "asset_paths.h"
#include "booted_machine.h"
#include "mirage/clocks.h"
#include "mirage/machine.h"
#include "mirage/stream_resampler.h"
#include "mirage/wav_writer.h"

using namespace mirage;
using mirage_test::bootedMachine;
using mirage_test::bootedSnapshot;

namespace {

uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
  return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 |
         uint32_t(b[at + 3]) << 24;
}
uint16_t le16(const std::vector<uint8_t>& b, size_t at) {
  return uint16_t(b[at] | b[at + 1] << 8);
}

std::vector<uint8_t> readFile(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

double rms(const std::vector<float>& v, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to; ++i) s += double(v[i]) * v[i];
  return std::sqrt(s / double(to - from));
}

// Frequency from the spacing of rising zero crossings in v[from..to).
double zeroCrossHz(const std::vector<float>& v, size_t from, size_t to, double rate) {
  size_t first = 0, last = 0, count = 0;
  for (size_t i = from; i + 1 < to; ++i)
    if (v[i] < 0 && v[i + 1] >= 0) {
      // Linear interpolation of the crossing for sub-sample accuracy.
      const double frac = double(-v[i]) / double(v[i + 1] - v[i]);
      const double at = double(i) + frac;
      if (count == 0) first = size_t(at * 1024);
      last = size_t(at * 1024);
      ++count;
    }
  if (count < 2) return 0;
  return rate * double(count - 1) / ((double(last) - double(first)) / 1024.0);
}

}  // namespace

TEST_CASE("wav: header sizes and rate, 0.5 maps to 16384") {
  const auto path = std::filesystem::temp_directory_path() / "mirage_test_audio.wav";
  const std::vector<float> samples = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f};
  writeWav(path, samples, 44100);
  const auto b = readFile(path);
  std::filesystem::remove(path);
  REQUIRE(b.size() == 44 + samples.size() * 2);
  CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
  CHECK(le32(b, 4) == b.size() - 8);
  CHECK(std::memcmp(b.data() + 8, "WAVE", 4) == 0);
  CHECK(std::memcmp(b.data() + 12, "fmt ", 4) == 0);
  CHECK(le32(b, 16) == 16);
  CHECK(le16(b, 20) == 1);  // PCM
  CHECK(le16(b, 22) == 1);  // mono
  CHECK(le32(b, 24) == 44100);
  CHECK(le32(b, 28) == 44100 * 2);  // byte rate
  CHECK(le16(b, 32) == 2);          // block align
  CHECK(le16(b, 34) == 16);         // bits
  CHECK(std::memcmp(b.data() + 36, "data", 4) == 0);
  CHECK(le32(b, 40) == samples.size() * 2);
  const auto s = [&](size_t i) { return int16_t(le16(b, 44 + 2 * i)); };
  CHECK(s(0) == 0);
  CHECK(s(1) == 16384);
  CHECK(s(2) == -16384);
  CHECK(s(3) == 32767);   // +1.0 clamps to the largest code
  CHECK(s(4) == -32768);
  CHECK(s(5) == 32767);   // out of range clamps
  CHECK(s(6) == -32768);
}

TEST_CASE("wav: an unwritable path throws") {
  const std::vector<float> one = {0.0f};
  CHECK_THROWS(writeWav(std::filesystem::temp_directory_path() / "no_such_dir_x" / "a.wav", one, 48000));
}

// The input rate is changed at run time (the DOC's scan rate follows its
// oscillator-enable count) without a click and without a pitch error.
TEST_CASE("resampler: input rate change keeps a 440 Hz sine continuous") {
  constexpr double kOut = 48000, kRateA = 29411.76, kRateB = 38461.54, kHz = 440;
  StreamResampler r(kOut);
  r.setInputRate(kRateA);
  std::vector<float> out;
  double phase = 0;
  const auto gen = [&](double rate, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      r.push(float(0.5 * std::sin(2 * std::numbers::pi * phase)));
      phase += kHz / rate;
      float y;
      while (r.pop(y)) out.push_back(y);
    }
  };
  gen(kRateA, 29412);  // 1 s
  // The resampler applies the new rate when its output position reaches the
  // first new-rate sample, about 32 input samples (53 outputs) after this
  // point; that is inside the checks below, which do not depend on the exact
  // index.
  const size_t switchAt = out.size();
  r.setInputRate(kRateB);
  gen(kRateB, 38462);  // 1 s

  REQUIRE(out.size() > 90000);
  // No click anywhere: a clean 440 Hz sine of amplitude 0.5 sampled at 48 kHz
  // moves by at most 2 pi 440 0.5 / 48000 = 0.0288 per sample. A jump in the
  // stream position at the rate change (even a few input samples) is several
  // times that.
  constexpr double kMaxSine = 2 * std::numbers::pi * kHz * 0.5 / kOut;
  float maxStep = 0;
  for (size_t i = 1001; i < out.size(); ++i) maxStep = std::max(maxStep, std::fabs(out[i] - out[i - 1]));
  CHECK(maxStep <= 1.5 * kMaxSine);
  CHECK(maxStep >= 0.9 * kMaxSine);  // and the sine really is there
  // Pitch on both sides, away from the start-up and the switch.
  CHECK(zeroCrossHz(out, 2000, switchAt - 400, kOut) == Catch::Approx(kHz).epsilon(0.01));
  CHECK(zeroCrossHz(out, switchAt + 400, out.size() - 400, kOut) ==
        Catch::Approx(kHz).epsilon(0.01));
  // Amplitude is preserved (a sine of 0.5 has RMS 0.3536).
  CHECK(rms(out, 2000, switchAt - 400) == Catch::Approx(0.5 / std::sqrt(2.0)).epsilon(0.01));
  CHECK(rms(out, switchAt + 400, out.size() - 400) ==
        Catch::Approx(0.5 / std::sqrt(2.0)).epsilon(0.01));
}

// Input faster than the output: the kernel is stretched, so a tone above the
// output Nyquist is removed instead of aliasing, and a tone below it is kept.
TEST_CASE("resampler: downsampling band-limits to the output Nyquist") {
  constexpr double kIn = 96000, kOut = 48000;
  const auto run = [&](double hz) {
    StreamResampler r(kOut);
    r.setInputRate(kIn);
    std::vector<float> out;
    for (size_t i = 0; i < 96000; ++i) {
      r.push(float(0.5 * std::sin(2 * std::numbers::pi * hz * double(i) / kIn)));
      float y;
      while (r.pop(y)) out.push_back(y);
    }
    return out;
  };
  const auto keep = run(1000);
  CHECK(rms(keep, 4000, keep.size() - 100) == Catch::Approx(0.5 / std::sqrt(2.0)).epsilon(0.01));
  const auto alias = run(40000);  // would fold to 8 kHz
  CHECK(rms(alias, 4000, alias.size() - 100) < 0.001);
}

TEST_CASE("resampler: save and load continue the stream bit for bit") {
  const auto feed = [](StreamResampler& r, std::vector<float>& out, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
      r.push(float(std::sin(0.01 * double(i)) + 0.3 * std::sin(0.37 * double(i))));
      float y;
      while (r.pop(y)) out.push_back(y);
    }
  };
  StreamResampler a(44100);
  a.setInputRate(29411.76);
  std::vector<float> sink;
  feed(a, sink, 0, 5000);
  a.setInputRate(38461.54);  // a rate change still waiting to be reached
  StateWriter w;
  a.save(w);
  const auto bytes = w.take();
  std::vector<float> contA, contB;
  feed(a, contA, 5000, 9000);
  StreamResampler b(8000);
  StateReader r(bytes);
  b.load(r);
  CHECK(r.atEnd());
  feed(b, contB, 5000, 9000);
  REQUIRE(contA.size() == contB.size());
  CHECK(contA == contB);
  // A truncated state leaves the resampler as it was.
  StreamResampler c(44100);
  const std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + 100);
  StateReader rc(cut);
  CHECK_THROWS_AS(c.load(rc), StateError);
  CHECK(c.outputRate() == 44100);
}

namespace {

constexpr uint64_t kSecond = kCpuClockHz;
constexpr double kHost = 48000;

// A ROM that loops on BRA *: nothing but the DOC (reset state) runs.
std::unique_ptr<Machine> idleMachine() {
  std::vector<uint8_t> rom(4096, 0x12);
  rom[0] = 0x20;
  rom[1] = 0xFE;
  rom[0xFFE] = 0xF0;
  rom[0xFFF] = 0x00;
  auto m = std::make_unique<Machine>(rom);
  m->powerOn();
  return m;
}

std::vector<float> renderSeconds(Machine& m, double seconds) {
  std::vector<float> v(size_t(seconds * kHost + 0.5));
  m.render(v.data(), v.size(), kHost);
  return v;
}

void append(std::vector<float>& to, const std::vector<float>& from) {
  to.insert(to.end(), from.begin(), from.end());
}

void note(Machine& m, uint8_t status, uint8_t key, uint8_t vel) {
  const uint8_t msg[] = {status, key, vel};
  m.queueMidi(msg);
}

// Period in samples of the strongest repeat of v[from..to) between lo and hi
// samples, by normalised autocorrelation (the first peak within 10% of the
// best, so a multiple of the period is not preferred), parabolic-refined.
double autocorrPeriod(const std::vector<float>& v, size_t from, size_t to, size_t lo, size_t hi) {
  std::vector<double> x(v.begin() + std::ptrdiff_t(from), v.begin() + std::ptrdiff_t(to));
  double mean = 0;
  for (double s : x) mean += s;
  mean /= double(x.size());
  for (double& s : x) s -= mean;
  std::vector<double> ac(hi + 2, 0.0);
  for (size_t lag = lo - 1; lag <= hi + 1; ++lag) {
    double num = 0, e1 = 0, e2 = 0;
    for (size_t i = 0; i + lag < x.size(); ++i) {
      num += x[i] * x[i + lag];
      e1 += x[i] * x[i];
      e2 += x[i + lag] * x[i + lag];
    }
    ac[lag] = num / std::sqrt(e1 * e2 + 1e-30);
  }
  double best = 0;
  for (size_t lag = lo; lag <= hi; ++lag) best = std::max(best, ac[lag]);
  for (size_t lag = lo; lag <= hi; ++lag) {
    if (ac[lag] >= 0.9 * best && ac[lag] >= ac[lag - 1] && ac[lag] >= ac[lag + 1]) {
      const double a = ac[lag - 1], b = ac[lag], c = ac[lag + 1];
      const double d = a - 2 * b + c;
      return double(lag) + (d != 0 ? 0.5 * (a - c) / d : 0.0);
    }
  }
  return 0;
}

}  // namespace

TEST_CASE("render: advances exactly frames / hostRate seconds, fractional cycles carried") {
  auto m = idleMachine();
  float s;
  // 44100 calls of one frame at 44100 Hz: 45.35 E cycles each. The run lands
  // within one instruction of a second, not 44100 roundings off.
  for (int i = 0; i < 44100; ++i) m->render(&s, 1, 44100);
  CHECK(m->cycles() >= kSecond);
  CHECK(m->cycles() < kSecond + 8);  // BRA is 3 cycles
  // One big call agrees.
  auto n = idleMachine();
  std::vector<float> v(44100);
  n->render(v.data(), v.size(), 44100);
  CHECK(n->cycles() >= kSecond);
  CHECK(n->cycles() < kSecond + 8);
}

TEST_CASE("render: silence from an idle machine, and the host rate can change") {
  auto m = idleMachine();
  std::vector<float> v(4800);
  m->render(v.data(), v.size(), 48000);
  m->render(v.data(), 2205, 22050);
  m->debugWrite(0xECE1, 0x3E);  // 16 oscillators: the scan rate changes under the stream
  m->render(v.data(), v.size(), 96000);
  CHECK(std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); }));
  CHECK(rms(v, 0, v.size()) < 1e-3);
}

// The OS auto-loads the first sound of SND-A01 into the lower half at boot.
// Sound 1's multisample: keys 44-56 play one wave (ptr $48, FC $01A6 at key 44
// to $034C at key 56, exactly one octave), key 60 a short transient (wave
// $A8). Measured with a register probe; see docs/hardware/boot-rom.md 13.5.
constexpr uint8_t kKeyLow = 44, kKeyHigh = 56, kKeyBrief = 60;

TEST_CASE("render: SND-A01 plays a note through the whole chain", "[assets]") {
  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  const double bootSeconds = bootedSnapshot("SND-A01.img").seconds;
  INFO("booted at " << bootSeconds << " s");
  REQUIRE(bootSeconds < 89.0);  // the condition was met, not the cap
  auto m = bootedMachine("SND-A01.img");

  SECTION("silence before the note") {
    const auto v = renderSeconds(*m, 1.0);
    CHECK(rms(v, 0, v.size()) < 1e-3);
  }

  SECTION("a held note sounds, then dies away after release") {
    std::vector<float> all = renderSeconds(*m, 1.0);  // silence
    note(*m, 0x90, kKeyLow, 100);
    const auto held = renderSeconds(*m, 1.0);
    note(*m, 0x80, kKeyLow, 0);
    const auto tail = renderSeconds(*m, 4.0);
    const double heldRms = rms(held, 0, held.size());
    const double tailRms = rms(tail, 0, tail.size());
    float peak = 0;
    for (float x : held) peak = std::max(peak, std::fabs(x));
    INFO("held rms " << heldRms << " tail rms " << tailRms << " peak " << peak);
    CHECK(heldRms > 0.01);
    CHECK(tailRms < 0.1 * heldRms);
    CHECK(peak > 0.05);
    CHECK(peak <= 1.0f);  // one voice does not clip
  }

  SECTION("key 60 is a short transient but audible") {
    note(*m, 0x90, kKeyBrief, 100);
    const auto v = renderSeconds(*m, 1.0);
    float peak = 0;
    for (float x : v) peak = std::max(peak, std::fabs(x));
    INFO("rms " << rms(v, 0, v.size()) << " peak " << peak);
    CHECK(rms(v, 0, v.size()) > 0.005);
    CHECK(peak > 0.05);
    CHECK(peak <= 1.0f);
  }

  SECTION("pitch: keys 44 and 56 (one octave) differ by a factor of 2") {
    const auto period = [&](uint8_t key) {
      auto n = bootedMachine("SND-A01.img");
      note(*n, 0x90, key, 100);
      const auto v = renderSeconds(*n, 0.5);
      // Steady part of the note: 50 ms to 280 ms after the key, lags for 70-700 Hz.
      return autocorrPeriod(v, size_t(0.05 * kHost), size_t(0.28 * kHost), size_t(kHost / 700),
                            size_t(kHost / 70));
    };
    const double pLow = period(kKeyLow), pHigh = period(kKeyHigh);
    INFO("period " << pLow << " vs " << pHigh << " samples");
    REQUIRE(pHigh > 0);
    CHECK(pLow / pHigh == Catch::Approx(2.0).epsilon(0.03));
  }

  SECTION("a snapshot taken mid-note continues bit for bit") {
    note(*m, 0x90, kKeyLow, 100);
    renderSeconds(*m, 0.1234);  // an odd length, so the carries are not zero
    const auto snap = m->saveState();
    const auto a = renderSeconds(*m, 0.3);
    m->loadState(snap);
    const auto b = renderSeconds(*m, 0.3);
    CHECK(rms(a, 0, a.size()) > 0.01);
    CHECK(a == b);
  }
}

TEST_CASE("render: the audio state is part of the saved state, atomically") {
  auto src = idleMachine();
  std::vector<float> v(1234);
  src->render(v.data(), v.size(), 44100);
  const auto snap = src->saveState();

  auto m = idleMachine();
  const auto before = m->saveState();
  REQUIRE(before != snap);  // the audio state differs (cycle count too)

  // A corrupt audio tag.
  const std::array<uint8_t, 4> tag = {'O', 'D', 'U', 'A'};  // fourcc("AUDO") as a LE u32
  const auto at = size_t(std::search(snap.begin(), snap.end(), tag.begin(), tag.end()) - snap.begin());
  REQUIRE(at < snap.size());
  auto badTag = snap;
  badTag[at] ^= 0xFF;
  CHECK_THROWS_AS(m->loadState(badTag), StateError);
  CHECK(m->saveState() == before);

  // A failure after the audio section has loaded (the panel, last in the
  // stream) puts the audio back too.
  auto badPanel = snap;
  badPanel[badPanel.size() - (4 + 4 + 2 + 2 * 8 * 8)] ^= 0xFF;
  CHECK_THROWS_AS(m->loadState(badPanel), StateError);
  CHECK(m->saveState() == before);

  CHECK_NOTHROW(m->loadState(snap));
  CHECK(m->saveState() == snap);
}

// SND-A04 is a strings sound that sustains while the key is held (SND-A01's
// sound 1 is percussive: every key ends by itself within 1.3 s, so it cannot
// show that note-off matters). Key 48 is 131.05 Hz, key 60 261.55 Hz.
TEST_CASE("render: SND-A04 strings sustain while held and stop after release", "[assets]") {
  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A04.img"));
  const double bootSeconds = bootedSnapshot("SND-A04.img").seconds;
  INFO("booted at " << bootSeconds << " s");
  REQUIRE(bootSeconds < 89.0);
  auto m = bootedMachine("SND-A04.img");

  SECTION("held key sounds throughout, then dies away after the release") {
    const auto silence = renderSeconds(*m, 0.5);
    CHECK(rms(silence, 0, silence.size()) < 1e-3);
    note(*m, 0x90, 48, 100);
    const auto held = renderSeconds(*m, 4.0);
    note(*m, 0x80, 48, 0);
    const auto tail = renderSeconds(*m, 4.0);
    const double heldRms = rms(held, 0, held.size());
    const double lastRms = rms(held, held.size() - size_t(0.25 * kHost), held.size());
    // The release itself takes up to a second; the 3 s after it must be quiet.
    const double tailRms = rms(tail, size_t(1.0 * kHost), tail.size());
    const double endRms = rms(tail, tail.size() - size_t(0.25 * kHost), tail.size());
    float peak = 0;
    for (float x : held) peak = std::max(peak, std::fabs(x));
    INFO("held rms " << heldRms << " last 0.25 s " << lastRms << " tail (1-4 s) " << tailRms
                     << " end " << endRms << " peak " << peak);
    CHECK(lastRms > 0.03);  // still sounding after 4 s: nothing ended it
    CHECK(tailRms < 0.1 * heldRms);
    CHECK(endRms < 1e-3);
    CHECK(peak <= 1.0f);
  }

  SECTION("pitch: keys 48 and 60 (one octave) differ by a factor of 2") {
    const auto period = [&](uint8_t key) {
      auto n = bootedMachine("SND-A04.img");
      note(*n, 0x90, key, 100);
      const auto v = renderSeconds(*n, 2.0);
      // 1 s to 2 s after the key; lags for 90-600 Hz.
      return autocorrPeriod(v, size_t(1.0 * kHost), size_t(2.0 * kHost), size_t(kHost / 600),
                            size_t(kHost / 90));
    };
    const double pLow = period(48), pHigh = period(60);
    INFO("period " << pLow << " vs " << pHigh << " samples");
    REQUIRE(pHigh > 0);
    CHECK(pLow / pHigh == Catch::Approx(2.0).epsilon(0.03));
    CHECK(kHost / pLow == Catch::Approx(131.05).epsilon(0.03));
    CHECK(kHost / pHigh == Catch::Approx(261.55).epsilon(0.03));
  }
}

TEST_CASE("render: a bad host rate writes silence, advances nothing and returns promptly") {
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (double rate : {nan, inf, -inf, 0.0, -1.0, 7999.0, 384001.0, 1e7}) {
    INFO("rate " << rate);
    auto m = idleMachine();
    m->runCycles(1000);
    const uint64_t before = m->cycles();
    const auto state = m->saveState();
    std::vector<float> v(512, 0.7f);
    const auto t0 = std::chrono::steady_clock::now();
    m->render(v.data(), v.size(), rate);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    CHECK(std::all_of(v.begin(), v.end(), [](float x) { return x == 0.0f; }));
    CHECK(m->cycles() == before);
    CHECK(m->saveState() == state);
  }
}

TEST_CASE("render: the supported host-rate limits 8000 and 384000 Hz work") {
  for (double rate : {8000.0, 384000.0}) {
    INFO("rate " << rate);
    auto m = idleMachine();
    std::vector<float> v(size_t(rate / 10));
    m->render(v.data(), v.size(), rate);
    CHECK(m->cycles() >= kSecond / 10);
    CHECK(m->cycles() < kSecond / 10 + 8);
    CHECK(std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); }));
  }
}

namespace {
// Unique per run so parallel ctest processes and leftovers never collide.
std::filesystem::path uniqueTemp(const std::string& stem) {
  static int n = 0;
  return std::filesystem::temp_directory_path() /
         (stem + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
          std::to_string(n++));
}
}  // namespace

TEST_CASE("wav: a non-ANSI file name writes, and failing there throws the module's own error") {
  const std::filesystem::path name(u8"\u041F\u0438\u0430\u043D\u043E\u4E2D.wav");
  const std::vector<float> one = {0.25f};
  const auto dir = uniqueTemp("mirage_wav_");
  std::filesystem::create_directories(dir);
  CHECK_NOTHROW(writeWav(dir / name, one, 48000));
  CHECK(std::filesystem::file_size(dir / name) == 46);
  std::filesystem::remove_all(dir);

  // A directory that does not exist: runtime_error (not std::system_error from
  // the ANSI conversion), and the message carries the UTF-8 name.
  bool ok = false;
  try {
    writeWav(dir / name, one, 48000);
  } catch (const std::system_error&) {
    FAIL("system_error escaped");
  } catch (const std::runtime_error& e) {
    ok = std::string(e.what()).find("\xD0\x9F\xD0\xB8\xD0\xB0\xD0\xBD\xD0\xBE\xE4\xB8\xAD.wav") !=
         std::string::npos;
  }
  CHECK(ok);
}
