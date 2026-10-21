// Acceptance 2-6, headless. 2: edit a parameter from the panel, save with the
// OS, write the disk to a file and boot a fresh machine from it. 3: import a WAV
// into wave RAM, play it across the keyboard, save the sound with the OS and
// reopen the image. 4: other OS images boot and play. 5: format a blank disk with
// fmt2, boot it, save a sound to it and load it back. 6: .edm disks, raw and
// Giebler, boot and play. The panel keys are the ones in docs/app.md (OS 3.2,
// musician's manual).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
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
  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));

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
  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));

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
  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));

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

// Note-on at velocity 100; the peak over 0.25 s and the fundamental (zero
// crossings of the mean-removed signal from 31 ms to 125 ms, after the attack).
Played playNote(Machine& m, int note) {
  const uint8_t on[] = {0x90, uint8_t(note), 100};
  m.queueMidi(on);
  std::vector<float> v(kPlayRate / 4);
  m.render(v.data(), v.size(), kPlayRate);
  Played p;
  for (float x : v) p.peak = std::max(p.peak, std::fabs(x));
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
  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));
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
  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));
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
  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));
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
