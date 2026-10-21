// Acceptance 2 and 5, headless. 2: edit a parameter from the panel, save with the
// OS, write the disk to a file and boot a fresh machine from it. 5: format a
// blank disk with fmt2, boot it, save a sound to it and load it back. The panel
// keys are the ones in docs/app.md (OS 3.2, musician's manual).
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
