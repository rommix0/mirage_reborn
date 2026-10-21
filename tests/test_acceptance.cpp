// Acceptance 2, headless: edit a parameter from the panel, save with the OS,
// write the disk to a file and boot a fresh machine from it. The panel keys are
// the ones in docs/app.md (OS 3.2, musician's manual).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

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
