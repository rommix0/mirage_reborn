// Giebler .edm disk files (docs/hardware/fdc.md, "Disk file formats"): detection
// by content, decoding, the writer, damaged files. The [assets] cases pair the
// Triton Soundprocess .EDM files with the raw rips of the same disks.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "asset_paths.h"
#include "mirage/disk_image.h"

using namespace mirage;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

constexpr const char* kNotADisk =
    "not a Mirage disk image (expected 450,560 bytes or a Giebler .edm file)";
constexpr const char* kTriton = "Soundprocess/TritonSoundprocess/";
constexpr const char* kEdmIn = "Tools/Ensoniq EDM2IMG & EDM2HFE (for Ensoniq Mirage) v0.3/[EDM_IN]/";

std::vector<uint8_t> readBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

// Byte offset of a sector in the raw image.
size_t rawOffset(int track, int sector) {
  return size_t(track) * DiskImage::kTrackBytes + size_t(sector < 5 ? sector * 1024 : 5120);
}

// A raw image with a pattern everywhere except some all-zero sectors: track 2
// sectors 0-4 (as on the Triton disks), track 0 sector 5, track 79 sector 3.
std::vector<uint8_t> patternedImage() {
  std::vector<uint8_t> raw(DiskImage::kImageBytes);
  for (size_t i = 0; i < raw.size(); ++i) raw[i] = uint8_t(1 + (i * 7 + (i >> 9)) % 255);
  const auto zero = [&](int t, int s) {
    std::fill_n(raw.begin() + std::ptrdiff_t(rawOffset(t, s)), s < 5 ? 1024 : 512, uint8_t(0));
  };
  for (int s = 0; s < 5; ++s) zero(2, s);
  zero(0, 5);
  zero(79, 3);
  return raw;
}

struct TempDir {
  fs::path dir;
  explicit TempDir(const std::string& name)
      : dir(fs::temp_directory_path() /
            (name + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    fs::create_directories(dir);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

}  // namespace

TEST_CASE("edm: the writer's layout") {
  const auto raw = patternedImage();
  const auto edm = DiskImage::fromBytes(raw).toEdmBytes();
  // 480 sectors, 7 of them all zero and left out; every stored sector takes 1024 bytes.
  REQUIRE(edm.size() == size_t(512 + 473 * 1024 + 1));
  const std::string head(edm.begin(), edm.begin() + 13);
  CHECK(head == "\r\nMirage Disk");
  CHECK(edm[0x4E] == 0x0D);
  CHECK(edm[0x4F] == 0x0A);
  CHECK(edm[0x9D] == 0x0D);
  CHECK(edm[0x9E] == 0x0A);
  CHECK(edm[0x9F] == 0x1A);
  CHECK(edm[0x1FF] == 0x01);
  CHECK(edm.back() == 0x1A);
  // Bitmap, per track in the order 5, 0, 1, 2, 3, 4, MSB first; 1 = left out.
  CHECK(edm[0xA0] == 0x80);  // track 0 sector 5
  CHECK(edm[0xA1] == 0x07);  // track 2 sectors 0-4 are bits 13-17
  CHECK(edm[0xA2] == 0xC0);
  CHECK(edm[0xDB] == 0x02);  // track 79 sector 3 is bit 478
  CHECK(std::all_of(edm.begin() + 0xDC, edm.begin() + 0x1FF, [](uint8_t b) { return b == 0; }));
  // Track 0: sector 5 is left out, so sector 0 comes first, then 1-4; track 1
  // starts with its sector 5, padded to 1024 with zeros.
  CHECK(std::equal(edm.begin() + 0x200, edm.begin() + 0x600, raw.begin()));
  const auto t1s5 = raw.begin() + std::ptrdiff_t(rawOffset(1, 5));
  CHECK(std::equal(edm.begin() + 0x1600, edm.begin() + 0x1800, t1s5));
  CHECK(std::all_of(edm.begin() + 0x1800, edm.begin() + 0x1A00, [](uint8_t b) { return b == 0; }));
  CHECK(std::equal(edm.begin() + 0x1A00, edm.begin() + 0x1E00,
                   raw.begin() + std::ptrdiff_t(rawOffset(1, 0))));
}

TEST_CASE("edm: round trip, and detection by content") {
  const auto raw = patternedImage();
  const auto edm = DiskImage::fromBytes(raw).toEdmBytes();
  CHECK(detectDiskFormat(edm) == DiskFormat::GieblerEdm);
  CHECK(detectDiskFormat(raw) == DiskFormat::Raw);
  DiskFormat found = DiskFormat::Raw;
  const DiskImage back = DiskImage::fromFileBytes(edm, &found);
  CHECK(found == DiskFormat::GieblerEdm);
  CHECK(back.toBytes() == raw);
  CHECK(back.toEdmBytes() == edm);
  CHECK_FALSE(back.dirty());
  CHECK(DiskImage::fromFileBytes(raw, &found).toBytes() == raw);
  CHECK(found == DiskFormat::Raw);

  // An all-zero disk is the header, nothing stored, and the end byte.
  const auto empty = DiskImage::fromBytes(std::vector<uint8_t>(DiskImage::kImageBytes)).toEdmBytes();
  CHECK(empty.size() == 513u);
  CHECK(DiskImage::fromFileBytes(empty).toBytes() == std::vector<uint8_t>(DiskImage::kImageBytes));
  // Whatever follows the stored sectors is ignored (one $1A from Giebler's
  // writer; M_11.EDM ends in $98).
  auto noEnd = edm;
  noEnd.pop_back();
  CHECK(DiskImage::fromFileBytes(noEnd).toBytes() == raw);
}

TEST_CASE("edm: an unformatted track cannot be written") {
  REQUIRE_THROWS_AS(DiskImage::blank().toEdmBytes(), DiskImageError);
  auto d = DiskImage::fromBytes(patternedImage());
  d.unformatTrack(40);
  REQUIRE_THROWS_WITH(d.toEdmBytes(), ContainsSubstring("track 40"));
}

TEST_CASE("edm: anything else is not a disk image") {
  for (size_t n : {size_t(0), size_t(13), size_t(450559), size_t(450561), size_t(819200)})
    REQUIRE_THROWS_WITH(detectDiskFormat(std::vector<uint8_t>(n)), kNotADisk);
  REQUIRE_THROWS_WITH(DiskImage::fromFileBytes(std::vector<uint8_t>(1000)), kNotADisk);
  std::vector<uint8_t> almost = {0x0D, 0x0A, 'M', 'i', 'r', 'a', 'g', 'e', ' ', 'D', 'i', 's', 'c'};
  almost.resize(2000);
  REQUIRE_THROWS_WITH(detectDiskFormat(almost), kNotADisk);
  // A raw image wins even if it happens to start like a Giebler header.
  auto raw = patternedImage();
  const auto edm = DiskImage::fromBytes(raw).toEdmBytes();
  std::copy_n(edm.begin(), 512, raw.begin());
  CHECK(detectDiskFormat(raw) == DiskFormat::Raw);
}

TEST_CASE("edm: damaged Giebler files throw and never read out of bounds") {
  const auto edm = DiskImage::fromBytes(patternedImage()).toEdmBytes();
  const auto cut = [&](size_t n) { return std::vector<uint8_t>(edm.begin(), edm.begin() + std::ptrdiff_t(n)); };
  // Cut inside the header: the bitmap is incomplete.
  REQUIRE_THROWS_WITH(DiskImage::fromFileBytes(cut(0x100)), ContainsSubstring("damaged"));
  REQUIRE_THROWS_WITH(DiskImage::fromFileBytes(cut(13)), ContainsSubstring("damaged"));
  // Cut in the middle of a sector (exact sizes: the decoder must check before copying).
  for (size_t n : {size_t(0x200), size_t(0x200 + 700), size_t(0x10000 + 3), edm.size() - 2})
    REQUIRE_THROWS_WITH(DiskImage::fromFileBytes(cut(n)), ContainsSubstring("damaged"));
  // A bitmap that says every sector is stored, but only some of them are there.
  auto allStored = edm;
  std::fill(allStored.begin() + 0xA0, allStored.begin() + 0xDC, uint8_t(0));
  REQUIRE_THROWS_WITH(DiskImage::fromFileBytes(allStored),
                      ContainsSubstring("damaged") && ContainsSubstring("480"));
}

TEST_CASE("edm: loadFile and saveFile in both formats") {
  TempDir tmp("mirage_edm_files");
  const auto raw = patternedImage();
  const auto disk = DiskImage::fromBytes(raw);

  disk.saveFile(tmp.dir / "g.edm", DiskFormat::GieblerEdm);
  CHECK(readBytes(tmp.dir / "g.edm") == disk.toEdmBytes());
  disk.saveFile(tmp.dir / "r.img", DiskFormat::Raw);
  CHECK(readBytes(tmp.dir / "r.img") == raw);
  disk.saveFile(tmp.dir / "plain.edm");  // the one-argument saveFile stays raw
  CHECK(readBytes(tmp.dir / "plain.edm") == raw);
  CHECK_FALSE(fs::exists(tmp.dir / "g.edm.tmp"));

  // Detection is by content: names do not matter.
  fs::copy_file(tmp.dir / "g.edm", tmp.dir / "giebler.img");
  for (const char* name : {"g.edm", "giebler.img", "r.img", "plain.edm"}) {
    DiskFormat found = DiskFormat::Raw;
    INFO(name);
    CHECK(DiskImage::loadFile(tmp.dir / name, &found).toBytes() == raw);
    CHECK(found == (std::string(name).find('g') == 0 ? DiskFormat::GieblerEdm : DiskFormat::Raw));
  }

  writeBytes(tmp.dir / "short.edm", std::vector<uint8_t>(1000, 0x20));
  REQUIRE_THROWS_WITH(DiskImage::loadFile(tmp.dir / "short.edm"),
                      std::string("short.edm: ") + kNotADisk);
  const auto edm = disk.toEdmBytes();
  writeBytes(tmp.dir / "cut.edm", std::vector<uint8_t>(edm.begin(), edm.begin() + 0x3000));
  REQUIRE_THROWS_WITH(DiskImage::loadFile(tmp.dir / "cut.edm"),
                      ContainsSubstring("cut.edm: ") && ContainsSubstring("damaged"));
  // Huge files are refused without reading them all.
  { std::ofstream(tmp.dir / "big.hfe", std::ios::binary).seekp(2008063).put('\0'); }
  REQUIRE_THROWS_WITH(DiskImage::loadFile(tmp.dir / "big.hfe"), std::string("big.hfe: ") + kNotADisk);
  // An unformatted track: nothing written, not even the temporary file.
  REQUIRE_THROWS_AS(DiskImage::blank().saveFile(tmp.dir / "blank.edm", DiskFormat::GieblerEdm),
                    DiskImageError);
  CHECK_FALSE(fs::exists(tmp.dir / "blank.edm"));
  CHECK_FALSE(fs::exists(tmp.dir / "blank.edm.tmp"));
}

TEST_CASE("edm: the Triton Giebler files decode to the raw rips, and re-encode exactly",
          "[assets]") {
  const std::pair<const char*, const char*> pairs[] = {
      {"SP1.EDM", "SoundProcess_Vol1.edm"}, {"SP2.EDM", "SoundProcess_Vol2.edm"},
      {"SP3.EDM", "SoundProcess_Vol3.edm"}, {"SP4.EDM", "SoundProcess_Vol4.edm"},
      {"SP5.EDM", "SoundProcess_Vol5.edm"}, {"SP6.EDM", "SoundProcess_Vol6.edm"},
      {"SNDPROCS.EDM", "Triton - Soundprocess OS.edm"}};
  for (const auto& [gieblerName, rawName] : pairs) {
    const auto giebler = assetPath(std::string(kTriton) + gieblerName);
    const auto rawFile = assetPath(std::string(kTriton) + rawName);
    REQUIRE_ASSET(giebler);
    REQUIRE_ASSET(rawFile);
    INFO(gieblerName);
    DiskFormat found = DiskFormat::Raw;
    const DiskImage g = DiskImage::loadFile(giebler, &found);
    CHECK(found == DiskFormat::GieblerEdm);
    // The raw rip is a 450,560-byte .edm: raw, by content.
    const DiskImage r = DiskImage::loadFile(rawFile, &found);
    CHECK(found == DiskFormat::Raw);
    CHECK(g.toBytes() == r.toBytes());
    CHECK(g.toEdmBytes() == readBytes(giebler));
  }
}

TEST_CASE("edm: the EDM2IMG sample files re-encode exactly", "[assets]") {
  for (const char* name : {"M_10.EDM", "M_11.EDM", "M_12.EDM"}) {
    const auto p = assetPath(std::string(kEdmIn) + name);
    REQUIRE_ASSET(p);
    INFO(name);
    const auto file = readBytes(p);
    auto again = DiskImage::loadFile(p).toEdmBytes();
    REQUIRE(again.size() == file.size());
    // M_11.EDM's last byte is $98, not the usual $1A; it is ignored on reading.
    if (std::string(name) == "M_11.EDM") again.back() = file.back();
    CHECK(again == file);
  }
}

TEST_CASE("edm: a 450,560-byte .edm is a raw image", "[assets]") {
  const auto p = assetPath("MetascapeMetaSounds/mp1.edm");
  REQUIRE_ASSET(p);
  DiskFormat found = DiskFormat::GieblerEdm;
  const DiskImage d = DiskImage::loadFile(p, &found);
  CHECK(found == DiskFormat::Raw);
  CHECK(d.toBytes() == readBytes(p));
}
