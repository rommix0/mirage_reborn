#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

#include "mirage/clocks.h"
#include "mirage/disk_image.h"
#include "mirage/machine.h"
#include "mirage/wd1772.h"

using namespace mirage;

namespace {

constexpr uint32_t kRev = kDiskIndexCycles;  // one revolution, 400000 E cycles

// Sector (3, 2) holds i & 0xFF; everything else is zero.
DiskImage makeDisk() {
  std::vector<uint8_t> raw(DiskImage::kImageBytes, 0);
  for (int i = 0; i < 1024; ++i) raw[3 * 5632 + 2 * 1024 + i] = uint8_t(i & 0xFF);
  return DiskImage::fromBytes(raw);
}

// Controller + synthetic disk, motor running. Commands in these tests use h=1
// (bit 3), as the boot ROM does, unless a test is about spin-up.
struct Rig {
  DiskImage disk = makeDisk();
  Wd1772 fdc;
  Rig() {
    fdc.setDisk(&disk);
    fdc.setMotor(true);
  }
  // Ticks until the command ends; returns the E cycles it took (or `limit`).
  uint64_t runIdle(uint64_t limit = 8ull * kRev) {
    uint64_t n = 0;
    while (fdc.busy() && n < limit) {
      fdc.tick(16);
      n += 16;
    }
    return n;
  }
  // Issues a command and waits for INTRQ; returns the status (reading it clears INTRQ).
  uint8_t run(uint8_t cmd, uint64_t limit = 8ull * kRev) {
    fdc.write(0, cmd);
    runIdle(limit);
    return fdc.read(0);
  }
  void seek(int track) {
    fdc.write(3, uint8_t(track));
    REQUIRE((run(0x18) & 0x99) == 0);  // seek, h=1, no verify
    REQUIRE(fdc.read(1) == track);
  }
  // Services DRQ immediately, collecting `n` bytes.
  std::vector<uint8_t> readBytes(size_t n) {
    std::vector<uint8_t> out;
    for (uint64_t guard = 0; out.size() < n && guard < 12ull * kRev; ++guard) {
      if (fdc.drq()) out.push_back(fdc.read(3));
      else fdc.tick(1);
    }
    return out;
  }
};

uint16_t crcCcitt(std::initializer_list<uint8_t> bytes) {
  uint16_t crc = 0xFFFF;
  for (uint8_t b : bytes) {
    crc = uint16_t(crc ^ (b << 8));
    for (int i = 0; i < 8; ++i) crc = uint16_t((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
  }
  return crc;
}

}  // namespace

TEST_CASE("wd1772 constants derive from the clocks") {
  CHECK(kFdcByteCycles == 64);   // 32 us at 2 MHz
  CHECK(kDiskIndexCycles == 400000);  // 200 ms
}

TEST_CASE("wd1772 restore reaches track 0 and sets INTRQ") {
  Rig r;
  r.seek(7);
  REQUIRE(r.fdc.headTrack() == 7);
  r.fdc.write(0, 0x08);  // Restore, h=1, no verify, 6 ms steps
  CHECK(r.fdc.busy());
  CHECK_FALSE(r.fdc.intrq());
  const uint64_t took = r.runIdle();
  CHECK(r.fdc.intrq());
  CHECK(r.fdc.headTrack() == 0);
  CHECK(r.fdc.read(1) == 0);
  CHECK(took >= 7 * 12000);  // seven 6 ms steps
  const uint8_t st = r.fdc.read(0);  // reading the status clears INTRQ
  CHECK_FALSE(r.fdc.intrq());
  CHECK((st & 1) == 0);      // not busy
  CHECK((st & 0x04) != 0);   // track 0
  CHECK((st & 0x18) == 0);   // no seek or CRC error
}

TEST_CASE("wd1772 restore with verify finds an ID on track 0") {
  Rig r;
  r.seek(2);
  const uint8_t st = r.run(0x0C);  // Restore, h=1, V=1
  CHECK((st & 0x18) == 0);
  CHECK(r.fdc.read(1) == 0);
}

TEST_CASE("wd1772 seek updates the track register") {
  Rig r;
  r.fdc.write(3, 3);  // data register = target
  const uint8_t st = r.run(0x18);
  CHECK((st & 0x18) == 0);
  CHECK(r.fdc.read(1) == 3);
  CHECK(r.fdc.headTrack() == 3);
  CHECK((st & 0x04) == 0);  // no longer at track 0
  r.fdc.write(3, 1);        // and back out
  r.run(0x18);
  CHECK(r.fdc.read(1) == 1);
  CHECK(r.fdc.headTrack() == 1);
}

TEST_CASE("wd1772 step rates are 6, 12, 2 and 3 ms") {
  const uint32_t expect[4] = {6 * 2000, 12 * 2000, 2 * 2000, 3 * 2000};
  for (int rate = 0; rate < 4; ++rate) {
    Rig r;
    r.fdc.write(0, uint8_t(0x48 | rate));  // Step In, u=0, h=1
    const uint64_t took = r.runIdle();
    CHECK(took >= expect[rate]);
    CHECK(took < expect[rate] + 200);  // command latency + tick granularity
    CHECK(r.fdc.headTrack() == 1);
    CHECK(r.fdc.read(1) == 0);  // u=0: track register untouched
  }
}

TEST_CASE("wd1772 step in/out with u=1 update the track register") {
  Rig r;
  r.run(0x58);  // Step In, u=1, h=1
  r.run(0x58);
  CHECK(r.fdc.read(1) == 2);
  r.run(0x78);  // Step Out, u=1
  CHECK(r.fdc.read(1) == 1);
  CHECK(r.fdc.headTrack() == 1);
  r.run(0x38);  // Step (same direction: out), u=1
  CHECK(r.fdc.read(1) == 0);
  CHECK(r.fdc.headTrack() == 0);
}

TEST_CASE("wd1772 seek with verify reports a seek error on an unformatted track") {
  Rig r;
  r.disk = DiskImage::blank();
  r.fdc.write(3, 0);
  const uint8_t st = r.run(0x1C);  // Seek, h=1, V=1 on a blank disk
  CHECK((st & 0x10) != 0);         // seek error
  CHECK((st & 1) == 0);
}

TEST_CASE("wd1772 read sector serves the data register in order, with DRQ") {
  Rig r;
  r.seek(3);
  r.fdc.write(2, 2);
  r.fdc.write(0, 0x88);  // Read Sector, h=1
  CHECK(r.fdc.busy());
  const auto got = r.readBytes(1024);
  REQUIRE(got.size() == 1024);
  for (size_t i = 0; i < got.size(); ++i) REQUIRE(got[i] == uint8_t(i & 0xFF));
  CHECK_FALSE(r.fdc.drq());
  CHECK(r.fdc.busy());  // CRC still to come
  r.runIdle();
  CHECK(r.fdc.intrq());
  const uint8_t st = r.fdc.read(0);
  CHECK((st & 0x5D) == 0);  // no write protect, RNF, CRC, lost data; not busy
  CHECK(r.fdc.sectorsRead() == 1);
}

TEST_CASE("wd1772 DRQ is set for each byte and cleared by a data register read") {
  Rig r;
  r.seek(3);
  r.fdc.write(2, 2);
  r.fdc.write(0, 0x88);
  while (!r.fdc.drq()) r.fdc.tick(1);
  CHECK((r.fdc.read(0) & 2) != 0);  // status bit 1 mirrors DRQ
  CHECK(r.fdc.read(3) == 0x00);
  CHECK_FALSE(r.fdc.drq());
  // The next byte is due one byte time (64 cycles) later.
  r.fdc.tick(kFdcByteCycles - 1);
  CHECK_FALSE(r.fdc.drq());
  r.fdc.tick(1);
  CHECK(r.fdc.drq());
  CHECK(r.fdc.read(3) == 0x01);
}

TEST_CASE("wd1772 an unserviced DRQ sets Lost Data but the sector still completes") {
  Rig r;
  r.seek(3);
  r.fdc.write(2, 2);
  r.fdc.write(0, 0x88);
  r.runIdle();  // never read the data register
  CHECK(r.fdc.intrq());
  const uint8_t st = r.fdc.read(0);
  CHECK((st & 0x04) != 0);  // lost data
  CHECK((st & 0x10) == 0);
  CHECK((st & 1) == 0);
  CHECK_FALSE(r.fdc.drq());
}

TEST_CASE("wd1772 read sector 6 gives Record Not Found after 5 revolutions") {
  Rig r;
  r.seek(3);
  r.fdc.write(2, 6);
  r.fdc.write(0, 0x88);
  r.fdc.tick(4 * kRev - 2 * kFdcByteCycles);
  CHECK(r.fdc.busy());  // fewer than 5 index pulses so far
  const uint64_t took = r.runIdle();
  CHECK(took <= 2ull * kRev);
  CHECK(r.fdc.intrq());
  const uint8_t st = r.fdc.read(0);
  CHECK((st & 0x10) != 0);  // RNF
  CHECK((st & 1) == 0);
}

TEST_CASE("wd1772 read sector with the wrong track register is Record Not Found") {
  Rig r;
  r.seek(3);
  r.fdc.write(1, 4);  // the ID fields will say 3
  r.fdc.write(2, 2);
  const uint8_t st = r.run(0x88);
  CHECK((st & 0x10) != 0);
}

TEST_CASE("wd1772 write sector 5 on track 3 reaches the disk image") {
  Rig r;
  r.seek(3);
  REQUIRE_FALSE(r.disk.dirty());
  r.fdc.write(2, 5);
  r.fdc.write(0, 0xA8);  // Write Sector, h=1
  size_t sent = 0;
  for (uint64_t guard = 0; sent < 512 && guard < 4ull * kRev; ++guard) {
    if (r.fdc.drq()) r.fdc.write(3, uint8_t(0xA0 + (sent * 7) % 251)), ++sent;
    else r.fdc.tick(1);
  }
  REQUIRE(sent == 512);
  r.runIdle();
  CHECK(r.fdc.intrq());
  const uint8_t st = r.fdc.read(0);
  CHECK((st & 0x5D) == 0);
  CHECK(r.disk.dirty());
  std::array<uint8_t, 512> back{};
  r.disk.readSector(3, 5, back);
  for (size_t i = 0; i < back.size(); ++i) REQUIRE(back[i] == uint8_t(0xA0 + (i * 7) % 251));
  // Neighbouring data is untouched.
  std::array<uint8_t, 1024> other{};
  r.disk.readSector(3, 2, other);
  CHECK(other[5] == 5);
}

TEST_CASE("wd1772 write sector without data ends with Lost Data and writes nothing") {
  Rig r;
  r.seek(3);
  r.fdc.write(2, 5);
  const uint8_t st = r.run(0xA8);  // never load the data register
  CHECK((st & 0x04) != 0);
  CHECK((st & 1) == 0);
  CHECK_FALSE(r.disk.dirty());
}

TEST_CASE("wd1772 read address returns track, side, sector, size code and CRC") {
  Rig r;
  r.seek(3);
  std::array<bool, 6> seen{};
  int prev = -1;
  for (int n = 0; n < 12; ++n) {
    r.fdc.write(0, 0xC8);  // Read Address, h=1
    const auto id = r.readBytes(6);
    REQUIRE(id.size() == 6);
    r.runIdle();
    const uint8_t st = r.fdc.read(0);
    CHECK((st & 0x1D) == 0);
    CHECK(id[0] == 3);
    CHECK(id[1] == 0);
    REQUIRE(id[2] < 6);
    CHECK(id[3] == (id[2] < 5 ? 3 : 2));  // 1024 or 512 bytes
    const uint16_t crc = crcCcitt({0xA1, 0xA1, 0xA1, 0xFE, id[0], id[1], id[2], id[3]});
    CHECK(id[4] == uint8_t(crc >> 8));
    CHECK(id[5] == uint8_t(crc & 0xFF));
    CHECK(r.fdc.read(2) == 3);  // the track number is copied into the sector register
    seen[id[2]] = true;
    // Sectors pass under the head in order 0..5.
    if (prev >= 0) CHECK(id[2] == (prev + 1) % 6);
    prev = id[2];
  }
  for (bool s : seen) CHECK(s);
}

TEST_CASE("wd1772 read address on an unformatted track is Record Not Found") {
  Rig r;
  r.disk = DiskImage::blank();
  const uint8_t st = r.run(0xC8);
  CHECK((st & 0x10) != 0);
}

TEST_CASE("wd1772 multi-sector read runs through sector 5 and ends with RNF") {
  Rig r;
  r.seek(3);
  r.fdc.write(2, 0);
  r.fdc.write(0, 0x98);  // Read Sector, m=1, h=1
  const auto got = r.readBytes(5 * 1024 + 512);
  REQUIRE(got.size() == size_t(5 * 1024 + 512));
  r.runIdle();
  CHECK(r.fdc.sectorsRead() == 6);
  const uint8_t st = r.fdc.read(0);
  CHECK((st & 0x10) != 0);  // sector 6 does not exist
  CHECK(r.fdc.read(2) == 6);
  // Sector 2's bytes are the pattern.
  for (size_t i = 0; i < 1024; ++i) REQUIRE(got[2 * 1024 + i] == uint8_t(i & 0xFF));
}

TEST_CASE("wd1772 reads sectors 0-5 of a track in under two revolutions back to back") {
  // Sector slots sit at cumulative byte offsets (ID + gaps + data), so the next
  // sector's ID field is still ahead of the head when the OS issues its next
  // command a NMI-handler's worth (kOsLatency) after INTRQ.
  constexpr uint32_t kOsLatency = 200;
  Rig r;
  r.seek(3);
  uint64_t cycles = 0;
  for (int sector = 0; sector < 6; ++sector) {
    r.fdc.write(2, uint8_t(sector));
    r.fdc.write(0, 0x88);  // Read Sector, h=1
    while (!r.fdc.intrq()) {
      if (r.fdc.drq()) (void)r.fdc.read(3);
      r.fdc.tick(1);
      ++cycles;
      if (cycles >= 8ull * kRev) FAIL("sector " << sector << " never completed");
    }
    const uint8_t st = r.fdc.read(0);  // clears INTRQ
    REQUIRE((st & 0x5D) == 0);         // no RNF, CRC error or lost data
    if (sector < 5) {
      r.fdc.tick(kOsLatency);
      cycles += kOsLatency;
    }
  }
  CHECK(r.fdc.sectorsRead() == 6);
  CHECK(cycles < 2ull * kRev);
}

TEST_CASE("wd1772 no disk: Read Sector never hangs the host and stays busy; no index pulses") {
  Rig r;
  r.fdc.setDisk(nullptr);
  r.fdc.write(2, 2);
  r.fdc.write(0, 0x88);
  // Without a disk there are no index pulses, so the chip waits for ever
  // (datasheet: the 5-revolution timeout counts index pulses).
  REQUIRE_NOTHROW(r.fdc.tick(20 * kRev));
  CHECK(r.fdc.busy());
  CHECK_FALSE(r.fdc.intrq());
  CHECK(r.fdc.read(0) == 0x01);  // busy, nothing else
  // Force Interrupt frees the chip.
  r.fdc.write(0, 0xD0);
  CHECK_FALSE(r.fdc.busy());
  CHECK_FALSE(r.fdc.intrq());
}

TEST_CASE("wd1772 no disk: the command resumes if a disk is inserted") {
  Rig r;
  r.seek(3);
  r.fdc.setDisk(nullptr);
  r.fdc.write(2, 2);
  r.fdc.write(0, 0x88);
  r.fdc.tick(3 * kRev);
  REQUIRE(r.fdc.busy());
  r.fdc.setDisk(&r.disk);
  const auto got = r.readBytes(1024);
  REQUIRE(got.size() == 1024);
  CHECK(got[77] == 77);
}

TEST_CASE("wd1772 no disk: a Type I step without verify still completes") {
  Rig r;
  r.fdc.setDisk(nullptr);
  const uint8_t st = r.run(0x58);
  CHECK((st & 1) == 0);
  CHECK(r.fdc.headTrack() == 1);
}

TEST_CASE("wd1772 motor off: the disk does not turn, so a command waits until the motor runs") {
  Rig r;
  r.seek(3);
  r.fdc.setMotor(false);
  r.fdc.write(2, 2);
  r.fdc.write(0, 0x88);
  r.fdc.tick(10 * kRev);
  CHECK(r.fdc.busy());
  CHECK_FALSE(r.fdc.drq());
  r.fdc.setMotor(true);
  const auto got = r.readBytes(1024);
  REQUIRE(got.size() == 1024);
  CHECK(got[200] == 200);
}

TEST_CASE("wd1772 eject in the middle of a read aborts with Record Not Found") {
  Rig r;
  r.seek(3);
  r.fdc.write(2, 2);
  r.fdc.write(0, 0x88);
  const auto part = r.readBytes(100);
  REQUIRE(part.size() == 100);
  REQUIRE_NOTHROW(r.fdc.setDisk(nullptr));
  CHECK_FALSE(r.fdc.busy());
  CHECK(r.fdc.intrq());
  CHECK_FALSE(r.fdc.drq());
  REQUIRE_NOTHROW(r.fdc.tick(2 * kRev));
  const uint8_t st = r.fdc.read(0);
  CHECK((st & 0x10) != 0);
  CHECK((st & 1) == 0);
}

TEST_CASE("wd1772 eject in the middle of a write leaves the image untouched") {
  Rig r;
  r.seek(3);
  r.fdc.write(2, 5);
  r.fdc.write(0, 0xA8);
  size_t sent = 0;
  for (uint64_t guard = 0; sent < 100 && guard < 4ull * kRev; ++guard) {
    if (r.fdc.drq()) r.fdc.write(3, 0xEE), ++sent;
    else r.fdc.tick(1);
  }
  REQUIRE(sent == 100);
  r.fdc.setDisk(nullptr);
  CHECK_FALSE(r.fdc.busy());
  CHECK((r.fdc.read(0) & 0x10) != 0);
  CHECK_FALSE(r.disk.dirty());
}

TEST_CASE("wd1772 disk change latch is set by insert and cleared by ack") {
  Wd1772 fdc;
  CHECK_FALSE(fdc.diskChanged());
  DiskImage d = makeDisk();
  fdc.setDisk(&d);
  CHECK(fdc.diskChanged());
  fdc.ackDiskChange();
  CHECK_FALSE(fdc.diskChanged());
  fdc.setDisk(&d);  // same disk again: no change
  CHECK_FALSE(fdc.diskChanged());
  fdc.setDisk(nullptr);
  CHECK(fdc.diskChanged());
}

TEST_CASE("wd1772 force interrupt $D0 ends a command without INTRQ") {
  Rig r;
  r.seek(3);
  r.fdc.write(2, 2);
  r.fdc.write(0, 0x88);
  r.readBytes(10);
  r.fdc.write(0, 0xD0);
  CHECK_FALSE(r.fdc.busy());
  CHECK_FALSE(r.fdc.intrq());
  CHECK_FALSE(r.fdc.drq());
  r.fdc.tick(kRev);
  CHECK_FALSE(r.fdc.intrq());
  CHECK_FALSE(r.fdc.drq());
  CHECK((r.fdc.read(0) & 1) == 0);
}

TEST_CASE("wd1772 force interrupt $D8 raises INTRQ until $D0") {
  Rig r;
  r.fdc.write(0, 0xD8);
  CHECK(r.fdc.intrq());
  r.fdc.read(0);  // a status read does not clear an immediate interrupt
  CHECK(r.fdc.intrq());
  r.fdc.write(0, 0xD0);
  CHECK_FALSE(r.fdc.intrq());
}

TEST_CASE("wd1772 force interrupt while idle gives Type I status") {
  Rig r;
  r.fdc.write(2, 6);
  r.run(0x88);  // RNF
  r.fdc.write(0, 0xD0);
  const uint8_t st = r.fdc.read(0);
  CHECK((st & 0x10) == 0);
  CHECK((st & 0x04) != 0);  // head is at track 0
}

TEST_CASE("wd1772 a command write clears INTRQ and a busy chip ignores commands") {
  Rig r;
  r.fdc.write(0, 0x58);
  r.runIdle();
  CHECK(r.fdc.intrq());
  r.fdc.write(0, 0x58);
  CHECK_FALSE(r.fdc.intrq());
  CHECK(r.fdc.busy());
  r.fdc.write(0, 0x78);  // ignored: busy
  r.fdc.write(1, 99);    // ignored: busy
  r.runIdle();
  CHECK(r.fdc.headTrack() == 2);
  CHECK(r.fdc.read(1) == 2);  // u=1 counted both steps; the write of 99 was ignored
}

TEST_CASE("wd1772 spin-up waits for 6 index pulses unless h=1") {
  Rig r;
  r.fdc.write(0, 0x00);  // Restore, h=0, motor not yet running
  r.fdc.tick(5 * kRev);
  CHECK(r.fdc.busy());
  CHECK((r.fdc.read(0) & 0x80) != 0);  // MO
  CHECK((r.fdc.read(0) & 0x20) == 0);  // spin-up not complete
  r.runIdle();
  const uint8_t st = r.fdc.read(0);
  CHECK((st & 1) == 0);
  CHECK((st & 0x20) != 0);  // spin-up complete
  // The motor is now "on": the next command runs at once.
  r.fdc.write(0, 0x40);  // Step In, h=0
  const uint64_t took = r.runIdle();
  CHECK(took < 20000);
}

TEST_CASE("wd1772 h=1 skips spin-up and E adds a 15 ms settle") {
  Rig r;
  r.fdc.write(0, 0xE8);  // Read Track, h=1, E=0: completes at once
  CHECK(r.runIdle() < 1000);
  r.fdc.read(0);
  r.fdc.write(0, 0xEC);  // E=1: 15 ms = 30000 cycles of head settle first
  const uint64_t took = r.runIdle();
  CHECK(took >= 30000);
  CHECK(took < 31000);
}

TEST_CASE("wd1772 Read Track and Write Track complete without data or error") {
  Rig r;
  r.seek(3);
  for (uint8_t cmd : {uint8_t(0xE8), uint8_t(0xF8)}) {
    const uint8_t st = r.run(cmd);
    CHECK((st & 0x5D) == 0);
    CHECK_FALSE(r.fdc.drq());
  }
  CHECK_FALSE(r.disk.dirty());
}

TEST_CASE("wd1772 save/load mid-read resumes identically") {
  Rig r;
  r.seek(3);
  r.fdc.write(2, 2);
  r.fdc.write(0, 0x88);
  r.readBytes(300);

  StateWriter w;
  r.fdc.save(w);
  const auto snap = w.take();

  Rig other;
  StateReader rd(snap);
  other.fdc.load(rd);
  CHECK(rd.atEnd());
  StateWriter w2;
  other.fdc.save(w2);
  CHECK(w2.take() == snap);

  const auto a = r.readBytes(724);
  const auto b = other.readBytes(724);
  REQUIRE(a.size() == 724);
  CHECK(a == b);
  CHECK(a.front() == uint8_t(300 & 0xFF));
  r.runIdle();
  other.runIdle();
  CHECK(r.fdc.read(0) == other.fdc.read(0));
}

TEST_CASE("wd1772 load rejects corrupt state and leaves the chip unchanged") {
  Rig r;
  r.seek(3);
  StateWriter w;
  r.fdc.save(w);
  auto snap = w.take();
  StateWriter before;
  r.fdc.save(before);
  const auto orig = before.take();

  auto truncated = std::vector<uint8_t>(snap.begin(), snap.end() - 3);
  StateReader tr(truncated);
  CHECK_THROWS_AS(r.fdc.load(tr), StateError);
  auto badTag = snap;
  badTag[0] ^= 0xFF;
  StateReader br(badTag);
  CHECK_THROWS_AS(r.fdc.load(br), StateError);

  StateWriter after;
  r.fdc.save(after);
  CHECK(after.take() == orig);
}

// ---- Machine wiring ----------------------------------------------------------

namespace {

std::vector<uint8_t> romFromBytes(const std::vector<uint8_t>& code) {
  std::vector<uint8_t> rom(4096, 0x12);
  for (size_t i = 0; i < code.size(); ++i) rom[i] = code[i];
  rom[0xFFE] = 0xF0;
  rom[0xFFF] = 0x00;
  return rom;
}

void put(std::vector<uint8_t>& rom, size_t at, std::initializer_list<uint8_t> bytes) {
  for (uint8_t b : bytes) rom[at++] = b;
}

// A miniature of the boot ROM's FDC usage (boot-rom.md section 5): DP=$E8,
// motor on via PB4=0, commands issued with `STA <$00; BRA *` and ended by the
// NMI handler (LEAS 12,S; LDB <$00; STB $8010; RTS), data moved with
// SYNC / LDA <$03 / STA ,X+ while I and F are masked.
std::vector<uint8_t> fdcRom() {
  std::vector<uint8_t> rom = romFromBytes({});
  put(rom, 0x000, {0x10, 0xCE, 0xBF, 0x80,  // LDS #$BF80
                   0x86, 0xE8, 0x1F, 0x8B,  // LDA #$E8; TFR A,DP
                   0x86, 0x1F, 0xB7, 0xE2, 0x02,  // DDRB = $1F
                   0x7F, 0xE2, 0x00,        // CLR $E200: PB4 = 0, motor on
                   0x86, 0x03, 0x97, 0x03,  // data register = 3
                   0x86, 0x18, 0xBD, 0xF0, 0x40,  // Seek (h=1), JSR cmd
                   0x86, 0x02, 0x97, 0x02,  // sector register = 2
                   0x8E, 0x81, 0x00,        // LDX #$8100
                   0x86, 0x88, 0xBD, 0xF0, 0x50,  // Read Sector (h=1), JSR rd
                   0x7C, 0x80, 0x20,        // INC $8020: done
                   0x20, 0xFE});            // BRA *
  put(rom, 0x040, {0x97, 0x00, 0x20, 0xFE});  // cmd: STA <$00; BRA *
  put(rom, 0x050, {0x97, 0x00,                // rd: STA <$00
                   0x13, 0x96, 0x03, 0xA7, 0x80, 0x20, 0xF9});  // SYNC; LDA <$03; STA ,X+; BRA
  put(rom, 0x080, {0x32, 0x6C, 0xD6, 0x00, 0xF7, 0x80, 0x10, 0x39});  // NMI handler
  rom[0xFFC] = 0xF0;
  rom[0xFFD] = 0x80;
  return rom;
}

}  // namespace

TEST_CASE("machine reads a sector with the boot ROM's SYNC/NMI usage pattern") {
  Machine m(fdcRom());
  m.insertDisk(makeDisk());
  m.powerOn();
  m.runCycles(2 * kRev);
  CHECK(m.peek(0x8020) == 1);
  CHECK((m.peek(0x8010) & 0x5C) == 0);
  for (int i = 0; i < 1024; ++i) REQUIRE(m.peek(uint16_t(0x8100 + i)) == uint8_t(i & 0xFF));
  CHECK(m.fdc().sectorsRead() == 1);
  CHECK(m.driveActive());
}

TEST_CASE("machine PB6 is the disk-present level and PB4 low is motor on") {
  // DDRB = $1F; ORB = $10 (motor off); read ORB -> $8000; ORB = 0; read -> $8001.
  std::vector<uint8_t> rom = romFromBytes({0x86, 0x1F, 0xB7, 0xE2, 0x02,
                                           0x86, 0x10, 0xB7, 0xE2, 0x00,
                                           0xB6, 0xE2, 0x00, 0xB7, 0x80, 0x00,
                                           0x7F, 0xE2, 0x00,
                                           0xB6, 0xE2, 0x00, 0xB7, 0x80, 0x01});
  Machine m(rom);
  m.powerOn();
  CHECK_FALSE(m.driveActive());
  m.runCycles(80);
  CHECK(m.peek(0x8000) == 0x10);  // no disk: PB6 = 0
  CHECK(m.driveActive());         // PB4 now low
  CHECK(m.peek(0x8001) == 0x00);

  m.insertDisk(makeDisk());
  m.powerOn();  // powerOn must not eject
  REQUIRE(m.disk() != nullptr);
  CHECK_FALSE(m.driveActive());
  m.runCycles(80);
  CHECK(m.peek(0x8000) == 0x50);  // PB6 = 1: disk in, a level
  CHECK(m.peek(0x8001) == 0x40);
  CHECK(m.driveActive());

  auto out = m.ejectDisk();
  REQUIRE(out != nullptr);
  CHECK(m.disk() == nullptr);
  CHECK(m.ejectDisk() == nullptr);
  m.powerOn();
  m.runCycles(80);
  CHECK(m.peek(0x8000) == 0x10);
}

TEST_CASE("machine powerOn leaves the inserted disk (and its dirty flag) alone") {
  Machine m(romFromBytes({}));
  DiskImage d = makeDisk();
  std::array<uint8_t, 512> s{};
  s[0] = 0x42;
  d.writeSector(1, 5, s);
  m.insertDisk(std::move(d));
  m.powerOn();
  REQUIRE(m.disk() != nullptr);
  CHECK(m.disk()->dirty());
  std::array<uint8_t, 512> back{};
  m.disk()->readSector(1, 5, back);
  CHECK(back[0] == 0x42);
}

TEST_CASE("machine ejecting during a read aborts the command and raises NMI") {
  Machine m(fdcRom());
  m.insertDisk(makeDisk());
  m.powerOn();
  m.runCycles(kRev / 4);  // seek done, probably searching or reading
  REQUIRE(m.peek(0x8020) == 0);
  auto out = m.ejectDisk();
  REQUIRE(out != nullptr);
  m.runCycles(1000);
  CHECK(m.peek(0x8020) == 1);        // the NMI handler ended the read
  CHECK((m.peek(0x8010) & 0x10) != 0);  // RNF
}

TEST_CASE("machine save/load round-trips the FDC and the disk, mid-command") {
  Machine m(fdcRom());
  m.insertDisk(makeDisk());
  m.powerOn();
  m.runCycles(kRev * 9 / 20);  // 180000: sector 2 is mid-transfer
  const auto snap = m.saveState();

  Machine other(fdcRom());
  other.powerOn();
  other.loadState(snap);
  REQUIRE(other.disk() != nullptr);
  CHECK(other.saveState() == snap);
  m.runCycles(2 * kRev);
  other.runCycles(2 * kRev);
  CHECK(other.saveState() == m.saveState());
  CHECK(other.peek(0x8020) == 1);
  for (int i = 0; i < 1024; ++i) REQUIRE(other.peek(uint16_t(0x8100 + i)) == uint8_t(i & 0xFF));

  // Loading a no-disk snapshot ejects the disk.
  Machine bare(fdcRom());
  bare.powerOn();
  const auto bareSnap = bare.saveState();
  other.loadState(bareSnap);
  CHECK(other.disk() == nullptr);
  other.loadState(snap);
  CHECK(other.disk() != nullptr);
}

TEST_CASE("machine loadState with a corrupt FDC section leaves the machine unchanged") {
  Machine m(fdcRom());
  m.insertDisk(makeDisk());
  m.powerOn();
  m.runCycles(1000);
  const auto before = m.saveState();
  auto bad = before;
  bad.resize(bad.size() - 100);  // chops into the panel section
  CHECK_THROWS_AS(m.loadState(bad), StateError);
  CHECK(m.saveState() == before);
  REQUIRE(m.disk() != nullptr);
}

TEST_CASE("machine insertDisk returns the previous disk") {
  Machine m(romFromBytes({}));
  CHECK(m.insertDisk(makeDisk()) == nullptr);  // empty drive: nothing replaced
  DiskImage* first = m.disk();
  REQUIRE(first != nullptr);

  auto second = std::make_unique<DiskImage>(DiskImage::blank());
  DiskImage* secondRaw = second.get();
  auto prev = m.insertDisk(std::move(second));  // pointers swap, no copy
  REQUIRE(prev != nullptr);
  CHECK(prev.get() == first);
  CHECK(prev->trackFormatted(3));
  CHECK(m.disk() == secondRaw);
  CHECK(m.fdc().diskChanged());

  // A null argument acts as an eject.
  auto last = m.insertDisk(std::unique_ptr<DiskImage>{});
  CHECK(last.get() == secondRaw);
  CHECK(m.disk() == nullptr);
}

TEST_CASE("machine ejectDisk on an empty drive returns null") {
  Machine m(romFromBytes({}));
  CHECK(m.ejectDisk() == nullptr);
  CHECK(m.disk() == nullptr);
  const Machine& cm = m;
  CHECK(cm.disk() == nullptr);
}
