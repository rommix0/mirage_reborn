// Versioned whole-machine snapshots and the determinism guarantee (Task 13).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "asset_paths.h"
#include "booted_machine.h"
#include "mirage/clocks.h"
#include "mirage/machine.h"
#include "mirage/state_io.h"

using namespace mirage;
using mirage_test::bootedMachine;
using mirage_test::bootedSnapshot;
using mirage_test::bootRomBytes;

namespace {

constexpr uint64_t kSecond = kCpuClockHz;
constexpr double kHost = 48000;
constexpr const char* kImage = "SND-A01.img";

// A ROM that loops on BRA *: no assets needed.
std::vector<uint8_t> idleRom() {
  std::vector<uint8_t> rom(4096, 0x12);
  rom[0] = 0x20;
  rom[1] = 0xFE;
  rom[0xFFE] = 0xF0;
  rom[0xFFF] = 0x00;
  return rom;
}

void setU32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i));
}
uint32_t getU32(const std::vector<uint8_t>& b, size_t at) {
  return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 |
         uint32_t(b[at + 3]) << 24;
}

void setU64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
  for (int i = 0; i < 8; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i));
}

// The offsets of the MCHN section's fields: the 28-byte header (magic, version,
// SHA-1), the "MCHN" tag, then u8 bank, u64 cycles, u32 credit.
constexpr size_t kCreditAt = 28 + 4 + 1 + 8;

// The one place in the blob that holds `value` as a little-endian u64.
size_t findU64(const std::vector<uint8_t>& b, uint64_t value) {
  std::array<uint8_t, 8> needle{};
  for (int i = 0; i < 8; ++i) needle[size_t(i)] = uint8_t(value >> (8 * i));
  size_t found = size_t(-1), hits = 0;
  for (auto it = b.begin(); (it = std::search(it, b.end(), needle.begin(), needle.end())) != b.end(); ++it) {
    found = size_t(it - b.begin());
    ++hits;
  }
  REQUIRE(hits == 1);
  return found;
}

void note(Machine& m, uint8_t status, uint8_t key, uint8_t vel) {
  const uint8_t msg[] = {status, key, vel};
  m.queueMidi(msg);
}

// 2 s at kHost: note-on at the start, note-off at 1 s.
std::vector<float> renderNote(Machine& m) {
  std::vector<float> v(size_t(2 * kHost));
  note(m, 0x90, 60, 100);
  m.render(v.data(), size_t(kHost), kHost);
  note(m, 0x80, 60, 0);
  m.render(v.data() + size_t(kHost), size_t(kHost), kHost);
  return v;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

std::string imagePath() { return std::string("Factory Library/IMG/") + kImage; }

}  // namespace

TEST_CASE("snapshot: header is the MIRG magic, then version 4") {
  Machine m(idleRom());
  m.powerOn();
  m.runCycles(1000);
  const auto snap = m.saveState();
  REQUIRE(snap.size() > 8);
  CHECK(getU32(snap, 0) == fourcc("MIRG"));
  CHECK(getU32(snap, 4) == Machine::kSnapshotVersion);
  CHECK(Machine::kSnapshotVersion == 4);
}

TEST_CASE("snapshot with an out-of-range credit is rejected and the machine is unchanged") {
  Machine src(idleRom());
  src.powerOn();
  src.runCycles(1234);
  const auto snap = src.saveState();
  CHECK(getU32(snap, kCreditAt) <= 64);

  Machine m(idleRom());
  m.powerOn();
  m.runCycles(77);
  const auto before = m.saveState();
  const auto cycles = m.cycles();
  for (uint32_t credit : {65u, 1000u, 0xFFFFFFFFu}) {
    INFO("credit " << credit);
    auto bad = snap;
    setU32(bad, kCreditAt, credit);
    CHECK_THROWS_AS(m.loadState(bad), StateError);
    CHECK(m.saveState() == before);
    CHECK(m.cycles() == cycles);
  }
  auto edge = snap;
  setU32(edge, kCreditAt, 64);  // the largest credit is still accepted
  m.loadState(edge);
}

TEST_CASE("snapshot with a far-future next-MIDI cycle is rejected") {
  Machine src(idleRom());
  src.powerOn();
  src.runCycles(5000);
  const uint64_t cycles = src.cycles();
  note(src, 0x90, 60, 100);
  const auto snap = src.saveState();
  const size_t at = findU64(snap, cycles + 640);  // the first byte is due 640 cycles on

  Machine m(idleRom());
  m.powerOn();
  m.runCycles(77);
  const auto before = m.saveState();
  const auto rejects = [&](uint64_t next) {
    INFO("nextMidiCycle " << next);
    auto bad = snap;
    setU64(bad, at, next);
    CHECK_THROWS_AS(m.loadState(bad), StateError);
    CHECK(m.saveState() == before);
  };
  rejects(cycles + 641);
  rejects(cycles + 1000000);
  rejects(0xFFFFFFFFFFFFFFFFull);
  rejects(cycles - 64);  // behind by a whole 64 cycles: no instruction takes that long
  rejects(0);

  // The ends of the legal range load.
  for (uint64_t next : {cycles - 63, cycles, cycles + 640}) {
    INFO("nextMidiCycle " << next);
    auto ok = snap;
    setU64(ok, at, next);
    m.loadState(ok);
    CHECK(m.midiQueueDepth() == 3);
  }

  // With nothing queued the field is a stale leftover and is not checked.
  note(src, 0x80, 60, 0);
  src.flushMidi();
  auto stale = src.saveState();
  setU64(stale, findU64(stale, cycles + 640), 0xFFFFFFFFFFFFFFFFull);
  m.loadState(stale);
  CHECK(m.midiQueueDepth() == 0);
}

TEST_CASE("snapshot: every state a running machine reaches with MIDI queued loads back") {
  // The loadState bounds on credit and the next-MIDI cycle must never reject a
  // snapshot the machine itself wrote: queue at random moments (idle line and
  // busy line), run for random spans, and round-trip each time.
  Machine m(idleRom());
  Machine other(idleRom());
  m.powerOn();
  uint32_t seed = 12345;
  const auto rnd = [&seed](uint32_t n) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % n;
  };
  for (int i = 0; i < 400; ++i) {
    if (rnd(3) == 0) {
      std::vector<uint8_t> bytes(1 + rnd(4), 0xF8);
      m.queueMidi(bytes);
    }
    m.runCycles(1 + rnd(1500));
    const auto snap = m.saveState();
    other.loadState(snap);
    REQUIRE(other.saveState() == snap);
  }
}

TEST_CASE("snapshot: the ROM's SHA-1 follows the version, and a different ROM is rejected") {
  Machine src(idleRom());
  src.powerOn();
  src.runCycles(1234);
  const auto snap = src.saveState();
  REQUIRE(snap.size() > 28);
  const auto id = src.romSha1();
  CHECK(std::equal(id.begin(), id.end(), snap.begin() + 8));
  CHECK(getU32(snap, 28) == fourcc("MCHN"));  // the first section follows the hash

  auto other = idleRom();
  other[0x800] = 0x55;  // a different ROM, same size
  Machine m(other);
  CHECK(m.romSha1() != src.romSha1());
  m.powerOn();
  m.runCycles(77);
  const auto before = m.saveState();
  try {
    m.loadState(snap);
    FAIL("expected StateError");
  } catch (const StateError& e) {
    CHECK(std::string(e.what()) == "snapshot was taken with a different boot ROM");
  }
  CHECK(m.saveState() == before);
  m.runCycles(10);

  // A single flipped hash byte is enough.
  auto bad = snap;
  bad[8 + 19] ^= 1;
  CHECK_THROWS_AS(src.loadState(bad), StateError);
}

TEST_CASE("snapshot: wrong magic, unknown version, truncation and trailing bytes are rejected") {
  Machine src(idleRom());
  src.powerOn();
  src.runCycles(1234);
  note(src, 0x90, 60, 100);
  const auto snap = src.saveState();

  Machine m(idleRom());
  m.powerOn();
  m.runCycles(77);
  const auto before = m.saveState();
  const auto cycles = m.cycles();

  const auto rejects = [&](std::vector<uint8_t> bad) {
    CHECK_THROWS_AS(m.loadState(bad), StateError);
    CHECK(m.saveState() == before);
    CHECK(m.cycles() == cycles);
  };
  for (uint32_t version : {0u, 1u, 2u, 3u, 5u, 99u, 0xFFFFFFFFu}) {
    INFO("version " << version);
    auto bad = snap;
    setU32(bad, 4, version);
    rejects(bad);
  }
  auto badMagic = snap;
  setU32(badMagic, 0, fourcc("MCHN"));  // the pre-versioned layout's first tag
  rejects(badMagic);
  rejects({});
  rejects(std::vector<uint8_t>(snap.begin(), snap.begin() + 4));  // magic only
  rejects(std::vector<uint8_t>(snap.begin(), snap.begin() + 8));  // header only
  auto trailing = snap;
  trailing.push_back(0);
  rejects(trailing);

  CHECK_NOTHROW(m.loadState(snap));
  CHECK(m.saveState() == snap);
}

TEST_CASE("snapshot: a version-99 blob throws and leaves a booted machine unchanged", "[assets]") {
  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));
  REQUIRE_ASSET(assetPath(imagePath()));
  auto m = bootedMachine(kImage);
  const auto snap = m->saveState();
  const DisplayState d = m->display();
  REQUIRE((d.left || d.right));  // a lit display makes "unchanged" meaningful
  const auto cycles = m->cycles();

  auto bad = snap;
  setU32(bad, 4, 99);
  CHECK_THROWS_AS(m->loadState(bad), StateError);
  CHECK(m->display().left == d.left);
  CHECK(m->display().right == d.right);
  CHECK(m->cycles() == cycles);
  CHECK(m->saveState() == snap);
}

TEST_CASE("snapshot: determinism, a restored machine renders the same bits", "[assets]") {
  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));
  REQUIRE_ASSET(assetPath(imagePath()));
  auto a = bootedMachine(kImage);
  const auto snap = a->saveState();
  CHECK(snap == bootedSnapshot(kImage).state);  // load then save is the identity

  const std::vector<float> bufA = renderNote(*a);
  double energy = 0;
  for (float x : bufA) energy += double(x) * x;
  INFO("rms " << std::sqrt(energy / double(bufA.size())));
  REQUIRE(std::sqrt(energy / double(bufA.size())) > 0.003);  // a real note, not silence

  Machine b(bootRomBytes());  // fresh instance, same ROM
  b.loadState(snap);
  const std::vector<float> bufB = renderNote(b);

  CHECK(sameBits(bufA, bufB));
  CHECK(a->cycles() == b.cycles());
  CHECK(a->saveState() == b.saveState());
}

TEST_CASE("snapshot: a mid-boot FDC command, held key and queued MIDI all resume exactly",
          "[assets]") {
  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));
  REQUIRE_ASSET(assetPath(imagePath()));
  // Power on and stop while the OS is loading the sound: motor on, a command
  // in flight and several sectors already read.
  Machine a(bootRomBytes());
  a.insertDisk(DiskImage::loadFile(assetPath(imagePath())));
  a.powerOn();
  a.setKey(PanelKey::K1, true);  // a finger on a key
  bool inFlight = false;
  for (int ms = 0; ms < 20000 && !inFlight; ++ms) {
    a.runCycles(kSecond / 1000);
    inFlight = a.driveActive() && a.fdc().busy() && a.fdc().sectorsRead() >= 4;
  }
  REQUIRE(inFlight);
  note(a, 0x90, 64, 90);  // still in the queue when the snapshot is taken
  const auto snap = a.saveState();

  Machine b(bootRomBytes());
  b.loadState(snap);
  CHECK(b.saveState() == snap);
  CHECK(b.driveActive());
  CHECK(b.fdc().busy());

  const uint64_t sectors = a.fdc().sectorsRead();
  for (int i = 0; i < 6; ++i) {
    a.runCycles(kSecond / 2);
    b.runCycles(kSecond / 2);
    REQUIRE(a.saveState() == b.saveState());
  }
  CHECK(a.fdc().sectorsRead() > sectors);  // the command really carried on
  CHECK(a.display().left == b.display().left);
  CHECK(a.display().right == b.display().right);
}

TEST_CASE("snapshot: a dirty disk stays dirty, with its contents, across a round trip") {
  Machine m(idleRom());
  m.insertDisk(DiskImage::fromBytes(std::vector<uint8_t>(DiskImage::kImageBytes, 0)));
  m.powerOn();
  std::vector<uint8_t> sector(1024);
  for (size_t i = 0; i < sector.size(); ++i) sector[i] = uint8_t(i * 7 + 3);
  // The OS's disk writes arrive through the same DiskImage::writeSector.
  m.disk()->writeSector(5, 2, sector);
  REQUIRE(m.disk()->dirty());

  Machine other(idleRom());
  other.loadState(m.saveState());
  REQUIRE(other.disk() != nullptr);
  CHECK(other.disk()->dirty());
  std::vector<uint8_t> back(1024);
  other.disk()->readSector(5, 2, back);
  CHECK(back == sector);

  // A clean disk stays clean, and "no disk" stays "no disk".
  m.disk()->clearDirty();
  other.loadState(m.saveState());
  CHECK_FALSE(other.disk()->dirty());
  Machine empty(idleRom());
  empty.powerOn();
  other.loadState(empty.saveState());
  CHECK(other.disk() == nullptr);
}

namespace {

// Formats track 0 the way a formatter would: motor on (PB4 = 0), Write Track
// ($F8, h=1) with DP = $E8, then SYNC / LDA ,X+ / STA <$03 for every DRQ from
// a stream at $8200. The NMI handler counts the INTRQ at $8020.
std::vector<uint8_t> writeTrackRom() {
  std::vector<uint8_t> rom = idleRom();
  const uint8_t code[] = {0x10, 0xCE, 0xBF, 0x80,        // LDS #$BF80
                          0x86, 0xE8, 0x1F, 0x8B,        // LDA #$E8; TFR A,DP
                          0x86, 0x1F, 0xB7, 0xE2, 0x02,  // DDRB = $1F
                          0x7F, 0xE2, 0x00,              // CLR $E200: motor on
                          0x8E, 0x82, 0x00,              // LDX #$8200
                          0x86, 0xF8, 0x97, 0x00,        // Write Track
                          // SYNC; LDA ,X+; STA <$03; BRA
                          0x13, 0xA6, 0x80, 0x97, 0x03, 0x20, 0xF9};
  std::copy(std::begin(code), std::end(code), rom.begin());
  const uint8_t nmi[] = {0x7C, 0x80, 0x20, 0x20, 0xFE};  // INC $8020; BRA *
  std::copy(std::begin(nmi), std::end(nmi), rom.begin() + 0x80);
  rom[0xFFC] = 0xF0;
  rom[0xFFD] = 0x80;
  return rom;
}

// The Mirage layout on track 0, every data byte `fill` (test_wd1772.cpp has the
// layout spelled out).
std::vector<uint8_t> mirageTrack0(uint8_t fill) {
  std::vector<uint8_t> s(16, 0x4E);
  for (uint8_t id = 0; id < 6; ++id) {
    s.insert(s.end(), 12, 0x00);
    s.insert(s.end(), {0xF5, 0xF5, 0xF5, 0xFE, 0x00, 0x00, id, uint8_t(id < 5 ? 3 : 2), 0xF7});
    s.insert(s.end(), 22, 0x4E);
    s.insert(s.end(), 12, 0x00);
    s.insert(s.end(), {0xF5, 0xF5, 0xF5, 0xFB});
    s.insert(s.end(), id < 5 ? 1024u : 512u, fill);
    s.push_back(0xF7);
    s.insert(s.end(), 32, 0x4E);
  }
  s.insert(s.end(), 100, 0x4E);
  return s;
}

}  // namespace

TEST_CASE("snapshot round-trips a machine mid write track") {
  Machine a(writeTrackRom());
  a.insertDisk(DiskImage::blank());
  a.powerOn();
  const auto stream = mirageTrack0(0x3C);
  for (size_t i = 0; i < stream.size(); ++i) a.debugWrite(uint16_t(0x8200 + i), stream[i]);
  // The index pulse comes one revolution after the motor starts; stop half way
  // through the revolution that follows.
  a.runCycles(kDiskIndexCycles * 3 / 2);
  REQUIRE(a.fdc().busy());
  REQUIRE(a.peek(0x8020) == 0);
  const auto snap = a.saveState();

  Machine b(writeTrackRom());
  b.loadState(snap);
  CHECK(b.saveState() == snap);
  a.runCycles(kDiskIndexCycles);
  b.runCycles(kDiskIndexCycles);
  CHECK(a.saveState() == b.saveState());
  CHECK(b.peek(0x8020) == 1);
  REQUIRE(b.disk() != nullptr);
  CHECK(b.disk()->formattedTracks() == 1);
  std::vector<uint8_t> sec(1024);
  b.disk()->readSector(0, 4, sec);
  CHECK(std::all_of(sec.begin(), sec.end(), [](uint8_t v) { return v == 0x3C; }));
  CHECK_FALSE(a.fdc().busy());
}

TEST_CASE("snapshot: host callbacks are not state and survive a load") {
  Machine m(idleRom());
  m.powerOn();
  int mixes = 0;
  m.onMix = [&mixes](float) { ++mixes; };
  Machine src(idleRom());
  src.powerOn();
  src.runCycles(5000);
  m.loadState(src.saveState());
  m.runCycles(5000);
  CHECK(mixes > 0);
}

TEST_CASE("snapshot: size with sample RAM and a disk stays under 1 MB", "[assets]") {
  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));
  REQUIRE_ASSET(assetPath(imagePath()));
  const auto& snap = bootedSnapshot(kImage).state;
  CHECK(snap.size() > size_t(Machine::kSampleRamSize) + size_t(DiskImage::kImageBytes));
  CHECK(snap.size() < 1024 * 1024);
}
