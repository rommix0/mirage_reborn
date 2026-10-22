// Acceptance 2-7, headless. 2: edit a parameter from the panel, save with the
// OS, write the disk to a file and boot a fresh machine from it. 3: import a WAV
// into wave RAM, play it across the keyboard, save the sound with the OS and
// reopen the image. 4: other OS images boot and play. 5: format a blank disk with
// fmt2, boot it, save a sound to it and load it back. 6: .edm disks, raw and
// Giebler, boot and play. 7: the OS samples a sine from the input jack. The panel
// keys are the ones in docs/app.md (OS 3.2, musician's manual).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "asset_paths.h"
#include "booted_machine.h"
#include "mirage/disk_image.h"
#include "mirage/machine.h"
#include "mirage/wav_import.h"
#include "mirage/wav_writer.h"

using namespace mirage;
using namespace mirage_test;

namespace {
constexpr const char* kImage = "Factory Library/IMG/SND-A01.img";
constexpr int kFilterFc = 36;  // parameter 36, Filter Fc (00-99), saved with the sound

bool sameDisplay(DisplayState a, DisplayState b) { return a.left == b.left && a.right == b.right; }

// The two digits of a parameter value on the display (segment bits a..g, dp
// masked off), or -1 if either is not a digit.
int displayedValue(DisplayState d) {
  static constexpr uint8_t kDigit[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  int v[2] = {-1, -1};
  const uint8_t seg[2] = {uint8_t(d.left & 0x7F), uint8_t(d.right & 0x7F)};
  for (int i = 0; i < 2; ++i)
    for (int n = 0; n < 10; ++n)
      if (seg[i] == kDigit[n]) v[i] = n;
  return (v[0] < 0 || v[1] < 0) ? -1 : v[0] * 10 + v[1];
}
}  // namespace

TEST_CASE("acceptance 2: an edited parameter survives OS save, Save As and reopen",
          "[assets][acceptance]") {
  REQUIRE_ASSET(assetPath(kImage));
  REQUIRE_ASSET(romPath());

  auto m = bootedMachineAt(kImage);
  const DisplayState original = showParameter(*m, kFilterFc);

  // Three presses of Up raise the value by three.
  pressKeys(*m, {PanelKey::Up, PanelKey::Up, PanelKey::Up});
  const DisplayState edited = m->display();
  // Value mode lights the right decimal point; Up x3 gave original + 3.
  REQUIRE((original.right & 0x80) != 0);
  REQUIRE(displayedValue(original) >= 0);
  REQUIRE(displayedValue(edited) == displayedValue(original) + 3);
  REQUIRE_FALSE(sameDisplay(edited, original));

  osSaveLowerSound(*m);
  REQUIRE(m->disk() != nullptr);
  REQUIRE(m->disk()->dirty());

  // Save As: write the image to a new file.
  const auto path = std::filesystem::temp_directory_path() / "mirage_acceptance2.img";
  m->disk()->saveFile(path);
  REQUIRE(std::filesystem::file_size(path) == size_t(DiskImage::kImageBytes));

  // Reopen: a fresh machine boots from that file and shows the saved value.
  Machine fresh(bootRomBytes());
  fresh.insertDisk(DiskImage::loadFile(path));
  fresh.powerOn();
  settleBoot(fresh);
  const DisplayState reopened = showParameter(fresh, kFilterFc);
  std::filesystem::remove(path);
  CHECK(displayedValue(reopened) == displayedValue(edited));
  CHECK(reopened.left == edited.left);
  CHECK(reopened.right == edited.right);
  CHECK_FALSE(sameDisplay(reopened, original));
}

// docs/app.md tells the owner to boot a disk with File > Open then Machine > Reset:
// Open only swaps the disk under the running OS (spec 6.5), and Reset
// (Machine::powerOn) keeps the disk and its unsaved changes.
TEST_CASE("acceptance 2: Open leaves the running OS alone, Open then Reset boots the new disk",
          "[assets][acceptance]") {
  REQUIRE_ASSET(assetPath(kImage));
  REQUIRE_ASSET(romPath());
  constexpr uint8_t kTwo = 0xDB, kOne = 0x06;  // "2." "1": the settled display

  auto m = bootedMachineAt(kImage);
  const DisplayState original = showParameter(*m, kFilterFc);
  pressKeys(*m, {PanelKey::Up, PanelKey::Up, PanelKey::Up});
  const DisplayState edited = m->display();
  REQUIRE(displayedValue(edited) == displayedValue(original) + 3);
  osSaveLowerSound(*m);
  REQUIRE(m->disk()->dirty());
  const auto path = std::filesystem::temp_directory_path() / "mirage_acceptance2_reset.img";
  m->disk()->saveFile(path);

  // Reset alone: RAM is cleared, the OS boots from the disk in the drive, which still
  // holds its unsaved changes, so the edited value comes back.
  m->powerOn();
  settleBoot(*m);
  CHECK(m->display().left == kTwo);
  CHECK(m->display().right == kOne);
  CHECK(m->disk()->dirty());
  CHECK(displayedValue(showParameter(*m, kFilterFc)) == displayedValue(edited));

  // Open SND-A01.img as shipped, no Reset: the OS keeps running and reads nothing.
  m->ejectDisk();
  runSeconds(*m, 0.5);
  m->insertDisk(DiskImage::loadFile(assetPath(kImage)));
  const DisplayState shown = m->display();
  const uint64_t sectors = m->fdc().sectorsRead();
  runSeconds(*m, 10.0);
  CHECK(m->fdc().sectorsRead() == sectors);
  CHECK(sameDisplay(m->display(), shown));
  CHECK(displayedValue(showParameter(*m, kFilterFc)) == displayedValue(edited));

  // Then Reset: it boots the shipped disk and shows the shipped value.
  m->powerOn();
  settleBoot(*m);
  CHECK(m->display().left == kTwo);
  CHECK(m->display().right == kOne);
  CHECK(displayedValue(showParameter(*m, kFilterFc)) == displayedValue(original));

  // Open the saved copy, then Reset: the edit comes back from the file.
  m->ejectDisk();
  runSeconds(*m, 0.5);
  m->insertDisk(DiskImage::loadFile(path));
  std::filesystem::remove(path);
  m->powerOn();
  settleBoot(*m);
  CHECK(m->display().left == kTwo);
  CHECK(m->display().right == kOne);
  CHECK(displayedValue(showParameter(*m, kFilterFc)) == displayedValue(edited));
}

// ---- Acceptance 5: format a blank disk with the fmt2 boot disk --------------

namespace {
constexpr const char* kFmt2 = "EnsoniqOS/fmt2";
constexpr const char* kSndA01 = "Factory Library/IMG/SND-A01.img";
// Display segments (the display's lower-case c is "Fc"): "Fd" waits for a
// blank disk, "Fc" is the finished format (docs/app.md, acceptance 5).
constexpr uint8_t kF = 0x71, kSegD = 0x5E, kSegC = 0x58;

struct Formatted {
  std::vector<uint8_t> bytes;  // the image fmt2 made
  double seconds = 0;          // from Enter to "Fc" on the emulated clock
};

// Boots fmt2, swaps in a blank disk, presses K1 and Enter and runs until "Fc"
// (at most 180 s). Done once per test run: the second acceptance test reuses
// the disk. The REQUIREs fail whichever test runs it first.
const Formatted& fmt2Formatted() {
  static Formatted result;
  if (!result.bytes.empty()) return result;
  auto m = bootedMachineAt(kFmt2);
  REQUIRE(m->display().left == kF);
  REQUIRE(m->display().right == kSegD);

  m->ejectDisk();
  runSeconds(*m, 0.5);
  m->insertDisk(DiskImage::blank());
  pressKeys(*m, {PanelKey::K1, PanelKey::Enter});

  const uint64_t tick = kCpuClockHz / 100;
  const uint64_t start = m->cycles();
  bool done = false;
  for (uint64_t t = 0; t < 18000 && !done; ++t) {
    m->runCycles(tick);
    done = m->display().left == kF && m->display().right == kSegC;
  }
  const double seconds = double(m->cycles() - start) / double(kCpuClockHz);
  INFO("formatting took " << seconds << " s of emulated time after Enter");
  REQUIRE(done);
  REQUIRE(m->disk() != nullptr);
  REQUIRE(m->disk()->formattedTracks() == DiskImage::kTracks);
  result.bytes = m->disk()->toBytes();
  result.seconds = seconds;
  WARN("fmt2 formatted the blank disk in " << seconds << " s of emulated time after Enter");
  return result;
}
}  // namespace

TEST_CASE("acceptance 5: fmt2 formats a blank disk that then boots", "[assets][acceptance]") {
  REQUIRE_ASSET(assetPath(kFmt2));
  REQUIRE_ASSET(romPath());

  const Formatted& formatted = fmt2Formatted();
  REQUIRE(formatted.bytes.size() == size_t(DiskImage::kImageBytes));

  // fmt2 writes OS 3.2 onto the disk (EnsoniqOS/About the disk Images.rtf):
  // a fresh machine boots from it to the usual "DB 06".
  Machine fresh(bootRomBytes());
  fresh.insertDisk(DiskImage::fromBytes(formatted.bytes));
  fresh.powerOn();
  const double bootSeconds = settleBoot(fresh);
  INFO("the formatted disk settled after " << bootSeconds << " s");
  REQUIRE(bootSeconds < 89.0);
  CHECK(fresh.display().left == 0xDB);
  CHECK(fresh.display().right == 0x06);
}

TEST_CASE("acceptance 5: a sound saved to the formatted disk loads back", "[assets][acceptance]") {
  REQUIRE_ASSET(assetPath(kFmt2));
  REQUIRE_ASSET(assetPath(kSndA01));
  REQUIRE_ASSET(romPath());

  const Formatted& formatted = fmt2Formatted();

  // Boot SND-A01, swap the formatted disk in and save the (edited) lower
  // sound to it: Param 1 1, 1, Enter always saves to place 1.
  auto m = bootedMachineAt(kSndA01);
  const DisplayState original = showParameter(*m, kFilterFc);
  pressKeys(*m, {PanelKey::Up, PanelKey::Up, PanelKey::Up});
  const DisplayState edited = m->display();
  REQUIRE(displayedValue(edited) == displayedValue(original) + 3);

  m->ejectDisk();
  runSeconds(*m, 0.5);
  m->insertDisk(DiskImage::fromBytes(formatted.bytes));
  runSeconds(*m, 1.0);
  osSaveLowerSound(*m);
  REQUIRE(m->disk() != nullptr);
  REQUIRE(m->disk()->dirty());

  const auto path = std::filesystem::temp_directory_path() / "mirage_acceptance5.img";
  m->disk()->saveFile(path);
  REQUIRE(std::filesystem::file_size(path) == size_t(DiskImage::kImageBytes));

  // Reopen: a fresh machine boots SND-A01, takes the saved disk and loads
  // sound 1 into the lower half from it.
  auto fresh = bootedMachineAt(kSndA01);
  fresh->ejectDisk();
  runSeconds(*fresh, 0.5);
  fresh->insertDisk(DiskImage::loadFile(path));
  std::filesystem::remove(path);
  runSeconds(*fresh, 1.0);
  const uint64_t sectorsBefore = fresh->fdc().sectorsRead();
  osLoadLowerSound(*fresh, 1);
  CHECK(fresh->fdc().sectorsRead() > sectorsBefore);  // it really read the disk

  // The edited value came back from the disk, not from the SND-A01 boot.
  const DisplayState loaded = showParameter(*fresh, kFilterFc);
  CHECK(displayedValue(loaded) == displayedValue(edited));
  CHECK_FALSE(sameDisplay(loaded, original));

  // And the sound plays.
  std::vector<float> v(48000);
  const uint8_t on[] = {0x90, 44, 100};
  fresh->queueMidi(on);
  fresh->render(v.data(), v.size(), 48000);
  float peak = 0;
  for (float x : v) peak = std::max(peak, std::fabs(x));
  INFO("peak " << peak);
  CHECK(peak > 0.05f);
}


// ---- Acceptance 3: import a WAV, play it, save it with the OS, reopen -------

namespace {
constexpr double kSineHz = 440.0;
constexpr int kPlayRate = 48000;

// A 2 s 440 Hz sine as a 16-bit WAV file under the temp folder (long enough that no
// key reads past its end during the 125 ms measured).
std::filesystem::path writeSineWav(const char* name) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::vector<float> sine(88200);
  for (size_t i = 0; i < sine.size(); ++i)
    sine[i] = float(0.8 * std::sin(2.0 * 3.14159265358979 * kSineHz * double(i) / 44100.0));
  writeWav(path, sine, 44100);
  return path;
}

// What the import dialog does: read, plan, convert, write into wave RAM.
ImportPlan importWav(Machine& m, const std::filesystem::path& wav, double rate) {
  const WavAudio audio = readWavFile(wav);
  ImportSettings settings;  // lower half, page 0, normalised
  settings.rate = rate;
  const ImportPlan plan = planImport(audio, settings);
  REQUIRE(plan.problem.empty());
  const std::vector<uint8_t> bytes = convertForWaveRam(audio, settings);
  REQUIRE(bytes.size() == plan.bytes);
  REQUIRE_FALSE(m.driveActive());
  m.writeWaveRam(plan.address, bytes);
  return plan;
}

struct Played {
  float peak = 0;
  double hz = 0;
};

// Note-on at velocity 100; the peak over 0.25 s (from frame peakFrom on) and the
// fundamental (zero crossings of the mean-removed signal from 31 ms to 125 ms, after
// the attack).
Played playNote(Machine& m, int note, size_t peakFrom = 0) {
  const uint8_t on[] = {0x90, uint8_t(note), 100};
  m.queueMidi(on);
  std::vector<float> v(kPlayRate / 4);
  m.render(v.data(), v.size(), kPlayRate);
  Played p;
  for (size_t i = peakFrom; i < v.size(); ++i) p.peak = std::max(p.peak, std::fabs(v[i]));
  const size_t from = 1500, to = 6000;
  double mean = 0;
  for (size_t i = from; i < to; ++i) mean += v[i];
  mean /= double(to - from);
  size_t crossings = 0;
  double first = 0, last = 0;
  for (size_t i = from + 1; i < to; ++i) {
    const double a = v[i - 1] - mean, b = v[i] - mean;
    if (a < 0 && b >= 0) {
      last = double(i - 1) + (-a) / (b - a);
      if (crossings++ == 0) first = last;
    }
  }
  if (crossings >= 3) p.hz = double(crossings - 1) * kPlayRate / (last - first);
  return p;
}

// Keys 41-56 of SND-A01's lower sound play one wavesample from page $48 on, tuned a
// semitone apart (docs/hardware/boot-rom.md 13.5). For each, the key at which the
// imported sine would play at 440 Hz: key - 12 log2(f / 440). Returns their mean and
// the spread (max - min).
struct OriginalPitch {
  double key = 0, spread = 0;
};
OriginalPitch measureOriginalPitchKey(const std::filesystem::path& wav, double rate) {
  double sum = 0, lo = 1e9, hi = -1e9;
  int n = 0;
  for (int note = 41; note <= 56; ++note) {
    auto m = bootedMachineAt(kSndA01);
    importWav(*m, wav, rate);
    const Played p = playNote(*m, note);
    REQUIRE(p.peak > 0.05f);
    REQUIRE(p.hz > 0);
    const double k = note - 12.0 * std::log2(p.hz / kSineHz);
    sum += k;
    lo = std::min(lo, k);
    hi = std::max(hi, k);
    ++n;
  }
  return {sum / n, hi - lo};
}
}  // namespace

TEST_CASE("acceptance 3: an imported sine plays at a pitch set by the key",
          "[assets][acceptance]") {
  REQUIRE_ASSET(assetPath(kSndA01));
  REQUIRE_ASSET(romPath());
  const auto wav = writeSineWav("mirage_acceptance3.wav");

  // The OS 3.2 lower sound lives in the lower half: Load Lower rewrites
  // $00000-$0FFFF and leaves $10000-$1FFFF alone (measured, docs/app.md), and the
  // lower keys 36-71 play wave tables below $10000.
  auto probe = bootedMachineAt(kSndA01);
  const ImportPlan plan = importWav(*probe, wav, kDefaultImportRate);
  CHECK(plan.address == 0);
  CHECK(plan.bytes == 58824);  // 2 s at 29,412 Hz

  // Two notes an octave apart on the one wavesample (keys 41-56 play from page $48):
  // the second plays twice as fast.
  auto low = bootedMachineAt(kSndA01);
  importWav(*low, wav, kDefaultImportRate);
  auto high = bootedMachineAt(kSndA01);
  importWav(*high, wav, kDefaultImportRate);
  const Played a = playNote(*low, 44), b = playNote(*high, 56);
  INFO("key 44: " << a.hz << " Hz, peak " << a.peak << "; key 56: " << b.hz << " Hz, peak "
                  << b.peak);
  CHECK(a.peak > 0.05f);
  CHECK(b.peak > 0.05f);
  REQUIRE(a.hz > 0);
  CHECK(b.hz / a.hz == Catch::Approx(2.0).epsilon(0.03));

  // The pitch hint (docs/app.md): the key that plays the import at its original pitch.
  const OriginalPitch atDefault = measureOriginalPitchKey(wav, kDefaultImportRate);
  WARN("original-pitch key at " << kDefaultImportRate << " Hz: " << atDefault.key
       << " (spread " << atDefault.spread << " semitones)");
  CHECK(atDefault.spread < 0.3);
  CHECK(atDefault.key == Catch::Approx(kPitchHintKey).margin(0.2));
  const OriginalPitch at22k = measureOriginalPitchKey(wav, 22050.0);
  WARN("original-pitch key at 22050 Hz: " << at22k.key);
  CHECK(at22k.key ==
        Catch::Approx(kPitchHintKey + 12.0 * std::log2(22050.0 / kPitchHintRate)).margin(0.3));
  CHECK(originalPitchKey(22050.0) == int(std::lround(at22k.key)));
  std::filesystem::remove(wav);
}

TEST_CASE("acceptance 3: the imported sound survives an OS save and reopen",
          "[assets][acceptance]") {
  REQUIRE_ASSET(assetPath(kSndA01));
  REQUIRE_ASSET(romPath());
  const auto wav = writeSineWav("mirage_acceptance3b.wav");

  auto m = bootedMachineAt(kSndA01);
  importWav(*m, wav, kDefaultImportRate);
  std::filesystem::remove(wav);
  const Played before = playNote(*m, 48);
  INFO("before saving: " << before.hz << " Hz, peak " << before.peak);
  REQUIRE(before.peak > 0.05f);
  REQUIRE(before.hz > 0);
  const uint8_t off[] = {0x80, 48, 0};
  m->queueMidi(off);
  runSeconds(*m, 1.0);

  // Save the lower sound to place 1 (the imported wave RAM goes with it), write the
  // image to a temp file and boot a fresh machine from that file. The image is a
  // copy: the user's SND-A01.img is only read.
  osSaveLowerSound(*m);
  REQUIRE(m->disk() != nullptr);
  REQUIRE(m->disk()->dirty());
  const auto path = std::filesystem::temp_directory_path() / "mirage_acceptance3.img";
  m->disk()->saveFile(path);
  Machine fresh(bootRomBytes());
  fresh.insertDisk(DiskImage::loadFile(path));
  std::filesystem::remove(path);
  fresh.powerOn();
  settleBoot(fresh);

  const Played after = playNote(fresh, 48);
  INFO("after reopening: " << after.hz << " Hz, peak " << after.peak);
  CHECK(after.peak == Catch::Approx(before.peak).epsilon(0.10));
  CHECK(after.hz == Catch::Approx(before.hz).epsilon(0.03));
}


// ---- Acceptance 4: each OS image boots and plays ----------------------------

namespace {
// Boots the image to settled and holds note 60 at velocity 100 for 1 s on MIDI
// channel 1. Every OS here loads or holds a sound at boot and listens on channel
// 1 (Soundprocess: program 01 plays as soon as the display shows "01", manual
// section 1; boot-rom.md 13.7), so no panel action is needed.
void bootsAndPlays(const char* image) {
  REQUIRE_ASSET(assetPath(image));
  REQUIRE_ASSET(romPath());
  const Booted& b = bootedSnapshotAt(image);
  auto m = bootedMachineAt(image);
  const DisplayState d = m->display();
  INFO(image << ": settled at " << b.seconds << " s, display " << int(d.left) << " "
             << int(d.right) << ", " << m->fdc().sectorsRead() << " sectors");
  REQUIRE(b.seconds < 90.0);
  const uint8_t on[] = {0x90, 60, 100}, off[] = {0x80, 60, 0};
  m->queueMidi(on);
  std::vector<float> v(48000);
  m->render(v.data(), v.size(), 48000);
  m->queueMidi(off);
  float peak = 0;
  for (float x : v) peak = std::max(peak, std::fabs(x));
  INFO("peak " << peak);
  CHECK(peak > 0.05f);
  CHECK(m->illegalOps() == 0);
}
}  // namespace

TEST_CASE("acceptance 4: each OS image boots and plays", "[assets][acceptance]") {
  SECTION("os32") { bootsAndPlays("EnsoniqOS/os32"); }
  SECTION("masos") { bootsAndPlays("EnsoniqOS/masos"); }
  SECTION("pianos") { bootsAndPlays("EnsoniqOS/pianos"); }
  SECTION("rhodes") { bootsAndPlays("EnsoniqOS/rhodes"); }
  SECTION("resmos32.s") { bootsAndPlays("EnsoniqOS/resmos32.s"); }
  SECTION("Soundprocess OS") { bootsAndPlays("Soundprocess/Soundprocess OS/Soundprocess OS.img"); }
}

// ---- Acceptance 6: .edm disks, raw and Giebler -------------------------------

TEST_CASE("acceptance 6: a Metascape .edm boots and plays", "[assets][acceptance]") {
  // A 450,560-byte .edm is a raw image; acceptance 1's conditions: the display
  // settles on "2." "1" (DB 06) and a note plays.
  constexpr const char* kMetascape = "MetascapeMetaSounds/mp1.edm";
  bootsAndPlays(kMetascape);
  const auto m = bootedMachineAt(kMetascape);
  CHECK(m->display().left == 0xDB);
  CHECK(m->display().right == 0x06);
}

TEST_CASE("acceptance 6: a Giebler .EDM boots its OS", "[assets][acceptance]") {
  // SNDPROCS.EDM holds the same disk as Soundprocess OS.img (fdc.md, "Disk file
  // formats"), so it boots and settles exactly like it, and plays.
  constexpr const char* kGiebler = "Soundprocess/TritonSoundprocess/SNDPROCS.EDM";
  constexpr const char* kImg = "Soundprocess/Soundprocess OS/Soundprocess OS.img";
  REQUIRE_ASSET(assetPath(kImg));
  bootsAndPlays(kGiebler);
  const auto edm = bootedMachineAt(kGiebler), img = bootedMachineAt(kImg);
  CHECK(bootedSnapshotAt(kGiebler).seconds == bootedSnapshotAt(kImg).seconds);
  CHECK(sameDisplay(edm->display(), img->display()));
  CHECK(edm->fdc().sectorsRead() == img->fdc().sectorsRead());
}

// ---- Acceptance 7: sampling from the audio input jack -------------------------

namespace {
// Removes the file when it goes out of scope, so a failed REQUIRE leaks nothing.
struct RemoveOnExit {
  std::filesystem::path path;
  ~RemoveOnExit() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

// A sine at the jack, fed in 512-frame render blocks so emulated time advances
// with the input (Machine::render, not runCycles). Phase continues across calls.
struct Jack {
  double hz = 440.0;
  float amp = 0.5f;
  uint64_t n = 0;
  std::vector<float> buf = std::vector<float>(512);

  // Renders `seconds` (whole blocks of 512 frames, at least one) with the sine at the jack.
  void run(Machine& m, double seconds) {
    const size_t blocks = std::max<size_t>(1, size_t(seconds * kPlayRate / 512.0 + 0.5));
    for (size_t b = 0; b < blocks; ++b) {
      for (float& x : buf)
        x = float(amp * std::sin(2.0 * 3.14159265358979 * hz * double(n++) / kPlayRate));
      m.render(buf.data(), buf.data(), buf.size(), kPlayRate);
    }
  }
  void press(Machine& m, PanelKey k, double holdS = 0.15, double gapS = 0.25) {
    m.setKey(k, true);
    run(m, holdS);
    m.setKey(k, false);
    run(m, gapS);
  }
};

std::vector<uint8_t> lowerHalf(const Machine& m) {
  std::vector<uint8_t> v(0x10000);
  for (uint32_t i = 0; i < v.size(); ++i) v[i] = m.sampleRam(i);
  return v;
}

// The OS's display codes (segment bits) at the end of a sampling run ("SF"-style
// 6D 71, filters.md 3.3) and when no sound crossed the threshold ("nS", 54 6D).
constexpr uint8_t kSegDoneL = 0x6D, kSegDoneR = 0x71, kSegNsL = 0x54, kSegNsR = 0x6D;
constexpr uint32_t kSampled = 0x0FFF0;  // sampling writes $00000-$0FFEF, then 16 zeros
constexpr double kSampleTimeUs = 34.0;  // parameter 73's default; the sampling period
constexpr double kSampleRate = 1.0e6 / kSampleTimeUs;

// What the keys do after the jack is plugged in: Sample Lower, wait for level-detect
// (2.55 s after the press, filters.md 3.3), then Enter. Returns at once after Enter;
// the caller keeps feeding the jack until the run is over.
void startSampleLower(Machine& m, Jack& j) {
  j.run(m, 0.1);
  j.press(m, PanelKey::SampleLower, 0.15, 2.5);
  j.press(m, PanelKey::Enter, 0.15, 0.0);
}

// Everything a sampling run left behind.
struct SampledSine {
  bool done = false;          // the display reached the end-of-run code
  double secondsAfterEnter = 0;
  std::vector<uint8_t> state;  // the machine afterwards, unplugged
  std::vector<uint8_t> lower;  // wave RAM $00000-$0FFFF
};

// Boots SND-A01, optionally selects line input (parameter 75 on), plugs a sine of
// frequency 440 Hz and amplitude `amp` into the jack and samples the lower half with
// the OS. Cached per (line, amp): a run is about 6 emulated seconds.
const SampledSine& sampledSine(bool line, float amp) {
  static std::map<std::pair<bool, float>, SampledSine> cache;
  const auto key = std::make_pair(line, amp);
  if (const auto it = cache.find(key); it != cache.end()) return it->second;

  auto m = bootedMachineAt(kSndA01);
  if (line) {  // Param 7 5 Value, Up: line (the value 1)
    showParameter(*m, 75);
    pressKeys(*m, {PanelKey::Up});
  }
  Jack j;
  j.amp = amp;
  startSampleLower(*m, j);
  SampledSine out;
  for (size_t t = 0; t < 600 && !out.done; ++t) {  // up to 6.4 s
    j.run(*m, 0.01);
    const DisplayState d = m->display();
    out.done = d.left == kSegDoneL && d.right == kSegDoneR;
  }
  out.secondsAfterEnter = double(j.n) / kPlayRate;
  // Unplug: sampling is over, and runCycles would hold the jack's last value. Then render
  // half a second with nothing plugged in. The unplug leaves the output stage mid-sine:
  // filter 7 rings for a few ms, and the host-side DC blocker (5 Hz, 32 ms time constant)
  // holds the state of the sampling mix, which runCycles() does not update, so a note
  // started at once opens with a decaying burst (peak 0.10 against 0.064; measured to be
  // in the DC-blocked output only, the raw mix of the note is the same). Half a second
  // of render settles both; the sampled data and the OS are not touched by it.
  std::vector<float> idle(kPlayRate / 2);
  m->render(idle.data(), idle.size(), kPlayRate);
  out.state = m->saveState();
  out.lower = lowerHalf(*m);
  return cache[key] = std::move(out);
}

std::unique_ptr<Machine> machineFrom(const std::vector<uint8_t>& state) {
  auto m = std::make_unique<Machine>(bootRomBytes());
  m->loadState(state);
  return m;
}

struct RamStats {
  int lo = 255, hi = 0;     // smallest and largest byte over the sampled range
  int longestClip = 0;      // the longest run of $01 or $FF
  size_t crossings = 0;     // rising crossings of $80
  double period = 0;        // mean distance between them, in bytes
};
RamStats analyse(const std::vector<uint8_t>& ram) {
  RamStats s;
  int run = 0;
  double first = 0, last = 0;
  for (uint32_t i = 0; i < kSampled; ++i) {
    s.lo = std::min<int>(s.lo, ram[i]);
    s.hi = std::max<int>(s.hi, ram[i]);
    run = (ram[i] == 0x01 || ram[i] == 0xFF) ? run + 1 : 0;
    s.longestClip = std::max(s.longestClip, run);
    if (i > 0 && ram[i - 1] < 0x80 && ram[i] >= 0x80) {
      if (s.crossings++ == 0) first = i;
      last = i;
    }
  }
  if (s.crossings >= 2) s.period = (last - first) / double(s.crossings - 1);
  return s;
}
}  // namespace

// The sine's level: mic (the default) has 11 times the gain of line and a compressor,
// and a full-scale sine clips it, so the main test uses a small one. 0.08 at the jack
// swings the sampled bytes about 70 either side of $80 (measured).
constexpr float kMicAmp = 0.08f;

TEST_CASE("acceptance 7: OS 3.2 samples a sine from the input jack", "[assets][acceptance]") {
  REQUIRE_ASSET(assetPath(kSndA01));
  REQUIRE_ASSET(romPath());

  // The sample time is parameter 73, 34 us by default.
  {
    auto m = bootedMachineAt(kSndA01);
    REQUIRE(displayedValue(showParameter(*m, 73)) == int(kSampleTimeUs));
  }

  const SampledSine& s = sampledSine(false, kMicAmp);
  REQUIRE(s.done);
  INFO("sampling ended " << s.secondsAfterEnter << " s of rendering after the first key");

  // The lower half holds a sine of the right pitch: rate / 440 bytes per cycle.
  const RamStats st = analyse(s.lower);
  INFO("bytes " << st.lo << ".." << st.hi << ", longest clip run " << st.longestClip
                << ", crossings " << st.crossings << ", period " << st.period);
  CHECK(st.crossings > 900);
  CHECK(st.period == Catch::Approx(kSampleRate / kSineHz).epsilon(0.02));
  CHECK(st.hi - st.lo > 100);  // a real signal, not a trickle
  CHECK(st.longestClip <= 3);  // within the 8-bit range, not clipped
  for (uint32_t i = kSampled; i < 0x10000; ++i) CHECK(s.lower[i] == 0);

  // It plays: a note on the lower keyboard has a level, and an octave up is twice the
  // pitch. Keys 46-66 of the sampled sound run a semitone apart on one wavesample (the
  // OS splits the keyboard into wavesamples; keys 44-45 and 66-67 straddle two), and key
  // 52 plays the 440 Hz sine back at 440 Hz (measured: 441.5 Hz).
  auto low = machineFrom(s.state);
  auto high = machineFrom(s.state);
  const Played a = playNote(*low, 52), b = playNote(*high, 64);
  INFO("key 52: " << a.hz << " Hz, peak " << a.peak << "; key 64: " << b.hz << " Hz, peak "
                  << b.peak);
  CHECK(a.peak > 0.05f);
  CHECK(b.peak > 0.05f);
  REQUIRE(a.hz > 0);
  REQUIRE(b.hz > 0);
  CHECK(b.hz / a.hz == Catch::Approx(2.0).epsilon(0.03));
  CHECK(a.hz == Catch::Approx(kSineHz).epsilon(0.03));
}

// Mic (parameter 75 off, the default) has 11 times the gain of line (on) and a
// compressor. The same full-scale sine gives the larger swing in wave RAM through the
// mic; through line it is still a clean sine at the right pitch.
TEST_CASE("acceptance 7: the mic input is hotter than the line input", "[assets][acceptance]") {
  REQUIRE_ASSET(assetPath(kSndA01));
  REQUIRE_ASSET(romPath());
  // Line is 11 times quieter, and filter 7 passes about 0.54 of it: 1.0 crosses the OS's
  // default threshold, 0.5 does not (the OS gives up with "nS").
  const SampledSine& mic = sampledSine(false, 1.0f);
  const SampledSine& line = sampledSine(true, 1.0f);
  REQUIRE(mic.done);
  REQUIRE(line.done);
  const RamStats m = analyse(mic.lower), l = analyse(line.lower);
  INFO("mic bytes " << m.lo << ".." << m.hi << " (clip run " << m.longestClip << "); line bytes "
                    << l.lo << ".." << l.hi << " (clip run " << l.longestClip << ")");
  CHECK((m.hi - m.lo) > (l.hi - l.lo));
  CHECK((l.hi - l.lo) > 100);
  CHECK(l.longestClip <= 3);
  CHECK(l.period == Catch::Approx(kSampleRate / kSineHz).epsilon(0.02));
  CHECK(m.period == Catch::Approx(kSampleRate / kSineHz).epsilon(0.02));
}

// Nothing crosses the threshold (parameter 76, 24 by default): the jack is plugged in but
// silent. After Enter the OS waits about 1.8 s for a level, gives up and shows "nS", and
// wave RAM is untouched.
TEST_CASE("acceptance 7: a silent jack never crosses the threshold", "[assets][acceptance]") {
  REQUIRE_ASSET(assetPath(kSndA01));
  REQUIRE_ASSET(romPath());
  auto m = bootedMachineAt(kSndA01);
  Jack j;
  j.amp = 0.0f;
  j.run(*m, 0.1);
  j.press(*m, PanelKey::SampleLower, 0.15, 2.5);
  REQUIRE(m->jackConnected());
  const auto level = m->display();  // level-detect, no level: 80 80
  CHECK(level.left == 0x80);
  CHECK(level.right == 0x80);

  std::vector<uint8_t> before(Machine::kSampleRamSize);
  for (uint32_t i = 0; i < before.size(); ++i) before[i] = m->sampleRam(i);
  j.press(*m, PanelKey::Enter, 0.15, 0.0);
  bool nS = false, sampled = false;
  for (size_t t = 0; t < 470; ++t) {  // 5 s
    j.run(*m, 0.01);
    const DisplayState d = m->display();
    nS = nS || (d.left == kSegNsL && d.right == kSegNsR);
    sampled = sampled || (d.left == kSegDoneL && d.right == kSegDoneR);
  }
  CHECK(nS);
  CHECK_FALSE(sampled);
  size_t changed = 0;
  for (uint32_t i = 0; i < before.size(); ++i) changed += m->sampleRam(i) != before[i];
  CHECK(changed == 0);
}

// As acceptance 3: the OS saves the sampled lower sound, the image is written out and a
// fresh machine boots from it; the same note has the same level and pitch.
TEST_CASE("acceptance 7: a sampled sound survives an OS save and reopen", "[assets][acceptance]") {
  REQUIRE_ASSET(assetPath(kSndA01));
  REQUIRE_ASSET(romPath());
  const SampledSine& s = sampledSine(false, kMicAmp);
  REQUIRE(s.done);
  auto m = machineFrom(s.state);
  // The level is the peak from 10 ms on. A note's first milliseconds depend on when the
  // OS happens to handle the note-on against its own timer ticks, which a machine that
  // booted at another moment does not share (acceptance 3's two machines happen to line
  // up bit for bit; these two do not). Measured on this sampled sound, note-on offset by
  // 0-12 ms in 1 ms steps: peak of the first 10 ms 0.064-0.074, peak after it 0.0638-0.0644
  // (the stock SND-A01 piano shows the same: 0.253-0.280 after 10 ms). The data is also
  // checked directly: the reopened wave RAM is the sampled wave RAM, byte for byte.
  constexpr size_t kAfterStart = 480;  // 10 ms at kPlayRate
  const Played before = playNote(*m, 52, kAfterStart);
  INFO("before saving: " << before.hz << " Hz, peak " << before.peak);
  REQUIRE(before.peak > 0.05f);
  REQUIRE(before.hz > 0);
  const uint8_t off[] = {0x80, 52, 0};
  m->queueMidi(off);
  runSeconds(*m, 1.0);

  osSaveLowerSound(*m);
  REQUIRE(m->disk() != nullptr);
  REQUIRE(m->disk()->dirty());
  const auto path = std::filesystem::temp_directory_path() / "mirage_acceptance7.img";
  const RemoveOnExit cleanup{path};
  m->disk()->saveFile(path);
  Machine fresh(bootRomBytes());
  fresh.insertDisk(DiskImage::loadFile(path));
  fresh.powerOn();
  settleBoot(fresh);

  CHECK(lowerHalf(fresh) == s.lower);
  const Played after = playNote(fresh, 52, kAfterStart);
  INFO("after reopening: " << after.hz << " Hz, peak " << after.peak);
  CHECK(after.peak == Catch::Approx(before.peak).epsilon(0.10));
  CHECK(after.hz == Catch::Approx(before.hz).epsilon(0.03));
}
