// DiskSession (host/include/mirage/disk_session.h): the UI thread's open, save,
// save as and eject on top of RealtimeHost. Asset-free: a synthetic ROM and
// generated disk images (raw and Giebler .edm) in a temp folder; the test
// thread plays the audio thread by calling process() itself.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
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
  double now = 0.0;  // the session's clock, in seconds; tests move it by hand
  DiskSession session{host, [this] { return now; }};
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
  CHECK(rig.session.open(file) ==
        "short.img: not a Mirage disk image (expected 450,560 bytes or a Giebler .edm file)");
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
  // Any extension: only the content matters.
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

TEST_CASE("session is clean right after a successful save, before the host confirms") {
  TempDir tmp("mirage_session_clean_after_save");
  const auto file = makeImage(tmp, "a.img", 0x00);
  Rig rig;
  REQUIRE(rig.session.open(file).empty());
  rig.pump();
  rig.osWrite(0x01);
  rig.pump();
  REQUIRE(rig.session.needsSaveBeforeReplacing());
  REQUIRE(rig.session.startSave());
  REQUIRE(rig.finishSave().ok);
  // The mark-saved is not applied yet (no process() since), and the published
  // status still says dirty, but the saved copy holds every write.
  REQUIRE(rig.host.status().diskDirty);
  CHECK_FALSE(rig.session.needsSaveBeforeReplacing());
  CHECK(rig.session.title() == "a.img");
  // A newer write makes it modified again.
  rig.pump();
  rig.osWrite(0x02);
  rig.pump();
  CHECK(rig.session.needsSaveBeforeReplacing());
  CHECK(rig.session.title() == "a.img (modified)");
}

TEST_CASE("session keeps modified when the OS writes between the copy and the mark") {
  TempDir tmp("mirage_session_write_race");
  const auto file = makeImage(tmp, "a.img", 0x00);
  Rig rig;
  REQUIRE(rig.session.open(file).empty());
  rig.pump();
  rig.osWrite(0x11);
  rig.pump();
  REQUIRE(rig.session.startSave());
  rig.pump();         // the copy is taken
  rig.osWrite(0x22);  // a write the copy does not hold
  const auto outcome = rig.session.poll();  // the file is written, the mark posted
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->ok);
  rig.pump();  // the mark is ignored: the write count moved on
  CHECK(rig.host.status().diskDirty);
  CHECK(rig.session.needsSaveBeforeReplacing());
  CHECK(rig.session.title() == "a.img (modified)");
  CHECK(readBytes(file)[0] == 0x11);  // the pre-write bytes
}

TEST_CASE("session save times out when the audio thread is not running") {
  TempDir tmp("mirage_session_timeout");
  const auto file = makeImage(tmp, "a.img", 0x00);
  Rig rig;
  REQUIRE(rig.session.open(file).empty());
  rig.pump();
  rig.osWrite(0x33);
  rig.pump();
  REQUIRE(rig.session.startSave());
  // No process() calls: the copy never comes back.
  rig.now = DiskSession::kSaveTimeoutSeconds - 0.1;
  CHECK_FALSE(rig.session.poll().has_value());
  CHECK(rig.session.saving());
  rig.now = DiskSession::kSaveTimeoutSeconds;
  const auto outcome = rig.session.poll();
  REQUIRE(outcome.has_value());
  CHECK_FALSE(outcome->ok);
  CHECK(outcome->message ==
        "The audio engine is not running; the disk was not saved. Check Audio Settings.");
  CHECK(outcome->path == file);
  CHECK_FALSE(rig.session.saving());
  CHECK(readBytes(file)[0] == 0x00);

  SECTION("a late copy is freed, and a new save still works") {
    rig.pump();  // the audio thread resumes: the stale copy comes back
    CHECK_FALSE(rig.session.poll().has_value());
    CHECK_FALSE(rig.host.takeReturnedDisk().has_value());  // freed by poll()
    CHECK(readBytes(file)[0] == 0x00);                      // and never written
    CHECK(rig.session.needsSaveBeforeReplacing());
    REQUIRE(rig.session.startSave());
    const SaveOutcome again = rig.finishSave();
    CHECK(again.ok);
    CHECK(readBytes(file)[0] == 0x33);
  }
  SECTION("a new save started before the late copy returns takes its own copy") {
    REQUIRE(rig.session.startSave());
    rig.pump();  // both copies come back in one block, the stale one first
    const auto again = rig.session.poll();
    REQUIRE(again.has_value());
    CHECK(again->ok);
    CHECK_FALSE(rig.session.poll().has_value());
    CHECK_FALSE(rig.host.takeReturnedDisk().has_value());
    CHECK(readBytes(file)[0] == 0x33);
  }
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

namespace {
// The first 13 bytes of a Giebler .edm file (docs/hardware/fdc.md).
bool isGiebler(const std::vector<uint8_t>& file) {
  return file.size() > 13 && std::string(file.begin(), file.begin() + 13) == "\r\nMirage Disk";
}
}  // namespace

TEST_CASE("session saves a Giebler .edm back as Giebler") {
  TempDir tmp("mirage_session_giebler");
  const auto file = tmp / "SP1.EDM";
  DiskImage::fromBytes(std::vector<uint8_t>(DiskImage::kImageBytes, 0x55))
      .saveFile(file, DiskFormat::GieblerEdm);
  Rig rig;
  REQUIRE(rig.session.open(file).empty());
  rig.pump();
  CHECK(rig.session.format() == DiskFormat::GieblerEdm);
  rig.osWrite(0xA5);
  REQUIRE(rig.session.startSave());
  REQUIRE(rig.finishSave().ok);
  const auto saved = readBytes(file);
  CHECK(isGiebler(saved));
  std::vector<uint8_t> expected(DiskImage::kImageBytes, 0x55);
  std::fill_n(expected.begin(), 1024, uint8_t(0xA5));
  CHECK(DiskImage::fromFileBytes(saved).toBytes() == expected);

  // Save As follows the new name: .img is raw, after which plain Save stays raw.
  REQUIRE(rig.session.startSave(tmp / "copy.img"));
  REQUIRE(rig.finishSave().ok);
  CHECK(rig.session.format() == DiskFormat::Raw);
  CHECK(readBytes(tmp / "copy.img") == expected);
  rig.osWrite(0x5A);
  REQUIRE(rig.session.startSave());
  REQUIRE(rig.finishSave().ok);
  CHECK(readBytes(tmp / "copy.img").size() == size_t(DiskImage::kImageBytes));
  CHECK(readBytes(tmp / "copy.img")[0] == 0x5A);
}

TEST_CASE("session Save As picks the format by name: .edm (any case) Giebler, else raw") {
  TempDir tmp("mirage_session_save_as_format");
  const auto original = makeImage(tmp, "a.img", 0x11);
  Rig rig;
  REQUIRE(rig.session.open(original).empty());
  rig.pump();
  for (const char* name : {"x.edm", "y.EdM", "z.img", "noext"}) {
    INFO(name);
    REQUIRE(rig.session.startSave(tmp / name));
    REQUIRE(rig.finishSave().ok);
    const auto saved = readBytes(tmp / name);
    const bool edm = name[0] == 'x' || name[0] == 'y';
    CHECK(isGiebler(saved) == edm);
    CHECK(rig.session.format() == (edm ? DiskFormat::GieblerEdm : DiskFormat::Raw));
    CHECK(DiskImage::fromFileBytes(saved).toBytes() ==
          std::vector<uint8_t>(DiskImage::kImageBytes, 0x11));
    if (!edm) CHECK(saved.size() == size_t(DiskImage::kImageBytes));
  }
  // Then plain Save keeps the last one's format (raw for "noext").
  rig.osWrite(0x22);
  REQUIRE(rig.session.startSave());
  REQUIRE(rig.finishSave().ok);
  CHECK(readBytes(tmp / "noext").size() == size_t(DiskImage::kImageBytes));
}

TEST_CASE("session saves a raw .edm back as raw") {
  TempDir tmp("mirage_session_raw_edm");
  const auto edm = makeImage(tmp, "disk.edm", 0x55);
  Rig rig;
  REQUIRE(rig.session.open(edm).empty());
  rig.pump();
  CHECK(rig.session.format() == DiskFormat::Raw);  // a raw .edm: Save As suggests .img
  rig.osWrite(0xA5);
  REQUIRE(rig.session.startSave());
  REQUIRE(rig.finishSave().ok);
  const auto saved = readBytes(edm);
  REQUIRE(saved.size() == size_t(DiskImage::kImageBytes));
  CHECK(saved[0] == 0xA5);
  CHECK(saved[1024] == 0x55);
}

TEST_CASE("session save during the insert gap says the disk is still going in") {
  TempDir tmp("mirage_session_gap_save");
  const auto a = makeImage(tmp, "a.img", 1), b = makeImage(tmp, "b.img", 2);
  Rig rig;
  REQUIRE(rig.session.open(a).empty());
  rig.pump();
  REQUIRE(rig.session.open(b).empty());  // swap: b waits out the 0.5 s gap
  REQUIRE(rig.session.startSave());
  const SaveOutcome outcome = rig.finishSave();
  CHECK_FALSE(outcome.ok);
  CHECK(outcome.message == "The disk is still going into the drive; try again in a moment.");
  CHECK(rig.session.path() == b);
  CHECK(readBytes(b)[0] == 2);  // untouched
}

TEST_CASE("disk session new blank inserts an unformatted disk with no path") {
  Rig rig;
  CHECK(rig.session.newBlank());
  CHECK(rig.session.path().empty());
  CHECK(rig.session.title() == "Untitled (blank)");
  CHECK(rig.session.hasDisk());
  rig.pump();
  CHECK(rig.host.status().diskPresent);
  const DiskImage* disk = rig.host.machineForTest().disk();
  REQUIRE(disk != nullptr);
  CHECK(disk->formattedTracks() == 0);
  CHECK_FALSE(rig.session.needsSaveBeforeReplacing());  // nothing written yet
  // A plain Save has no file to go to.
  CHECK_FALSE(rig.session.startSave());
}

TEST_CASE("disk session new blank replaces a disk; refused while a save is in flight") {
  TempDir tmp("mirage_session_new_blank_swap");
  const auto file = makeImage(tmp, "a.img", 0x00);
  Rig rig;
  REQUIRE(rig.session.open(file).empty());
  rig.pump();
  rig.osWrite(0x01);
  rig.pump();
  REQUIRE(rig.session.startSave());
  CHECK_FALSE(rig.session.newBlank());  // the mark-saved must reach the disk that was copied
  REQUIRE(rig.finishSave().ok);
  CHECK(rig.session.newBlank());
  rig.pump();
  CHECK(rig.session.title() == "Untitled (blank)");  // the published status has moved on to the new disk
  CHECK(rig.session.poll() == std::nullopt);  // the old disk is freed
  CHECK(rig.host.machineForTest().disk() == nullptr);  // waiting out the swap gap
}

TEST_CASE("disk session refuses to save a partly formatted disk") {
  TempDir tmp("mirage_session_partial");
  Rig rig;
  REQUIRE(rig.session.newBlank());
  rig.pump();
  std::array<uint8_t, DiskImage::kTrackBytes> track;
  track.fill(0x5A);
  DiskImage* disk = rig.host.machineForTest().disk();
  REQUIRE(disk != nullptr);
  disk->setTrack(0, track);
  disk->setTrack(1, track);
  disk->setTrack(79, track);
  rig.pump();
  CHECK(rig.session.needsSaveBeforeReplacing());

  for (const char* name : {"out.img", "out.edm"}) {
    INFO(name);
    const auto target = tmp / name;
    REQUIRE(rig.session.startSave(target));
    const SaveOutcome outcome = rig.finishSave();
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.message ==
          "This disk is not fully formatted (3 of 80 tracks). Format it with the OS before saving.");
    CHECK(outcome.path == target);
    CHECK(fs::is_empty(tmp.dir));  // neither the file nor its .tmp
  }
  rig.pump();
  CHECK(rig.host.status().diskDirty);
  CHECK(rig.session.needsSaveBeforeReplacing());
  CHECK(rig.session.path().empty());  // still untitled
  CHECK(rig.session.title() == "Untitled (blank) (modified)");
}

TEST_CASE("disk session saves a formatted blank disk and then follows its file") {
  TempDir tmp("mirage_session_blank_save");
  Rig rig;
  REQUIRE(rig.session.newBlank());
  rig.pump();
  std::array<uint8_t, DiskImage::kTrackBytes> track;
  track.fill(0x33);
  DiskImage* disk = rig.host.machineForTest().disk();
  REQUIRE(disk != nullptr);
  for (int t = 0; t < DiskImage::kTracks; ++t) disk->setTrack(t, track);
  rig.pump();
  const auto target = tmp / "new.img";
  REQUIRE(rig.session.startSave(target));
  const SaveOutcome outcome = rig.finishSave();
  REQUIRE(outcome.ok);
  CHECK(rig.session.path() == target);
  rig.pump();
  CHECK(rig.session.title() == "new.img");
  CHECK(readBytes(target).size() == size_t(DiskImage::kImageBytes));
}
