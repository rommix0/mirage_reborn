// DiskSession (host/include/mirage/disk_session.h): the UI thread's open, save,
// save as and eject on top of RealtimeHost. Asset-free: a synthetic ROM and
// generated 450,560-byte images in a temp folder; the test thread plays the
// audio thread by calling process() itself.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "mirage/disk_image.h"
#include "mirage/disk_session.h"
#include "mirage/realtime_host.h"
#include "test_roms.h"

using namespace mirage;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

// BRA *: a CPU that runs and does nothing else.
const std::vector<uint8_t> kIdleRom = mirage_test::romWith({0x20, 0xFE});

// A fresh, empty folder for one test, removed when the test ends.
struct TempDir {
  fs::path dir;
  explicit TempDir(const std::string& name) : dir(fs::temp_directory_path() / name) {
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  fs::path operator/(const std::string& file) const { return dir / file; }
};

void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::vector<uint8_t> readBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A 450,560-byte disk image file with every byte `fill`.
fs::path makeImage(const TempDir& tmp, const std::string& name, uint8_t fill) {
  const auto path = tmp / name;
  writeBytes(path, std::vector<uint8_t>(DiskImage::kImageBytes, fill));
  return path;
}

struct Rig {
  RealtimeHost host{kIdleRom};
  DiskSession session{host};
  Rig() { host.prepare(64, 48000); }
  void pump() {
    std::array<float, 64> buf{};
    host.process(buf.data(), buf.size(), 48000.0, 0.0);
  }
  // Writes sector 0 of track 0 with `fill`, as the OS would through the controller.
  void osWrite(uint8_t fill) {
    std::array<uint8_t, 1024> sector;
    sector.fill(fill);
    host.machineForTest().disk()->writeSector(0, 0, sector);
  }
  // Pumps process() and poll() until a save finishes.
  SaveOutcome finishSave() {
    for (int i = 0; i < 100; ++i) {
      pump();
      if (auto outcome = session.poll()) return *outcome;
    }
    FAIL("the save never finished");
    return {};
  }
};

}  // namespace

TEST_CASE("session open rejects a wrong-size file with a clear message and leaves the drive empty") {
  TempDir tmp("mirage_session_wrong_size");
  const auto file = tmp / "short.img";
  writeBytes(file, std::vector<uint8_t>(1000, 0x11));
  Rig rig;
  const std::string error = rig.session.open(file);
  CHECK_THAT(error, ContainsSubstring("450,560"));
  CHECK_THAT(error, ContainsSubstring("1000"));
  CHECK(rig.session.path().empty());
  CHECK(rig.session.title() == "No disk");
  rig.pump();
  CHECK_FALSE(rig.host.status().diskPresent);
  CHECK(rig.host.machineForTest().disk() == nullptr);
}

TEST_CASE("session open of a directory or missing file reports an error") {
  TempDir tmp("mirage_session_bad_paths");
  Rig rig;
  const std::string folder = rig.session.open(tmp.dir);
  CHECK_THAT(folder, ContainsSubstring("mirage_session_bad_paths"));
  CHECK_THAT(folder, ContainsSubstring("folder"));
  CHECK_THAT(rig.session.open(tmp / "missing.img"), ContainsSubstring("missing.img"));
  CHECK(rig.session.path().empty());
  rig.pump();
  CHECK_FALSE(rig.host.status().diskPresent);
}

TEST_CASE("session open inserts and reports the title") {
  TempDir tmp("mirage_session_open");
  // Any extension: only the size matters.
  const auto file = makeImage(tmp, "SND-A01.dsk", 0x42);
  Rig rig;
  CHECK(rig.session.title() == "No disk");
  CHECK(rig.session.open(file).empty());
  CHECK(rig.session.path() == file);
  CHECK(rig.session.title() == "SND-A01.dsk");
  rig.pump();
  CHECK(rig.host.status().diskPresent);
  const DiskImage* disk = rig.host.machineForTest().disk();
  REQUIRE(disk != nullptr);
  CHECK(disk->toBytes() == std::vector<uint8_t>(DiskImage::kImageBytes, 0x42));
}

TEST_CASE("session save writes the bytes the OS wrote and clears modified") {
  TempDir tmp("mirage_session_save");
  const auto file = makeImage(tmp, "SND-A01.img", 0x00);
  Rig rig;
  REQUIRE(rig.session.open(file).empty());
  rig.pump();
  rig.osWrite(0x5A);
  rig.pump();
  CHECK(rig.session.title() == "SND-A01.img (modified)");

  REQUIRE(rig.session.startSave());
  CHECK(rig.session.saving());
  const SaveOutcome outcome = rig.finishSave();
  CHECK(outcome.ok);
  CHECK(outcome.message.empty());
  CHECK(outcome.path == file);
  CHECK_FALSE(rig.session.saving());

  const auto bytes = readBytes(file);
  REQUIRE(bytes.size() == size_t(DiskImage::kImageBytes));
  CHECK(bytes[0] == 0x5A);
  CHECK(bytes[1023] == 0x5A);
  CHECK(bytes[1024] == 0x00);
  rig.pump();  // applies the mark-saved
  CHECK_FALSE(rig.host.status().diskDirty);
  CHECK(rig.session.title() == "SND-A01.img");
  CHECK_FALSE(rig.session.poll().has_value());  // the outcome is reported once
}

TEST_CASE("session save to an unwritable path fails and keeps modified") {
  TempDir tmp("mirage_session_unwritable");
  const auto file = makeImage(tmp, "SND-A01.img", 0x00);
  Rig rig;
  REQUIRE(rig.session.open(file).empty());
  rig.pump();
  rig.osWrite(0x5A);
  const auto target = tmp / "no-such-folder" / "out.img";
  REQUIRE(rig.session.startSave(target));
  const SaveOutcome outcome = rig.finishSave();
  CHECK_FALSE(outcome.ok);
  CHECK_THAT(outcome.message, ContainsSubstring("out.img"));
  CHECK(outcome.path == target);
  rig.pump();
  CHECK(rig.host.status().diskDirty);
  CHECK(rig.session.path() == file);  // still the file it came from
  CHECK(rig.session.title() == "SND-A01.img (modified)");
  CHECK(readBytes(file)[0] == 0x00);
}

TEST_CASE("session needsSaveBeforeReplacing only when dirty") {
  TempDir tmp("mirage_session_needs_save");
  const auto file = makeImage(tmp, "a.img", 0x00);
  Rig rig;
  CHECK_FALSE(rig.session.needsSaveBeforeReplacing());  // no disk
  REQUIRE(rig.session.open(file).empty());
  rig.pump();
  CHECK_FALSE(rig.session.needsSaveBeforeReplacing());  // clean
  rig.osWrite(0x01);
  rig.pump();
  CHECK(rig.session.needsSaveBeforeReplacing());
  REQUIRE(rig.session.startSave());
  REQUIRE(rig.finishSave().ok);
  rig.pump();
  CHECK_FALSE(rig.session.needsSaveBeforeReplacing());
  rig.osWrite(0x02);
  rig.pump();
  CHECK(rig.session.needsSaveBeforeReplacing());
  rig.session.eject();
  rig.pump();
  CHECK_FALSE(rig.session.needsSaveBeforeReplacing());  // the dirty disk is gone
}

TEST_CASE("session save as switches the path") {
  TempDir tmp("mirage_session_save_as");
  const auto original = makeImage(tmp, "a.img", 0x00);
  const auto copy = tmp / "b.img";
  Rig rig;
  REQUIRE(rig.session.open(original).empty());
  rig.pump();
  rig.osWrite(0x77);
  REQUIRE(rig.session.startSave(copy));
  const SaveOutcome outcome = rig.finishSave();
  REQUIRE(outcome.ok);
  CHECK(outcome.path == copy);
  CHECK(rig.session.path() == copy);
  rig.pump();
  CHECK(rig.session.title() == "b.img");
  CHECK(readBytes(copy)[0] == 0x77);
  CHECK(readBytes(original)[0] == 0x00);  // the original is untouched

  // A later plain Save goes to the new file.
  rig.osWrite(0x78);
  REQUIRE(rig.session.startSave());
  REQUIRE(rig.finishSave().ok);
  CHECK(readBytes(copy)[0] == 0x78);
  CHECK(readBytes(original)[0] == 0x00);
}

TEST_CASE("session save with no disk reports it") {
  TempDir tmp("mirage_session_no_disk");
  Rig rig;
  CHECK_FALSE(rig.session.startSave());  // no path to save to
  REQUIRE(rig.session.startSave(tmp / "x.img"));
  const SaveOutcome outcome = rig.finishSave();
  CHECK_FALSE(outcome.ok);
  CHECK(outcome.message == "There is no disk in the drive.");
  CHECK_FALSE(fs::exists(tmp / "x.img"));
}

TEST_CASE("session frees ejected disks") {
  TempDir tmp("mirage_session_frees");
  const auto a = makeImage(tmp, "a.img", 1), b = makeImage(tmp, "b.img", 2),
             c = makeImage(tmp, "c.img", 3);
  Rig rig;
  REQUIRE(rig.session.open(a).empty());
  rig.pump();
  REQUIRE(rig.session.open(b).empty());  // swap: a comes back Ejected
  rig.pump();
  REQUIRE(rig.session.open(c).empty());  // during the gap: b comes back Unused
  rig.pump();
  rig.session.eject();  // c, pending, comes back Unused
  rig.pump();
  CHECK(rig.session.path().empty());
  CHECK(rig.session.title() == "No disk");
  CHECK_FALSE(rig.session.poll().has_value());
  CHECK_FALSE(rig.host.takeReturnedDisk().has_value());  // poll() took and freed them all
  CHECK_FALSE(rig.host.status().diskPresent);
}
