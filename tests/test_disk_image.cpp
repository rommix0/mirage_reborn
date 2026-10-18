#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

#include "asset_paths.h"
#include "mirage/disk_image.h"
#include "mirage/state_io.h"

using namespace mirage;

TEST_CASE("sector offsets match Mirage layout") {
  std::vector<uint8_t> raw(DiskImage::kImageBytes);
  raw[32*5632 + 5120 + 511] = 0x7E; raw[2*5632 + 0] = 0x11; raw[79*5632 + 4*1024] = 0x22;
  auto d = DiskImage::fromBytes(raw);
  std::array<uint8_t,1024> s{}; std::array<uint8_t,512> t{};
  d.readSector(32,5,t); REQUIRE(t[511]==0x7E);
  d.readSector(2,0,s);  REQUIRE(s[0]==0x11);
  d.readSector(79,4,s); REQUIRE(s[0]==0x22);
  REQUIRE(d.sectorSize(0,0)==1024); REQUIRE(d.sectorSize(0,5)==512); REQUIRE(d.sectorSize(0,6)==0);
}

TEST_CASE("wrong sizes are rejected with a clear message") {
  for (size_t n : {size_t(0), size_t(450559), size_t(450561), size_t(819200)})
    REQUIRE_THROWS_WITH(DiskImage::fromBytes(std::vector<uint8_t>(n)),
      Catch::Matchers::ContainsSubstring("450,560"));
  REQUIRE_THROWS_WITH(DiskImage::fromBytes(std::vector<uint8_t>(450559)),
    "Not a Mirage disk image: expected exactly 450,560 bytes, got 450559.");
}

TEST_CASE("write marks dirty and round-trips through bytes") {
  auto d = DiskImage::fromBytes(std::vector<uint8_t>(DiskImage::kImageBytes));
  REQUIRE_FALSE(d.dirty());
  std::array<uint8_t,1024> s{}; s[0] = 0x5A; d.writeSector(10,3,s);
  REQUIRE(d.dirty()); REQUIRE(d.toBytes()[10*5632 + 3*1024] == 0x5A);
  d.clearDirty(); REQUIRE_FALSE(d.dirty());
}

TEST_CASE("blank disk is unformatted and refuses toBytes") {
  auto d = DiskImage::blank(); REQUIRE_FALSE(d.trackFormatted(0));
  REQUIRE(d.sectorSize(0,0)==0); REQUIRE_THROWS_AS(d.toBytes(), DiskImageError);
}

TEST_CASE("absent sectors and wrong span sizes throw") {
  auto blank = DiskImage::blank();
  std::array<uint8_t,1024> s{}; std::array<uint8_t,512> t{};
  REQUIRE_THROWS_AS(blank.readSector(0,0,s), DiskImageError);
  REQUIRE_THROWS_AS(blank.writeSector(0,0,s), DiskImageError);
  REQUIRE_FALSE(blank.dirty());

  auto d = DiskImage::fromBytes(std::vector<uint8_t>(DiskImage::kImageBytes));
  REQUIRE_THROWS_AS(d.readSector(0,6,s), DiskImageError);   // id > 5
  REQUIRE_THROWS_AS(d.readSector(0,-1,s), DiskImageError);
  REQUIRE_THROWS_AS(d.readSector(80,0,s), DiskImageError);  // track out of range
  REQUIRE_THROWS_AS(d.readSector(-1,0,s), DiskImageError);
  REQUIRE_THROWS_AS(d.writeSector(80,0,s), DiskImageError);
  REQUIRE_THROWS_AS(d.readSector(0,0,t), DiskImageError);   // 512 into a 1024 sector
  REQUIRE_THROWS_AS(d.readSector(0,5,s), DiskImageError);   // 1024 into a 512 sector
  REQUIRE_THROWS_AS(d.writeSector(0,0,t), DiskImageError);
  REQUIRE_THROWS_AS(d.writeSector(0,5,s), DiskImageError);
  REQUIRE_FALSE(d.dirty());
  REQUIRE(d.sectorSize(80,0)==0); REQUIRE(d.sectorSize(-1,0)==0); REQUIRE(d.sectorSize(0,-1)==0);
  REQUIRE_FALSE(d.trackFormatted(80)); REQUIRE_FALSE(d.trackFormatted(-1));
}

TEST_CASE("state round-trips contents, formatted flags and dirty") {
  auto full = DiskImage::fromBytes(std::vector<uint8_t>(DiskImage::kImageBytes, 0x33));
  std::array<uint8_t,512> t{}; t[511] = 0xA5; full.writeSector(7,5,t);

  StateWriter w; full.save(w); auto buf = w.take();
  auto d = DiskImage::blank();
  StateReader r(buf); d.load(r); REQUIRE(r.atEnd());
  REQUIRE(d.dirty()); REQUIRE(d.toBytes() == full.toBytes());
  std::array<uint8_t,512> o{}; d.readSector(7,5,o); REQUIRE(o[511]==0xA5);

  // A clean blank disk loaded over a formatted dirty one.
  StateWriter w2; DiskImage::blank().save(w2); auto buf2 = w2.take();
  StateReader r2(buf2); d.load(r2);
  REQUIRE_FALSE(d.dirty()); REQUIRE_FALSE(d.trackFormatted(7)); REQUIRE_FALSE(d.trackFormatted(0));

  // Truncated state throws and leaves the image untouched.
  auto good = DiskImage::fromBytes(std::vector<uint8_t>(DiskImage::kImageBytes, 0x44));
  buf.resize(buf.size() / 2);
  StateReader r3(buf); REQUIRE_THROWS_AS(good.load(r3), StateError);
  REQUIRE(good.trackFormatted(79)); REQUIRE(good.toBytes()[0] == 0x44);
}

TEST_CASE("mixed formatted/unformatted state is preserved") {
  // Craft a state with only track 3 formatted.
  StateWriter w; w.tag(fourcc("DSKI")); w.u8(0);
  for (int i = 0; i < DiskImage::kTracks; ++i) {
    w.u8(i == 3 ? 1 : 0);
    if (i == 3) w.bytes(std::vector<uint8_t>(DiskImage::kTrackBytes, 0x99));
  }
  auto buf = w.take();
  auto d = DiskImage::blank(); StateReader r(buf); d.load(r);
  REQUIRE(d.trackFormatted(3)); REQUIRE_FALSE(d.trackFormatted(2)); REQUIRE_FALSE(d.trackFormatted(4));
  std::array<uint8_t,1024> s{}; d.readSector(3,2,s); REQUIRE(s[100]==0x99);
  StateWriter w2; d.save(w2); REQUIRE(w2.take() == buf);
}

TEST_CASE("file I/O: any extension, round trip, and clear errors") {
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / "mirage_disk_image_test";
  fs::remove_all(dir);
  fs::create_directories(dir);

  std::vector<uint8_t> raw(DiskImage::kImageBytes);
  for (size_t i = 0; i < raw.size(); ++i) raw[i] = uint8_t(i * 7 + (i >> 8));

  auto noExt = dir / "os32";
  DiskImage::fromBytes(raw).saveFile(noExt);
  REQUIRE(fs::file_size(noExt) == DiskImage::kImageBytes);
  REQUIRE(DiskImage::loadFile(noExt).toBytes() == raw);

  auto odd = dir / "disk.weird-ext";
  DiskImage::fromBytes(raw).saveFile(odd);
  REQUIRE(DiskImage::loadFile(odd).toBytes() == raw);

  auto bad = dir / "short.img";
  { std::ofstream(bad, std::ios::binary) << "tiny"; }
  REQUIRE_THROWS_WITH(DiskImage::loadFile(bad), Catch::Matchers::ContainsSubstring("450,560"));
  { std::ofstream(bad, std::ios::binary | std::ios::trunc); }  // empty
  REQUIRE_THROWS_AS(DiskImage::loadFile(bad), DiskImageError);

  auto missing = dir / "does-not-exist.img";
  REQUIRE_THROWS_WITH(DiskImage::loadFile(missing),
                      Catch::Matchers::ContainsSubstring("does-not-exist.img"));
  REQUIRE_THROWS_AS(DiskImage::loadFile(dir), DiskImageError);  // a directory

  REQUIRE_THROWS_AS(DiskImage::blank().saveFile(dir / "blank.img"), DiskImageError);
  auto nodir = dir / "no" / "such" / "dir" / "x.img";
  REQUIRE_THROWS_WITH(DiskImage::fromBytes(raw).saveFile(nodir),
                      Catch::Matchers::ContainsSubstring("x.img"));

  fs::remove_all(dir);
}

TEST_CASE("factory disk directory byte", "[assets]") {
  auto p = assetPath("Factory Library/IMG/SND-A01.img"); REQUIRE_ASSET(p);
  std::array<uint8_t,512> t{}; DiskImage::loadFile(p).readSector(32,5,t); REQUIRE(t[511]==0x7E);
}

TEST_CASE("extensionless image loads", "[assets]") {
  auto p = assetPath("EnsoniqOS/os32"); REQUIRE_ASSET(p);
  REQUIRE_NOTHROW(DiskImage::loadFile(p));
}
