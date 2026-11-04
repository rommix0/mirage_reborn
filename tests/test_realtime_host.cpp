// RealtimeHost (host/include/mirage/realtime_host.h): the audio thread owns the
// Machine; the UI and MIDI threads post through lock-free queues.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "alloc_counter.h"
#include "asset_paths.h"
#include "booted_machine.h"
#include "mirage/acia6850.h"
#include "mirage/clocks.h"
#include "mirage/machine.h"
#include "mirage/realtime_host.h"
#include "mirage/state_io.h"
#include "test_roms.h"

using namespace mirage;
using mirage_test::AllocationScope;
using mirage_test::kAciaReceiver;
using mirage_test::romFrom;
using mirage_test::romWith;

namespace {

// BRA *: a CPU that runs and does nothing else.
const std::vector<uint8_t> kIdleRom = romWith({0x20, 0xFE});

// A formatted disk with every byte `fill`.
std::unique_ptr<DiskImage> filledDisk(uint8_t fill) {
  return std::make_unique<DiskImage>(
      DiskImage::fromBytes(std::vector<uint8_t>(DiskImage::kImageBytes, fill)));
}

std::unique_ptr<DiskImage> blankDisk() { return std::make_unique<DiskImage>(DiskImage::blank()); }

std::unique_ptr<WaveWrite> waveWrite(uint32_t address, std::vector<uint8_t> bytes) {
  auto w = std::make_unique<WaveWrite>();
  w->address = address;
  w->bytes = std::move(bytes);
  return w;
}

// One block at 48 kHz into a scratch buffer; returns the buffer.
std::vector<float> block(RealtimeHost& host, size_t frames, double now = 0.0) {
  std::vector<float> buf(frames);
  host.process(buf.data(), frames, 48000.0, now);
  return buf;
}

}  // namespace

TEST_CASE("host starts powered on with an empty drive") {
  RealtimeHost host(kIdleRom);
  const HostStatus s = host.status();
  CHECK_FALSE(s.diskPresent);
  CHECK_FALSE(s.diskDirty);
  CHECK(s.cycles == 0);
  CHECK(host.machineForTest().disk() == nullptr);
  CHECK(host.machineForTest().cpu().regs().pc == 0xF000);  // at the reset vector
  CHECK(host.romSha1() == Machine(kIdleRom).romSha1());
  CHECK_FALSE(host.takeReturnedDisk().has_value());
}

TEST_CASE("host insert into an empty drive is immediate") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  auto d = filledDisk(1);
  DiskImage* raw = d.get();
  REQUIRE(host.postInsert(std::move(d)));
  CHECK(d == nullptr);
  block(host, 64);
  CHECK(host.machineForTest().disk() == raw);
  CHECK(host.status().diskPresent);
  CHECK_FALSE(host.takeReturnedDisk().has_value());
}

TEST_CASE("host rejects a null disk without taking anything") {
  RealtimeHost host(kIdleRom);
  CHECK_FALSE(host.postInsert(nullptr));
  CHECK_FALSE(host.postCopyDisk(nullptr));
}

TEST_CASE("host swap ejects, waits 0.5 s of emulated time, then inserts") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  auto a = filledDisk(1), b = filledDisk(2);
  DiskImage *rawA = a.get(), *rawB = b.get();
  REQUIRE(host.postInsert(std::move(a)));
  block(host, 64);
  REQUIRE(host.machineForTest().disk() == rawA);

  REQUIRE(host.postInsert(std::move(b)));
  const uint64_t ejectCycle = host.status().cycles;  // the eject happens before the next block runs
  block(host, 64);
  auto old = host.takeReturnedDisk();
  REQUIRE(old.has_value());
  CHECK(old->disk.get() == rawA);
  CHECK(old->why == DiskReturn::Ejected);
  CHECK_FALSE(host.takeReturnedDisk().has_value());

  // Absent over [eject, eject + 0.5 s), present from the first block that ends past it.
  int blocksAbsent = 1;
  for (;;) {
    const HostStatus s = host.status();
    if (s.cycles >= ejectCycle + RealtimeHost::kSwapGapCycles) {
      CHECK(s.diskPresent);
      CHECK(host.machineForTest().disk() == rawB);
      break;
    }
    CHECK_FALSE(s.diskPresent);
    CHECK(host.machineForTest().disk() == nullptr);
    block(host, 64);
    ++blocksAbsent;
  }
  CHECK(RealtimeHost::kSwapGapCycles == 1'000'000);
  // 1e6 cycles / (64 frames x 2e6 / 48000) = 375 blocks; one more if the
  // fractional cycle carry leaves the 375th a cycle short.
  CHECK(blocksAbsent >= 375);
  CHECK(blocksAbsent <= 376);
  CHECK_FALSE(host.takeReturnedDisk().has_value());
}

TEST_CASE("host second insert during the gap returns the first as Unused") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  auto a = filledDisk(1), b = filledDisk(2), c = filledDisk(3);
  DiskImage *rawA = a.get(), *rawB = b.get(), *rawC = c.get();
  REQUIRE(host.postInsert(std::move(a)));
  block(host, 64);
  REQUIRE(host.postInsert(std::move(b)));
  const uint64_t ejectCycle = host.status().cycles;
  block(host, 64);
  REQUIRE(host.postInsert(std::move(c)));
  block(host, 64);

  auto first = host.takeReturnedDisk();
  REQUIRE(first.has_value());
  CHECK(first->disk.get() == rawA);
  CHECK(first->why == DiskReturn::Ejected);
  auto second = host.takeReturnedDisk();
  REQUIRE(second.has_value());
  CHECK(second->disk.get() == rawB);
  CHECK(second->why == DiskReturn::Unused);
  CHECK_FALSE(host.takeReturnedDisk().has_value());

  // The gap still runs from the first eject.
  while (host.status().cycles < ejectCycle + RealtimeHost::kSwapGapCycles) {
    CHECK_FALSE(host.status().diskPresent);
    block(host, 64);
  }
  CHECK(host.machineForTest().disk() == rawC);
  CHECK(host.status().diskPresent);
}

TEST_CASE("host eject during the gap returns the pending disk as Unused") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  auto a = filledDisk(1), b = filledDisk(2);
  DiskImage* rawB = b.get();
  REQUIRE(host.postInsert(std::move(a)));
  block(host, 64);
  REQUIRE(host.postInsert(std::move(b)));
  block(host, 64);
  REQUIRE(host.takeReturnedDisk().has_value());  // a, Ejected
  REQUIRE(host.postEject());
  block(host, 64);
  auto pending = host.takeReturnedDisk();
  REQUIRE(pending.has_value());
  CHECK(pending->disk.get() == rawB);
  CHECK(pending->why == DiskReturn::Unused);
  for (int i = 0; i < 400; ++i) block(host, 64);  // past the gap: nothing goes in
  CHECK(host.machineForTest().disk() == nullptr);
  CHECK_FALSE(host.status().diskPresent);
  CHECK_FALSE(host.takeReturnedDisk().has_value());
}

TEST_CASE("host short key press is held for 100 ms") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  block(host, 64);
  const PanelKey k = PanelKey::Enter;
  REQUIRE(host.postKey(k, true));
  REQUIRE(host.postKey(k, false));
  const uint64_t pressCycle = host.status().cycles;
  CHECK(RealtimeHost::kMinKeyHoldCycles == 200'000);
  int blocksHeld = 0;
  for (int i = 0; i < 100; ++i) {
    block(host, 64);
    const bool due = host.status().cycles - pressCycle >= RealtimeHost::kMinKeyHoldCycles;
    CHECK(host.machineForTest().keyDown(k) == !due);
    if (!due) ++blocksHeld;
  }
  // 200000 / 2666.67 = 75 blocks: the 75th ends at the hold (or a cycle short of it).
  CHECK(blocksHeld >= 74);
  CHECK(blocksHeld <= 75);

  // A key already held for longer than the minimum is released at once.
  REQUIRE(host.postKey(PanelKey::K5, true));
  for (int i = 0; i < 80; ++i) block(host, 64);
  CHECK(host.machineForTest().keyDown(PanelKey::K5));
  REQUIRE(host.postKey(PanelKey::K5, false));
  block(host, 64);
  CHECK_FALSE(host.machineForTest().keyDown(PanelKey::K5));
}

TEST_CASE("host places MIDI by timestamp within the block") {
  // The receiver ROM reads each byte from the ACIA, so the moment a byte is
  // queued shows in the saved state (queue timing, ACIA, RAM, cycle count).
  const auto rom = romFrom({kAciaReceiver});
  RealtimeHost host(rom);
  Machine ref(rom);
  ref.powerOn();
  host.prepare(480, 48000);
  ref.prepare(480, 48000);
  std::vector<float> got(480), want(480);
  const double rate = 48000.0;
  double now = 1000.0;
  auto hostBlock = [&] {
    host.process(got.data(), 480, rate, now);
    now += 480 / rate;
  };
  for (int i = 0; i < 4; ++i) {
    hostBlock();
    ref.render(want.data(), 480, rate);
  }
  REQUIRE(host.machineForTest().saveState() == ref.saveState());

  const uint8_t on[] = {0x90, 60, 100}, off[] = {0x80, 60, 0}, cc[] = {0xB0, 1, 64};

  // Stamped 5 ms before `now`: 240 frames before the end of the block.
  REQUIRE(host.postMidi(on, now - 0.005));
  hostBlock();
  ref.render(want.data(), 240, rate);
  ref.queueMidi(on);
  ref.render(want.data() + 240, 240, rate);
  CHECK(host.machineForTest().saveState() == ref.saveState());
  CHECK(got == want);

  // Offsets never decrease: an event stamped earlier than the one before it
  // goes at the same offset (384), after it.
  REQUIRE(host.postMidi(off, now - 0.002));
  REQUIRE(host.postMidi(cc, now - 0.008));
  hostBlock();
  ref.render(want.data(), 384, rate);
  ref.queueMidi(off);
  ref.queueMidi(cc);
  ref.render(want.data() + 384, 96, rate);
  CHECK(host.machineForTest().saveState() == ref.saveState());

  // Stamped after `now` (the clocks disagree): the last frame, never beyond.
  REQUIRE(host.postMidi(on, now + 1.0));
  hostBlock();
  ref.render(want.data(), 479, rate);
  ref.queueMidi(on);
  ref.render(want.data() + 479, 1, rate);
  CHECK(host.machineForTest().saveState() == ref.saveState());

  // Stamped long ago (the audio stalled): the start of the block.
  REQUIRE(host.postMidi(off, now - 10.0));
  hostBlock();
  ref.queueMidi(off);
  ref.render(want.data(), 480, rate);
  CHECK(host.machineForTest().saveState() == ref.saveState());
}

TEST_CASE("host process allocates nothing after prepare", "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A02.img"));
  // Everything that allocates or frees happens outside the scopes: the host,
  // the disks, the buffers, and freeing the disks that come back.
  RealtimeHost host(mirage_test::bootRomBytes());
  constexpr size_t kMax = 4096;
  host.prepare(kMax, 48000);
  auto a01 = std::make_unique<DiskImage>(
      DiskImage::loadFile(assetPath("Factory Library/IMG/SND-A01.img")));
  auto a02 = std::make_unique<DiskImage>(
      DiskImage::loadFile(assetPath("Factory Library/IMG/SND-A02.img")));
  auto copyDest = blankDisk();
  DiskImage *rawA01 = a01.get(), *rawA02 = a02.get(), *rawCopy = copyDest.get();
  std::vector<ReturnedDisk> returned;
  std::vector<ReturnedWave> waves;
  auto busyWrite = waveWrite(0x10000, std::vector<uint8_t>(4096, 0x55));
  auto idleWrite = waveWrite(0x00100, std::vector<uint8_t>(4096, 0x66));
  WaveWrite *rawBusy = busyWrite.get(), *rawIdle = idleWrite.get();
  std::vector<float> buf(kMax), inBuf(kMax);  // inBuf: the audio input (a tone; unplugged now and then)
  for (size_t k = 0; k < kMax; ++k) inBuf[k] = 0.3f * float(std::sin(0.05 * double(k)));
  const uint8_t on[] = {0x90, 60, 100}, off[] = {0x80, 60, 0};
  std::vector<uint8_t> sysex(20, 0x01);  // non-commercial ID: the OS ignores it
  sysex.front() = 0xF0;
  sysex[1] = 0x7D;
  sysex.back() = 0xF7;
  size_t allocations = 0, frees = 0;
  uint64_t sectorsRead = 0;
  double now = 100.0;
  for (int i = 0; i < 100; ++i) {
    const double rate = i < 70 ? 48000.0 : 44100.0;  // the host rate changes between blocks
    switch (i) {
      case 0: REQUIRE(host.postInsert(std::move(a01))); break;  // empty drive: the ROM boots it
      case 10:
        REQUIRE(host.postKey(PanelKey::Cancel, true));
        REQUIRE(host.postKey(PanelKey::Cancel, false));  // deferred release
        break;
      case 15:
        REQUIRE(host.status().driveActive);  // the ROM is loading the OS: refused
        REQUIRE(host.postWaveWrite(std::move(busyWrite)));
        break;
      case 20: REQUIRE(host.postMidi(on, now - 0.01)); break;
      case 25: REQUIRE(host.postMidi(off, now - 0.03)); break;
      case 30: REQUIRE(host.postMidi(sysex, now - 0.02)); break;  // two commands
      case 40: REQUIRE(host.postInsert(std::move(a02))); break;   // swap: eject now, insert later
      case 60: REQUIRE(host.postCopyDisk(std::move(copyDest))); break;
      case 62:
        REQUIRE(returned.size() == 2);
        REQUIRE(host.postMarkSaved(returned[1].writeCount));
        break;
      case 80: REQUIRE(host.postEject()); break;
      case 85: REQUIRE(host.postPanic()); break;
      case 50: host.setInputGain(2.5f); break;
      case 90:
        sectorsRead = host.machineForTest().fdc().sectorsRead();  // powerOn() zeroes it
        REQUIRE(host.postReset());
        break;
      case 95:
        REQUIRE_FALSE(host.status().driveActive);  // after the reset, before the ROM looks for a disk
        REQUIRE(host.postWaveWrite(std::move(idleWrite)));
        break;
      default: break;
    }
    {
      AllocationScope scope;
      host.process(i % 9 == 8 ? nullptr : inBuf.data(), buf.data(), kMax, rate, now);
      allocations += scope.count();
      frees += scope.frees();
    }
    now += double(kMax) / rate;
    while (auto r = host.takeReturnedDisk()) returned.push_back(std::move(*r));
    while (auto r = host.takeReturnedWave()) waves.push_back(std::move(*r));
  }
  CHECK(allocations == 0);
  CHECK(frees == 0);
  // The paths really ran: the ROM read the disk, and every disk came back.
  CHECK(sectorsRead > 0);
  CHECK(host.status().cycles < 11 * kMax * kCpuClockHz / 44100);  // restarted at block 90
  REQUIRE(returned.size() == 3);
  CHECK(returned[0].disk.get() == rawA01);
  CHECK(returned[0].why == DiskReturn::Ejected);
  CHECK(returned[1].disk.get() == rawCopy);
  CHECK(returned[1].why == DiskReturn::Copy);
  CHECK(returned[2].disk.get() == rawA02);
  CHECK(returned[2].why == DiskReturn::Ejected);
  CHECK(returned[1].disk->toBytes() == returned[2].disk->toBytes());
  CHECK_FALSE(host.status().diskPresent);
  REQUIRE(waves.size() == 2);
  CHECK(waves[0].write.get() == rawBusy);
  CHECK(waves[0].why == WaveReturn::RefusedDriveBusy);
  CHECK(waves[1].write.get() == rawIdle);
  CHECK(waves[1].why == WaveReturn::Written);
}

TEST_CASE("host applies a wave write before the block and returns it Written") {
  // LDA $0005; STA $8000; BRA: the CPU copies wave RAM byte 5 (bank 0) all the time.
  RealtimeHost host(romWith({0xB6, 0x00, 0x05, 0xB7, 0x80, 0x00, 0x20, 0xF8}));
  host.prepare(64, 48000);
  block(host, 64);
  REQUIRE(host.machineForTest().peek(0x8000) == 0x00);

  auto low = waveWrite(0x00004, {0x11, 0x77, 0x33});
  auto high = waveWrite(0x1FFFE, {0xA1, 0xA2});  // the last two bytes of wave RAM
  const WaveWrite *rawLow = low.get(), *rawHigh = high.get();
  REQUIRE(host.postWaveWrite(std::move(low)));
  CHECK(low == nullptr);
  REQUIRE(host.postWaveWrite(std::move(high)));
  CHECK(host.machineForTest().sampleRam(0x00005) == 0x00);  // nothing before process()
  CHECK_FALSE(host.takeReturnedWave().has_value());

  block(host, 64);
  const Machine& m = host.machineForTest();
  CHECK(host.machineForTest().peek(0x8000) == 0x77);  // the CPU read it during the block
  CHECK(m.sampleRam(0x00003) == 0x00);
  CHECK(m.sampleRam(0x00004) == 0x11);
  CHECK(m.sampleRam(0x00005) == 0x77);
  CHECK(m.sampleRam(0x00006) == 0x33);
  CHECK(m.sampleRam(0x00007) == 0x00);
  CHECK(m.sampleRam(0x1FFFD) == 0x00);
  CHECK(m.sampleRam(0x1FFFE) == 0xA1);
  CHECK(m.sampleRam(0x1FFFF) == 0xA2);

  auto first = host.takeReturnedWave();
  REQUIRE(first.has_value());
  CHECK(first->write.get() == rawLow);
  CHECK(first->why == WaveReturn::Written);
  CHECK(first->write->bytes == std::vector<uint8_t>{0x11, 0x77, 0x33});
  auto second = host.takeReturnedWave();
  REQUIRE(second.has_value());
  CHECK(second->write.get() == rawHigh);
  CHECK(second->why == WaveReturn::Written);
  CHECK_FALSE(host.takeReturnedWave().has_value());
}

TEST_CASE("host rejects a null, oversized or queue-full wave write without taking it") {
  RealtimeHost host(kIdleRom);
  CHECK_FALSE(host.postWaveWrite(nullptr));
  auto past = waveWrite(0x1FFFF, {1, 2});  // one byte past the end of wave RAM
  CHECK_FALSE(host.postWaveWrite(std::move(past)));
  REQUIRE(past != nullptr);
  CHECK(past->bytes.size() == 2);
  auto far = waveWrite(0x20001, {});
  CHECK_FALSE(host.postWaveWrite(std::move(far)));
  CHECK(far != nullptr);
  CHECK(host.postWaveWrite(waveWrite(0x20000 - 4, {1, 2, 3, 4})));  // fits exactly

  for (size_t i = 1; i < RealtimeHost::kWaveQueueWrites; ++i)
    REQUIRE(host.postWaveWrite(waveWrite(0, {1})));
  auto extra = waveWrite(0, {9});
  CHECK_FALSE(host.postWaveWrite(std::move(extra)));
  CHECK(extra != nullptr);
}

TEST_CASE("host keeps wave writes queued while the return queue is full") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  constexpr size_t kN = RealtimeHost::kWaveQueueWrites;
  for (size_t i = 0; i < kN; ++i) REQUIRE(host.postWaveWrite(waveWrite(uint32_t(i), {1})));
  block(host, 64);  // all applied; the return queue is now full
  std::vector<const WaveWrite*> second;
  for (size_t i = 0; i < kN; ++i) {
    auto w = waveWrite(uint32_t(0x100 + i), {2});
    second.push_back(w.get());
    REQUIRE(host.postWaveWrite(std::move(w)));
  }
  block(host, 64);  // nowhere to return them: none is applied, none is freed
  CHECK(host.machineForTest().sampleRam(0x100) == 0x00);
  for (size_t i = 0; i < kN; ++i) {
    auto r = host.takeReturnedWave();
    REQUIRE(r.has_value());
    CHECK(r->write->address == i);
  }
  CHECK_FALSE(host.takeReturnedWave().has_value());
  block(host, 64);
  for (size_t i = 0; i < kN; ++i) {
    CHECK(host.machineForTest().sampleRam(uint32_t(0x100 + i)) == 0x02);
    auto r = host.takeReturnedWave();
    REQUIRE(r.has_value());
    CHECK(r->write.get() == second[i]);
    CHECK(r->why == WaveReturn::Written);
  }
}

TEST_CASE("host refuses a wave write while the drive is active and returns it untouched",
          "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  // Two hosts booting the same disk with the same blocks stay identical; only
  // one is sent the write, which must change nothing.
  RealtimeHost host(mirage_test::bootRomBytes()), twin(mirage_test::bootRomBytes());
  constexpr size_t kFrames = 1024;
  for (RealtimeHost* h : {&host, &twin}) {
    h->prepare(kFrames, 48000);
    REQUIRE(h->postInsert(std::make_unique<DiskImage>(
        DiskImage::loadFile(assetPath("Factory Library/IMG/SND-A01.img")))));
  }
  int blocks = 0;
  while (!host.status().driveActive && blocks < 2000) {
    block(host, kFrames);
    block(twin, kFrames);
    ++blocks;
  }
  REQUIRE(host.status().driveActive);

  auto w = waveWrite(0x08000, std::vector<uint8_t>(256, 0x55));
  const WaveWrite* raw = w.get();
  REQUIRE(host.postWaveWrite(std::move(w)));
  block(host, kFrames);
  block(twin, kFrames);
  CHECK(host.machineForTest().saveState() == twin.machineForTest().saveState());
  auto r = host.takeReturnedWave();
  REQUIRE(r.has_value());
  CHECK(r->write.get() == raw);
  CHECK(r->why == WaveReturn::RefusedDriveBusy);
  CHECK(r->write->address == 0x08000);
  CHECK(r->write->bytes == std::vector<uint8_t>(256, 0x55));
  CHECK_FALSE(host.takeReturnedWave().has_value());
}

TEST_CASE("host copy returns the disk contents and mark-saved clears dirty only when unchanged") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);

  // Nothing to copy: the buffer comes straight back.
  auto spare = blankDisk();
  DiskImage* rawSpare = spare.get();
  REQUIRE(host.postCopyDisk(std::move(spare)));
  block(host, 64);
  auto none = host.takeReturnedDisk();
  REQUIRE(none.has_value());
  CHECK(none->disk.get() == rawSpare);
  CHECK(none->why == DiskReturn::NoDiskToCopy);

  REQUIRE(host.postInsert(filledDisk(0x11)));
  block(host, 64);
  CHECK(host.status().diskWrites == 0);
  // The OS writes a sector (tests write it directly, between blocks).
  const std::vector<uint8_t> sector(1024, 0x5A);
  host.machineForTest().disk()->writeSector(3, 1, sector);
  block(host, 64);
  CHECK(host.status().diskDirty);
  CHECK(host.status().diskWrites == 1);

  REQUIRE(host.postCopyDisk(std::move(none->disk)));
  block(host, 64);
  auto copy = host.takeReturnedDisk();
  REQUIRE(copy.has_value());
  CHECK(copy->why == DiskReturn::Copy);
  CHECK(copy->disk.get() == rawSpare);
  CHECK(copy->writeCount == 1);
  CHECK(copy->disk->toBytes() == host.machineForTest().disk()->toBytes());
  std::vector<uint8_t> back(1024);
  copy->disk->readSector(3, 1, back);
  CHECK(back == sector);

  // Another write lands while the UI is saving the copy: the stale mark is ignored.
  host.machineForTest().disk()->writeSector(4, 2, sector);
  REQUIRE(host.postMarkSaved(copy->writeCount));
  block(host, 64);
  CHECK(host.status().diskDirty);
  CHECK(host.status().diskWrites == 2);
  CHECK(host.machineForTest().disk()->dirty());
  REQUIRE(host.postMarkSaved(2));
  block(host, 64);
  CHECK_FALSE(host.status().diskDirty);
  CHECK_FALSE(host.machineForTest().disk()->dirty());
}

TEST_CASE("host invalid rate writes silence but still applies an eject") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  auto d = filledDisk(1);
  DiskImage* raw = d.get();
  REQUIRE(host.postInsert(std::move(d)));
  block(host, 64);
  const uint64_t cycles = host.status().cycles;
  REQUIRE(cycles > 0);

  REQUIRE(host.postEject());
  std::vector<float> buf(64, 1.0f);
  host.process(buf.data(), 64, 0.0, 0.0);
  CHECK(buf == std::vector<float>(64, 0.0f));
  CHECK_FALSE(host.status().diskPresent);
  CHECK(host.status().cycles == cycles);
  auto out = host.takeReturnedDisk();
  REQUIRE(out.has_value());
  CHECK(out->disk.get() == raw);
  CHECK(out->why == DiskReturn::Ejected);

  std::fill(buf.begin(), buf.end(), 1.0f);
  host.process(buf.data(), 64, std::numeric_limits<double>::quiet_NaN(), 0.0);
  CHECK(buf == std::vector<float>(64, 0.0f));
  CHECK(host.status().cycles == cycles);
  CHECK(host.machineForTest().cycles() == cycles);
}

TEST_CASE("host invalid rate discards waiting MIDI instead of replaying it later") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  block(host, 64);
  const uint8_t on[] = {0x90, 60, 100};
  std::vector<uint8_t> sysex(30, 0x01);
  sysex.front() = 0xF0;
  sysex.back() = 0xF7;
  REQUIRE(host.postMidi(on, 0.0));
  REQUIRE(host.postMidi(sysex, 0.0));
  std::vector<float> buf(64);
  host.process(buf.data(), 64, 0.0, 0.0);  // the device has stopped
  CHECK(host.status().midiDropped == 33);
  block(host, 64, 0.0);  // the rate recovers: nothing stale arrives
  CHECK(host.machineForTest().midiQueueDepth() == 0);
  CHECK(host.status().midiDropped == 33);
  REQUIRE(host.postMidi(on, 0.0));  // new input still flows
  block(host, 64, 0.0);
  CHECK(host.machineForTest().midiQueueDepth() == 3);
}

TEST_CASE("host MIDI queue full drops and counts") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  const uint8_t note[] = {0x90, 60, 100};
  for (size_t i = 0; i < RealtimeHost::kMidiQueueCommands; ++i) REQUIRE(host.postMidi(note, 0.0));
  CHECK(host.status().midiDropped == 0);
  CHECK_FALSE(host.postMidi(note, 0.0));
  CHECK(host.status().midiDropped == 3);
  const std::vector<uint8_t> sysex(30, 0x01);
  CHECK_FALSE(host.postMidi(sysex, 0.0));  // all or nothing
  CHECK(host.status().midiDropped == 33);
  block(host, 64);  // drains 256 commands
  CHECK(host.postMidi(sysex, 0.0));
  CHECK(host.status().midiDropped == 33);
}

TEST_CASE("host drains at most 256 MIDI commands per block and splits SysEx") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  const uint8_t clock[] = {0xF8};
  for (int i = 0; i < 300; ++i) REQUIRE(host.postMidi(clock, 0.0));
  // Stamped at `now`: queued at the last frame, so no byte reaches the ACIA yet.
  block(host, 16, 0.0);
  CHECK(host.machineForTest().midiQueueDepth() == 256);
  block(host, 16, 0.0);
  const size_t depth = host.machineForTest().midiQueueDepth();
  CHECK(depth <= 300);
  CHECK(depth >= 298);

  // 30 bytes travel as 3 commands of up to 12 and arrive whole, in order.
  RealtimeHost h2(romFrom({kAciaReceiver}));
  h2.prepare(64, 48000);
  std::vector<uint8_t> sysex(30);
  for (size_t i = 0; i < sysex.size(); ++i) sysex[i] = uint8_t(0x10 + i);
  sysex.front() = 0xF0;
  sysex.back() = 0xF7;
  REQUIRE(h2.postMidi(sysex, 0.0));
  block(h2, 64, 0.0);
  CHECK(h2.machineForTest().midiQueueDepth() == 30);
  for (int i = 0; i < 400; ++i) block(h2, 64, 0.0);  // ~0.5 s, room for 30 x 320 us
  for (size_t i = 0; i < sysex.size(); ++i)
    CHECK(h2.machineForTest().peek(uint16_t(0x8000 + i)) == sysex[i]);
  CHECK(h2.machineForTest().peek(0x8000 + 30) == 0);
}

TEST_CASE("host panic flushes and sends CC123 on 16 channels") {
  RealtimeHost host(romFrom({kAciaReceiver}));
  host.prepare(64, 48000);
  block(host, 64);  // the receiver sets up the ACIA
  // Stale input in both places: 100 bytes waiting in the host's MIDI queue
  // (9 commands) and 20 already in the Machine's input queue.
  const std::vector<uint8_t> stale(100, 0xF8);
  REQUIRE(host.postMidi(stale, 0.0));
  host.machineForTest().queueMidi(std::span<const uint8_t>(stale).first(20));
  REQUIRE(host.postPanic());
  block(host, 16, 0.0);
  const size_t depth = host.machineForTest().midiQueueDepth();
  CHECK(depth <= 48);
  CHECK(depth >= 47);  // 16 frames = 667 cycles: at most one byte delivered
  CHECK(host.status().midiDropped == 0);  // a flush, not a loss
  for (int i = 0; i < 400; ++i) block(host, 64);
  for (int ch = 0; ch < 16; ++ch) {
    CHECK(host.machineForTest().peek(uint16_t(0x8000 + 3 * ch)) == 0xB0 + ch);
    CHECK(host.machineForTest().peek(uint16_t(0x8000 + 3 * ch + 1)) == 0x7B);
    CHECK(host.machineForTest().peek(uint16_t(0x8000 + 3 * ch + 2)) == 0x00);
  }
  CHECK(host.machineForTest().peek(0x8000 + 48) == 0);  // none of the stale bytes
}

TEST_CASE("host passes transmitted MIDI bytes out in order and counts overflow") {
  // Transmits 00, 01, 02, ... as fast as the ACIA takes them (one per 640 cycles).
  const auto rom = romWith({
      0x86, 0x03, 0xB7, 0xE1, 0x00,  // LDA #$03; STA $E100 (ACIA reset)
      0x5F,                          // CLRB
      0xB6, 0xE1, 0x00,              // loop: LDA $E100
      0x85, 0x02,                    // BITA #$02 (TDRE)
      0x27, 0xF9,                    // BEQ loop
      0xF7, 0xE1, 0x01,              // STB $E101
      0x5C,                          // INCB
      0x20, 0xF3,                    // BRA loop
  });
  RealtimeHost host(rom);
  host.prepare(4800, 48000);
  block(host, 4800);  // 0.1 s: about 312 bytes
  std::vector<MidiOutByte> out;
  MidiOutByte b{};
  while (host.popMidiOut(b)) out.push_back(b);
  REQUIRE(out.size() > 300);
  for (size_t i = 0; i < out.size(); ++i) {
    CHECK(out[i].byte == uint8_t(i));
    if (i) CHECK(out[i].cycle == out[i - 1].cycle + Acia6850::kByteCycles);
  }

  // Nobody reads for 3 s (about 9400 bytes): the queue holds 8192, the rest are dropped.
  for (int i = 0; i < 30; ++i) block(host, 4800);
  size_t queued = 0;
  while (host.popMidiOut(b)) {
    CHECK(b.byte == uint8_t(out.size() + queued));
    ++queued;
  }
  CHECK(queued == RealtimeHost::kMidiOutQueueBytes);
  const uint64_t sent = host.status().cycles / Acia6850::kByteCycles;
  const uint64_t dropped = host.status().midiDropped;
  CHECK(dropped > 1000);
  CHECK(out.size() + queued + dropped + 1 >= sent);
  CHECK(out.size() + queued + dropped <= sent + 1);
}

TEST_CASE("host reset powers on and keeps the disk") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  auto d = filledDisk(1);
  DiskImage* raw = d.get();
  REQUIRE(host.postInsert(std::move(d)));
  for (int i = 0; i < 10; ++i) block(host, 64);
  REQUIRE(host.status().cycles > 20'000);
  REQUIRE(host.postReset());
  block(host, 64);
  CHECK(host.status().cycles < 3'000);  // restarted from 0, then one block
  CHECK(host.machineForTest().disk() == raw);
  CHECK(host.status().diskPresent);
}

TEST_CASE("host threads: a MIDI producer and a UI producer against a processing thread for 2 s run clean") {
  RealtimeHost host(kIdleRom);
  constexpr size_t kFrames = 256;
  host.prepare(kFrames, 48000);
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  auto seconds = [start] { return std::chrono::duration<double>(Clock::now() - start).count(); };
  std::atomic<bool> stop{false};

  // Catch2 assertions are not thread-safe: the workers only count.
  uint64_t blocks = 0;
  std::thread audio([&] {
    std::vector<float> buf(kFrames);
    while (!stop.load()) {
      host.process(buf.data(), kFrames, 48000.0, seconds());
      ++blocks;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });

  uint64_t midiDroppedBytes = 0, midiPosted = 0;
  std::thread midi([&] {
    const uint8_t on[] = {0x90, 60, 100}, off[] = {0x80, 60, 0};
    std::vector<uint8_t> sysex(40, 0x01);
    sysex.front() = 0xF0;
    sysex.back() = 0xF7;
    for (uint64_t i = 0; !stop.load(); ++i) {
      const std::span<const uint8_t> msg =
          i % 7 == 0 ? std::span<const uint8_t>(sysex)
                     : (i % 2 ? std::span<const uint8_t>(on) : std::span<const uint8_t>(off));
      if (host.postMidi(msg, seconds())) ++midiPosted;
      else midiDroppedBytes += msg.size();
      if (i % 16 == 0) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  });

  uint64_t outBytes = 0;
  std::thread midiOut([&] {
    MidiOutByte b{};
    while (!stop.load()) {
      while (host.popMidiOut(b)) ++outBytes;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  // The UI side: keys, disks in and out, copies, saves, wave writes, panic, reset.
  uint64_t inserted = 0, copiesPosted = 0, keyPosts = 0, wavesPosted = 0, wavesBack = 0;
  uint64_t back[4] = {};  // returned, by DiskReturn
  auto takeAll = [&] {
    while (auto r = host.takeReturnedDisk()) {
      ++back[size_t(r->why)];
      if (r->why == DiskReturn::Copy) host.postMarkSaved(r->writeCount);
    }
    while (host.takeReturnedWave()) ++wavesBack;
  };
  std::thread ui([&] {
    for (uint64_t i = 0; !stop.load(); ++i) {
      const PanelKey k = PanelKey(uint8_t(i % kPanelKeyCount));
      if (host.postKey(k, true)) ++keyPosts;
      if (host.postKey(k, false)) ++keyPosts;
      if (i % 3 == 0 && host.postInsert(blankDisk())) ++inserted;
      if (i % 5 == 0) host.postEject();
      if (i % 4 == 0 && host.postCopyDisk(blankDisk())) ++copiesPosted;
      if (i % 6 == 0 && host.postWaveWrite(waveWrite(uint32_t(i % 512) * 256, {1, 2, 3})))
        ++wavesPosted;
      if (i % 50 == 0) host.postPanic();
      if (i % 200 == 199) host.postReset();
      takeAll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  // Status is readable from this thread throughout.
  uint64_t statusReads = 0, illegal = 0;
  while (seconds() < 2.0) {
    const HostStatus s = host.status();
    illegal += s.illegalOps;
    ++statusReads;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  stop.store(true);
  audio.join();
  midi.join();
  midiOut.join();
  ui.join();

  CHECK(blocks > 100);
  CHECK(statusReads > 100);
  CHECK(illegal == 0);
  CHECK(midiPosted > 100);
  CHECK(keyPosts > 100);
  CHECK(outBytes == 0);  // the idle ROM transmits nothing
  CHECK(host.status().midiDropped >= midiDroppedBytes);

  // With the producers stopped, this thread becomes the audio thread: an eject
  // and a few blocks bring every disk back.
  REQUIRE(host.postEject());
  for (int i = 0; i < 10; ++i) {
    block(host, kFrames);
    takeAll();
  }
  CHECK(inserted > 10);
  CHECK(copiesPosted > 10);
  CHECK(back[size_t(DiskReturn::Ejected)] + back[size_t(DiskReturn::Unused)] == inserted);
  CHECK(back[size_t(DiskReturn::Copy)] + back[size_t(DiskReturn::NoDiskToCopy)] == copiesPosted);
  CHECK(wavesPosted > 10);
  CHECK(wavesBack == wavesPosted);
  CHECK(host.machineForTest().disk() == nullptr);
}

TEST_CASE("host destructor frees disks still in its queues") {
  // The same history with and without disks left behind: one returned and not
  // taken, one pending insert, and an insert and a copy still in the UI queue.
  // The extra frees the destructor makes are those disks, each at least its
  // DiskImage and its track buffer.
  auto destructorFrees = [](bool withDisks) {
    auto host = std::make_unique<RealtimeHost>(kIdleRom);
    host->prepare(64, 48000);
    std::vector<float> buf(64);
    if (withDisks) REQUIRE(host->postInsert(filledDisk(1)));
    host->process(buf.data(), 64, 48000.0, 0.0);
    if (withDisks) REQUIRE(host->postInsert(filledDisk(2)));
    host->process(buf.data(), 64, 48000.0, 0.0);
    if (withDisks) {
      REQUIRE(host->postInsert(filledDisk(3)));
      REQUIRE(host->postCopyDisk(blankDisk()));
    }
    AllocationScope scope;
    host.reset();
    return scope.frees();
  };
  const size_t bare = destructorFrees(false);
  CHECK(destructorFrees(true) >= bare + 4 * 2);
}

TEST_CASE("host destructor frees wave writes still in its queues") {
  // One write returned and not taken, one still waiting in the wave queue: each
  // is at least its WaveWrite and its byte buffer.
  auto destructorFrees = [](bool withWrites) {
    auto host = std::make_unique<RealtimeHost>(kIdleRom);
    host->prepare(64, 48000);
    std::vector<float> buf(64);
    if (withWrites) REQUIRE(host->postWaveWrite(waveWrite(0, {1, 2, 3})));
    host->process(buf.data(), 64, 48000.0, 0.0);
    if (withWrites) REQUIRE(host->postWaveWrite(waveWrite(0x10000, {4, 5, 6})));
    AllocationScope scope;
    host.reset();
    return scope.frees();
  };
  const size_t bare = destructorFrees(false);
  CHECK(destructorFrees(true) >= bare + 2 * 2);
}

// ---- Audio input through the host (Plan 4 Task 4) ----

namespace {

constexpr double kRate = 48000.0;
constexpr size_t kBlk = 256;

// A host running mirage_test::adcLoopRom(mux), prepared for maxFrames at 48 kHz.
struct InputRig {
  explicit InputRig(uint8_t mux = 1, size_t maxFrames = kBlk) : host(mirage_test::adcLoopRom(mux)) {
    host.prepare(maxFrames, kRate);
  }
  // `blocks` blocks of a constant `level` through process() (in == nullptr when !plugged).
  void run(int blocks, float level, bool plugged = true, double rate = kRate) {
    const std::vector<float> in(kBlk, level);
    std::vector<float> out(kBlk);
    for (int i = 0; i < blocks; ++i)
      host.process(plugged ? in.data() : nullptr, out.data(), kBlk, rate, 0.0);
  }
  float filter7() { return host.machineForTest().filters().output(7); }
  bool jack() { return host.machineForTest().jackConnected(); }
  RealtimeHost host;
};

}  // namespace

TEST_CASE("host input reaches the machine as Machine::render does directly") {
  InputRig r;
  Machine direct(mirage_test::adcLoopRom(1));
  direct.powerOn();
  direct.prepare(kBlk, kRate);
  std::vector<float> in(kBlk), outHost(kBlk), outDirect(kBlk);
  for (int b = 0; b < 120; ++b) {
    for (size_t i = 0; i < kBlk; ++i)
      in[i] = (b % 2 ? 0.5f : 0.25f) + 0.1f * float(std::sin(0.05 * double(i)));
    r.host.process(in.data(), outHost.data(), kBlk, kRate, 0.0);
    direct.render(in.data(), outDirect.data(), kBlk, kRate);
    REQUIRE(outHost == outDirect);
  }
  CHECK(r.jack());
  CHECK(r.filter7() == direct.filters().output(7));
  CHECK(std::fabs(r.filter7()) > 0.2f);  // and the input really moved it
  CHECK(r.host.machineForTest().saveState() == direct.saveState());
}

TEST_CASE("host in == nullptr after input blocks unplugs the jack and it goes silent") {
  InputRig r;
  r.run(100, 0.5f);
  CHECK(r.jack());
  CHECK(r.filter7() == Catch::Approx(0.5).margin(0.01));
  r.run(100, 0.0f, false);
  CHECK_FALSE(r.jack());
  CHECK(std::fabs(r.filter7()) < 0.01f);
  CHECK(r.host.machineForTest().peek(0x8000) == 0x80);  // mux 1 with nothing plugged in
  CHECK(r.host.status().inputPeak == 0.0f);
  r.run(100, 0.5f);  // a device that comes back is plugged in again
  CHECK(r.jack());
  CHECK(r.filter7() == Catch::Approx(0.5).margin(0.01));
}

TEST_CASE("host input gain 0 is silence at the jack, which stays connected") {
  InputRig r;
  r.host.setInputGain(0.0f);
  r.run(100, 0.5f);
  CHECK(r.jack());
  CHECK(std::fabs(r.filter7()) < 0.01f);
  CHECK(r.host.status().inputPeak == 0.0f);
  r.host.setInputGain(1.0f);
  r.run(100, 0.5f);
  CHECK(r.filter7() == Catch::Approx(0.5).margin(0.01));
}

TEST_CASE("host input gain scales the signal at the jack and is clamped to 0..4") {
  InputRig r;
  r.host.setInputGain(0.5f);
  r.run(100, 0.5f);
  CHECK(r.filter7() == Catch::Approx(0.25).margin(0.01));
  r.host.setInputGain(100.0f);  // clamped to 4: 0.1 x 4
  r.run(100, 0.1f);
  CHECK(r.filter7() == Catch::Approx(0.4).margin(0.01));
  CHECK(r.host.status().inputPeak == Catch::Approx(0.4f));
  r.host.setInputGain(-3.0f);  // clamped to 0
  r.run(100, 0.5f);
  CHECK(std::fabs(r.filter7()) < 0.01f);
  r.host.setInputGain(2.0f);
  r.host.setInputGain(std::numeric_limits<float>::quiet_NaN());  // ignored
  r.run(100, 0.1f);
  CHECK(r.filter7() == Catch::Approx(0.2).margin(0.01));
}

TEST_CASE("host inputPeak is the block's largest |sample| after the gain; NaN cannot poison it") {
  InputRig r;
  CHECK(r.host.status().inputPeak == 0.0f);
  std::vector<float> in(kBlk, 0.1f), out(kBlk);
  in[10] = -0.6f;
  in[20] = 0.3f;
  r.host.process(in.data(), out.data(), kBlk, kRate, 0.0);
  CHECK(r.host.status().inputPeak == Catch::Approx(0.6f));
  r.host.setInputGain(2.0f);
  r.host.process(in.data(), out.data(), kBlk, kRate, 0.0);
  CHECK(r.host.status().inputPeak == Catch::Approx(1.2f));  // not clipped: the gain is a plain multiply
  // The next block reports its own peak, not a decaying or held one.
  std::fill(in.begin(), in.end(), 0.05f);
  r.host.process(in.data(), out.data(), kBlk, kRate, 0.0);
  CHECK(r.host.status().inputPeak == Catch::Approx(0.1f));
  // NaN and infinity read as 0 (as at the jack), also with gain 0 (inf x 0 is NaN).
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  in[0] = nan;
  in[1] = inf;
  in[2] = -inf;
  in[3] = 0.25f;
  r.host.process(in.data(), out.data(), kBlk, kRate, 0.0);
  CHECK(r.host.status().inputPeak == Catch::Approx(0.5f));
  r.host.setInputGain(0.0f);
  r.host.process(in.data(), out.data(), kBlk, kRate, 0.0);
  CHECK(r.host.status().inputPeak == 0.0f);
  r.host.setInputGain(4.0f);  // a finite sample that overflows to infinity after the gain
  std::fill(in.begin(), in.end(), 3.0e38f);
  r.host.process(in.data(), out.data(), kBlk, kRate, 0.0);
  CHECK(r.host.status().inputPeak == 0.0f);
  for (float s : out) REQUIRE(std::isfinite(s));
  // No input, and an unusable rate: 0.
  r.host.setInputGain(1.0f);
  std::fill(in.begin(), in.end(), 0.4f);
  r.host.process(in.data(), out.data(), kBlk, kRate, 0.0);
  CHECK(r.host.status().inputPeak == Catch::Approx(0.4f));
  r.host.process(in.data(), out.data(), kBlk, 0.0, 0.0);
  CHECK(r.host.status().inputPeak == 0.0f);
  r.host.process(in.data(), out.data(), kBlk, kRate, 0.0);
  r.host.process(nullptr, out.data(), kBlk, kRate, 0.0);
  CHECK(r.host.status().inputPeak == 0.0f);
}

TEST_CASE("host a rate change between input blocks keeps the jack alive and finite") {
  InputRig r;
  r.run(60, 0.5f);
  REQUIRE(r.filter7() == Catch::Approx(0.5).margin(0.01));
  r.run(60, 0.5f, true, 44100.0);  // the device restarted at another rate: re-plugged
  CHECK(r.jack());
  CHECK(r.filter7() == Catch::Approx(0.5).margin(0.01));
  r.run(60, 0.5f, true, 96000.0);
  CHECK(r.filter7() == Catch::Approx(0.5).margin(0.01));
  // An unusable rate in the middle changes nothing at the jack, and the input is not read.
  std::vector<float> in(kBlk, 0.5f), out(kBlk, 1.0f);
  r.host.process(in.data(), out.data(), kBlk, std::numeric_limits<double>::quiet_NaN(), 0.0);
  for (float s : out) CHECK(s == 0.0f);
  CHECK(r.jack());
  r.run(60, 0.5f);
  CHECK(r.filter7() == Catch::Approx(0.5).margin(0.01));
  // Input gone, then back at yet another rate.
  r.run(30, 0.0f, false);
  CHECK_FALSE(r.jack());
  r.run(60, 0.5f, true, 32000.0);
  CHECK(r.jack());
  CHECK(r.filter7() == Catch::Approx(0.5).margin(0.01));
}

TEST_CASE("host a block larger than the prepared size is processed in chunks, same as one render") {
  InputRig r(1, 64);  // prepared for 64, blocks of 256
  Machine direct(mirage_test::adcLoopRom(1));
  direct.powerOn();
  direct.prepare(kBlk, kRate);
  std::vector<float> in(kBlk), outHost(kBlk), outDirect(kBlk);
  for (int b = 0; b < 60; ++b) {
    for (size_t i = 0; i < kBlk; ++i) in[i] = 0.4f * float(std::sin(0.03 * double(b * 256 + int(i))));
    {
      AllocationScope scope;
      r.host.process(in.data(), outHost.data(), kBlk, kRate, 0.0);
      CHECK(scope.count() == 0);
      CHECK(scope.frees() == 0);
    }
    direct.render(in.data(), outDirect.data(), kBlk, kRate);
    // The engine's first output block depends on the block length (the output
    // resampler is still priming); from then on chunking changes nothing.
    if (b > 0) REQUIRE(outHost == outDirect);
  }
  CHECK(r.host.machineForTest().saveState() == direct.saveState());
  CHECK(r.host.status().inputPeak > 0.3f);  // the peak spans all the chunks
}

TEST_CASE("host input stays aligned across segments split at a MIDI event") {
  InputRig r(1);
  Machine direct(mirage_test::adcLoopRom(1));
  direct.powerOn();
  direct.prepare(kBlk, kRate);
  std::vector<float> in(kBlk), outHost(kBlk), outDirect(kBlk);
  for (size_t i = 0; i < kBlk; ++i) in[i] = 0.3f * float(std::sin(0.07 * double(i)));
  const uint8_t on[] = {0x90, 60, 100};
  constexpr size_t kAt = 100;
  for (int b = 0; b < 30; ++b) {
    const double now = 10.0 + b;
    // The event lands (now - t) * rate = 156 frames before the end: at frame 100.
    REQUIRE(r.host.postMidi(on, now - double(kBlk - kAt) / kRate));
    r.host.process(in.data(), outHost.data(), kBlk, kRate, now);
    direct.render(in.data(), outDirect.data(), kAt, kRate);
    direct.queueMidi(on);
    direct.render(in.data() + kAt, outDirect.data() + kAt, kBlk - kAt, kRate);
    REQUIRE(outHost == outDirect);
  }
  CHECK(r.host.machineForTest().saveState() == direct.saveState());
}

TEST_CASE("host input may alias the output buffer") {
  InputRig r;
  Machine direct(mirage_test::adcLoopRom(1));
  direct.powerOn();
  direct.prepare(kBlk, kRate);
  std::vector<float> in(kBlk, 0.3f), buf(kBlk), outDirect(kBlk);
  for (int b = 0; b < 20; ++b) {
    buf = in;
    r.host.process(buf.data(), buf.data(), kBlk, kRate, 0.0);
    direct.render(in.data(), outDirect.data(), kBlk, kRate);
    REQUIRE(buf == outDirect);
  }
}

TEST_CASE("host process with input allocates nothing after prepare") {
  InputRig r(1, 64);
  std::vector<float> in(256, 0.3f), out(256);
  size_t allocations = 0, frees = 0;
  for (int i = 0; i < 40; ++i) {
    if (i == 10) r.host.setInputGain(2.0f);
    const double rate = i < 20 ? 48000.0 : 44100.0;
    AllocationScope scope;
    r.host.process(i % 7 == 6 ? nullptr : in.data(), out.data(), i % 3 == 0 ? 64 : 256, rate, 0.0);
    allocations += scope.count();
    frees += scope.frees();
  }
  CHECK(allocations == 0);
  CHECK(frees == 0);
}

TEST_CASE("host input works before prepare (the buffer grows on first use)") {
  RealtimeHost host(mirage_test::adcLoopRom(1));  // never prepared
  std::vector<float> in(256, 0.5f), out(256);
  for (int i = 0; i < 60; ++i) host.process(in.data(), out.data(), 256, 48000.0, 0.0);
  CHECK(host.machineForTest().jackConnected());
  CHECK(host.machineForTest().filters().output(7) == Catch::Approx(0.5).margin(0.01));
  CHECK(host.status().inputPeak == Catch::Approx(0.5f));
}

// ---- fast-forward (HostOptions::fastDisk) and the calibration gate ----------

namespace {

constexpr size_t kFfBlock = 512;
constexpr double kFfRate = 48000.0;
constexpr double kFfBlockSeconds = double(kFfBlock) / kFfRate;
constexpr uint8_t kSettledLeft = 0xDB, kSettledRight = 0x06;  // "2." "1"

std::unique_ptr<DiskImage> sndA01() {
  return std::make_unique<DiskImage>(
      DiskImage::loadFile(assetPath("Factory Library/IMG/SND-A01.img")));
}

// Steps a host in 512-frame blocks at 48 kHz on a virtual clock.
struct FfDriver {
  RealtimeHost& host;
  double now = 0.0;
  std::vector<float> out = std::vector<float>(kFfBlock);
  explicit FfDriver(RealtimeHost& h) : host(h) { host.prepare(kFfBlock, kFfRate); }
  float step() {
    host.process(out.data(), kFfBlock, kFfRate, now);
    now += kFfBlockSeconds;
    float p = 0;
    for (float s : out) p = std::fmax(p, std::fabs(s));
    return p;
  }
  void steps(int n) {
    for (int i = 0; i < n; ++i) step();
  }
  bool settled() const {
    const HostStatus s = host.status();
    return s.display.left == kSettledLeft && s.display.right == kSettledRight;
  }
  // Runs until the display is settled and the drive has been idle for 2 s of blocks.
  bool bootToSettled(int maxBlocks = 6000) {
    int idle = 0;
    for (int i = 0; i < maxBlocks && idle < 188; ++i) {
      step();
      idle = (settled() && !host.status().driveActive) ? idle + 1 : 0;
    }
    return idle >= 188;
  }
  // Steps until the drive is on (the ROM starts it a little after power-on).
  bool untilDriveActive(int maxBlocks = 1000) {
    for (int i = 0; i < maxBlocks && !host.status().driveActive; ++i) step();
    return host.status().driveActive;
  }
  // Press, hold 12 blocks (>= kMinKeyHoldCycles), release, 12 blocks.
  void tap(PanelKey k) {
    host.postKey(k, true);
    steps(12);
    host.postKey(k, false);
    steps(12);
  }
};

}  // namespace

TEST_CASE("fast-forward: same machine state as a plain run", "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  for (bool mute : {false, true}) {
    RealtimeHost host(mirage_test::bootRomBytes(), HostOptions{mute, true});
    host.setOffline(true);
    FfDriver d(host);
    REQUIRE(host.postInsert(sndA01()));
    bool sawFast = false;
    {
      int idle = 0;
      for (int i = 0; i < 6000 && idle < 188; ++i) {
        d.step();
        sawFast = sawFast || host.status().fastForwarding;
        idle = (d.settled() && !host.status().driveActive) ? idle + 1 : 0;
      }
      REQUIRE(idle >= 188);
    }
    CHECK(host.status().blocks > 0);
    CHECK(sawFast);

    Machine m(mirage_test::bootRomBytes());
    m.insertDisk(DiskImage::loadFile(assetPath("Factory Library/IMG/SND-A01.img")));
    m.powerOn();
    Machine& h = host.machineForTest();
    const uint64_t target = h.cycles();
    m.runCycles(target - 1);
    while (m.cycles() < target) m.runCycles(1);
    REQUIRE(m.cycles() == target);

    const Regs &a = m.cpu().regs(), &b = h.cpu().regs();
    CHECK(a.a == b.a);
    CHECK(a.b == b.b);
    CHECK(a.dp == b.dp);
    CHECK(a.cc == b.cc);
    CHECK(a.x == b.x);
    CHECK(a.y == b.y);
    CHECK(a.u == b.u);
    CHECK(a.s == b.s);
    CHECK(a.pc == b.pc);
    bool ramSame = true, sampleSame = true;
    for (uint32_t addr = 0x8000; addr < 0xC000; ++addr)
      ramSame = ramSame && m.peek(uint16_t(addr)) == h.peek(uint16_t(addr));
    for (uint32_t addr = 0; addr < Machine::kSampleRamSize; ++addr)
      sampleSame = sampleSame && m.sampleRam(addr) == h.sampleRam(addr);
    CHECK(ramSame);
    CHECK(sampleSame);
    CHECK(m.display().left == h.display().left);
    CHECK(m.display().right == h.display().right);
    REQUIRE(m.disk() != nullptr);
    CHECK(m.disk()->writeCount() == h.disk()->writeCount());
    CHECK(m.fdc().sectorsRead() == h.fdc().sectorsRead());
    CHECK(h.fdc().sectorsRead() > 0);
  }
}

TEST_CASE("fast-forward: off by default", "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  RealtimeHost host(mirage_test::bootRomBytes());
  CHECK_FALSE(host.calibrationMuteArmed());
  FfDriver d(host);
  REQUIRE(host.postInsert(sndA01()));
  Machine m(mirage_test::bootRomBytes());
  m.insertDisk(DiskImage::loadFile(assetPath("Factory Library/IMG/SND-A01.img")));
  m.powerOn();
  m.prepare(kFfBlock, kFfRate);
  std::vector<float> ref(kFfBlock);
  bool same = true, everActive = false, everFast = false;
  float peak = 0;
  for (int i = 0; i < 400; ++i) {  // 4.3 s: the calibration and the disk start
    d.step();
    m.render(ref.data(), kFfBlock, kFfRate);
    same = same && d.out == ref;
    everActive = everActive || host.status().driveActive;
    everFast = everFast || host.status().fastForwarding;
    for (float s : d.out) peak = std::fmax(peak, std::fabs(s));
  }
  CHECK(same);
  CHECK(everActive);
  CHECK_FALSE(everFast);
  CHECK_FALSE(host.status().outputMuted);
  CHECK(host.status().blocks == 400);
  CHECK(peak > 0.0f);  // the calibration tones are audible
}

TEST_CASE("gate arming: unknown ROM", "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  auto rom = mirage_test::bootRomBytes();
  REQUIRE(rom[0x5B8] == 0x12);  // the unreached NOP filler
  RealtimeHost known(rom, HostOptions{true, false});
  CHECK(known.calibrationMuteArmed());
  rom[0x5B8] = 0x00;
  RealtimeHost unknown(rom, HostOptions{true, false});
  CHECK_FALSE(unknown.calibrationMuteArmed());
  CHECK_FALSE(RealtimeHost(rom, HostOptions{false, false}).calibrationMuteArmed());

  // Compare the blocks the known ROM's gate held closed from edge to edge.
  float peakKnown = 0, peakUnknown = 0;
  bool sawMuted = false;
  std::vector<float> pk, pu;
  std::vector<bool> muted;
  FfDriver a(known), b(unknown);
  REQUIRE(known.postInsert(sndA01()));
  REQUIRE(unknown.postInsert(sndA01()));
  for (int i = 0; i < 1000; ++i) {  // 10.7 s: through the OS-called calibration
    pk.push_back(a.step());
    pu.push_back(b.step());
    muted.push_back(known.status().outputMuted);
    CHECK_FALSE(unknown.status().outputMuted);
  }
  // Blocks with the gate closed from the block before to the block after: the
  // 5 ms close ramp is over and the 10 ms open ramp has not begun.
  for (size_t i = 1; i + 1 < muted.size(); ++i) {
    if (!(muted[i - 1] && muted[i] && muted[i + 1])) continue;
    sawMuted = true;
    peakKnown = std::fmax(peakKnown, pk[i]);
    peakUnknown = std::fmax(peakUnknown, pu[i]);
  }
  CHECK(sawMuted);
  CHECK(peakUnknown > 0.01f);  // audible
  CHECK(peakKnown < 1e-3f);    // gated (the DC blocker and resampler leave ~-88 dBFS)
}

TEST_CASE("fast-forward: offline fast-forward stops at the cap") {
  // Motor on (VIA PB4 low) and a tight loop: the drive never goes idle, so
  // only the cap ends the fast-forward.
  const auto rom = romWith({0x86, 0x1F, 0xB7, 0xE2, 0x02, 0x7F, 0xE2, 0x00, 0x20, 0xFE});
  RealtimeHost host(rom, HostOptions{false, true});
  host.setOffline(true);
  FfDriver d(host);
  d.step();  // runs the first instructions normally, or already fast-forwards
  const uint64_t frameCycles = kFfBlock * kCpuClockHz / 48000 + 64;
  for (int i = 0; i < 2; ++i) {
    const uint64_t before = host.status().cycles;
    const float peak = d.step();
    const uint64_t ran = host.status().cycles - before;
    CHECK(host.status().driveActive);
    CHECK(host.status().fastForwarding);
    CHECK(peak == 0.0f);
    CHECK(ran >= RealtimeHost::kFastOfflineCapCycles);
    CHECK(ran <= RealtimeHost::kFastOfflineCapCycles + RealtimeHost::kFastChunkCycles + frameCycles);
  }
}

#ifdef NDEBUG  // timing tests: registered in Release only
TEST_CASE("fast-forward: real-time fast-forward stays within half the block", "[assets]") {
  REQUIRE_ASSET(romPath());
  RealtimeHost host(mirage_test::bootRomBytes(), HostOptions{false, true});
  FfDriver d(host);
  REQUIRE(host.postInsert(std::make_unique<DiskImage>(DiskImage::blank())));
  using Clock = std::chrono::steady_clock;
  double worst = 0;
  int fast = 0;
  for (int i = 0; i < 600; ++i) {
    const auto t0 = Clock::now();
    d.step();
    worst = std::fmax(worst, std::chrono::duration<double>(Clock::now() - t0).count());
    fast += host.status().fastForwarding ? 1 : 0;
  }
  WARN("worst block " << worst * 1000 << " ms of " << kFfBlockSeconds * 1000 << " ms");
  CHECK(worst < kFfBlockSeconds);
  CHECK(fast > 0);
}

#endif  // NDEBUG

TEST_CASE("fast-forward: input during a fast-forward is kept") {
  // Motor on (VIA PB4 low, a Restore with h=0), then the ACIA receiver: bytes
  // land at $8000,X+.
  std::vector<uint8_t> prog = {0x86, 0x1F, 0xB7, 0xE2, 0x02, 0x7F, 0xE2, 0x00,
                               0x86, 0x00, 0xB7, 0xE8, 0x00};
  prog.insert(prog.end(), kAciaReceiver.begin(), kAciaReceiver.end());
  RealtimeHost host(romFrom({prog}), HostOptions{false, true});
  FfDriver d(host);
  REQUIRE(host.postInsert(std::make_unique<DiskImage>(DiskImage::blank())));
  d.step();
  REQUIRE(d.untilDriveActive(20));
  d.step();
  REQUIRE(host.status().fastForwarding);
  const uint64_t taken = host.status().midiBytesTaken;
  const uint8_t note[] = {0x90, 0x3C, 0x64};
  REQUIRE(host.postMidi(note, d.now));
  d.step();
  CHECK(host.status().midiBytesTaken == taken + 3);
  CHECK(host.status().fastForwarding);
  for (int i = 0; i < 200 && host.machineForTest().peek(0x8002) != 0x64; ++i) d.step();
  Machine& m = host.machineForTest();
  CHECK(m.peek(0x8000) == 0x90);
  CHECK(m.peek(0x8001) == 0x3C);
  CHECK(m.peek(0x8002) == 0x64);

  // A key pressed and released in one fast-forwarded block is held long enough.
  RealtimeHost h2(romFrom({prog}), HostOptions{false, true});
  FfDriver d2(h2);
  REQUIRE(h2.postInsert(std::make_unique<DiskImage>(DiskImage::blank())));
  REQUIRE(d2.untilDriveActive(20));
  d2.step();
  const uint64_t pressed = h2.status().cycles;
  REQUIRE(h2.postKey(PanelKey::Enter, true));
  REQUIRE(h2.postKey(PanelKey::Enter, false));
  bool released = false;
  for (int i = 0; i < 100 && !released; ++i) {
    d2.step();
    released = !h2.machineForTest().keyDown(PanelKey::Enter);
    if (released) CHECK(h2.status().cycles - pressed >= RealtimeHost::kMinKeyHoldCycles);
  }
  CHECK(released);
}

TEST_CASE("fast-forward: fade-in after the trigger ends", "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  RealtimeHost host(mirage_test::bootRomBytes(), HostOptions{false, true});
  host.setOffline(true);
  FfDriver d(host);
  REQUIRE(host.postInsert(sndA01()));
  REQUIRE(d.bootToSettled());
  const uint8_t on[] = {0x90, 60, 100};
  d.tap(PanelKey::LoadLower);
  d.tap(PanelKey::K1);
  REQUIRE(host.postMidi(on, d.now));
  d.steps(2);  // sounding

  // Press Enter. The load starts within a block or two; find the block that
  // starts with the drive on and ends with it off (the trigger ended inside it,
  // so it was rendered), and replay it on a copy of the state before it to get
  // the unfaded block.
  host.postKey(PanelKey::Enter, true);
  bool checked = false;
  float refPeak = 0, rampDrop = 0;
  for (int i = 0; i < 3000 && !checked; ++i) {
    if (i == 12) host.postKey(PanelKey::Enter, false);
    std::vector<uint8_t> s0;
    const bool trigger = host.status().driveActive;
    if (trigger) {
      s0 = host.machineForTest().saveState();
      // A note in the block's MIDI: the OS plays it once the load is over.
    }
    d.step();
    if (!trigger || host.status().driveActive) continue;
    REQUIRE(host.status().fastForwarding);
    Machine ref(mirage_test::bootRomBytes());
    ref.loadState(s0);
    ref.prepare(kFfBlock, kFfRate);
    while (ref.driveActive() || ref.outputMuted()) ref.runCycles(RealtimeHost::kFastChunkCycles);
    std::vector<float> un(kFfBlock);
    ref.render(un.data(), kFfBlock, kFfRate);
    const size_t len = size_t(RealtimeHost::kFadeInSeconds * kFfRate);
    CHECK(d.out[0] == 0.0f);
    for (size_t k = 0; k < kFfBlock; ++k) {
      const float g = k < len ? float(double(k) / double(len)) : 1.0f;
      REQUIRE(d.out[k] == Catch::Approx(un[k] * g).margin(1e-9));
      if (k >= len) refPeak = std::fmax(refPeak, std::fabs(un[k]));
      else rampDrop = std::fmax(rampDrop, std::fabs(un[k] - d.out[k]));
    }
    checked = true;
  }
  REQUIRE(checked);
  // The OS's note is mostly gone after a load, but what is left (about -72 dBFS)
  // is real signal: the ramp took part of it away and the tail is untouched.
  CHECK(refPeak > 1e-4f);
  CHECK(rampDrop > 1e-5f);
}

TEST_CASE("fast-forward: process allocates nothing, real time and offline", "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  for (bool offline : {false, true}) {
    RealtimeHost host(mirage_test::bootRomBytes(), HostOptions{true, true});
    host.setOffline(offline);
    FfDriver d(host);
    REQUIRE(host.postInsert(sndA01()));
    const uint8_t on[] = {0x90, 60, 100};
    size_t allocations = 0, frees = 0, fast = 0;
    for (int i = 0; i < 900; ++i) {
      if (i == 5) REQUIRE(host.postMidi(on, d.now));
      {
        AllocationScope scope;
        d.step();
        allocations += scope.count();
        frees += scope.frees();
      }
      fast += host.status().fastForwarding ? 1 : 0;
    }
    CHECK(allocations == 0);
    CHECK(frees == 0);
    CHECK(fast > 0);
  }
}

#ifdef NDEBUG
TEST_CASE("fast-forward: a factory boot is playable fast (Release)", "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  using Clock = std::chrono::steady_clock;
  RealtimeHost host(mirage_test::bootRomBytes(), HostOptions{true, true});
  FfDriver d(host);
  const auto t0 = Clock::now();
  auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - t0).count(); };
  // One paced block: sleep to the block's wall-clock slot, then process.
  int n = 0;
  auto paced = [&] {
    std::this_thread::sleep_until(t0 + std::chrono::duration<double>(n++ * kFfBlockSeconds));
    d.now = elapsed();
    return d.step();
  };
  REQUIRE(host.postInsert(sndA01()));
  bool posted = false;
  double playable = -1, settledAt = -1;
  while (elapsed() < 20.0 && playable < 0) {
    const float p = paced();
    if (!posted && d.settled() && !host.status().driveActive) {
      settledAt = elapsed();
      const uint8_t on[] = {0x90, 60, 100};
      REQUIRE(host.postMidi(on, d.now));
      posted = true;
    }
    if (posted && p > 0.05f) playable = elapsed();
  }
  WARN("boot to first audible note: " << playable << " s (display settled at " << settledAt
                                       << " s)");
  REQUIRE(playable > 0);
  CHECK(playable < 3.0);

  // Panel Load Lower: Load Lower, 1, Enter, until the drive has stopped.
  const uint8_t off[] = {0x80, 60, 0};
  host.postMidi(off, d.now);
  auto tapPaced = [&](PanelKey k) {
    host.postKey(k, true);
    for (int i = 0; i < 12; ++i) paced();
    host.postKey(k, false);
    for (int i = 0; i < 12; ++i) paced();
  };
  tapPaced(PanelKey::LoadLower);
  tapPaced(PanelKey::K1);
  host.postKey(PanelKey::Enter, true);
  for (int i = 0; i < 12; ++i) paced();
  const double loadStart = elapsed();
  host.postKey(PanelKey::Enter, false);
  bool sawActive = false;
  int idle = 0;
  while (elapsed() < loadStart + 20.0 && idle < 94) {
    paced();
    sawActive = sawActive || host.status().driveActive;
    idle = (sawActive && !host.status().driveActive) ? idle + 1 : 0;
  }
  WARN("panel Load Lower: " << elapsed() - loadStart - 94 * kFfBlockSeconds << " s");
  CHECK(sawActive);
}
#endif  // NDEBUG

// ---- block MIDI: events with frame offsets, and the backlog ----------------

namespace {

// A host process() call with block MIDI, no audio input.
void blockWith(RealtimeHost& host, size_t frames, std::span<const BlockMidiEvent> events,
               double rate = 48000.0) {
  std::vector<float> buf(frames + 1);
  host.process(nullptr, buf.data(), frames, rate, events);
}

std::vector<uint8_t> sysexOf(size_t n) {
  std::vector<uint8_t> v(n, 0x01);
  v.front() = 0xF0;
  v[1] = 0x7D;  // non-commercial ID
  v.back() = 0xF7;
  return v;
}

}  // namespace

TEST_CASE("block MIDI: each event enters at its frame") {
  const auto rom = romFrom({kAciaReceiver});
  RealtimeHost host(rom);
  Machine ref(rom);
  ref.powerOn();
  host.prepare(512, 48000);
  ref.prepare(512, 48000);
  std::vector<float> got(512), want(512);
  for (int i = 0; i < 4; ++i) {  // the receiver sets up the ACIA
    host.process(nullptr, got.data(), 512, 48000.0, std::span<const BlockMidiEvent>());
    ref.render(want.data(), 512, 48000.0);
  }
  const uint8_t on[] = {0x90, 60, 100}, cc[] = {0xB0, 1, 64}, off[] = {0x80, 60, 0};
  const BlockMidiEvent events[] = {{0, on}, {100, cc}, {511, off}};
  host.process(nullptr, got.data(), 512, 48000.0, events);
  ref.queueMidi(on);
  ref.render(want.data(), 100, 48000.0);
  ref.queueMidi(cc);
  ref.render(want.data() + 100, 411, 48000.0);
  ref.queueMidi(off);
  ref.render(want.data() + 511, 1, 48000.0);
  CHECK(got == want);
  CHECK(host.machineForTest().cycles() == ref.cycles());
  CHECK(host.machineForTest().midiQueueDepth() == ref.midiQueueDepth());
  CHECK(host.machineForTest().saveState() == ref.saveState());
  // Frames out of order or past the end are clamped, not trusted.
  const BlockMidiEvent odd[] = {{300, on}, {200, cc}, {9999, off}};
  host.process(nullptr, got.data(), 512, 48000.0, odd);
  ref.render(want.data(), 300, 48000.0);
  ref.queueMidi(on);
  ref.queueMidi(cc);
  ref.render(want.data() + 300, 211, 48000.0);
  ref.queueMidi(off);
  ref.render(want.data() + 511, 1, 48000.0);
  CHECK(host.machineForTest().saveState() == ref.saveState());
  CHECK(host.status().midiBytesTaken == 0);  // the timestamped queue is untouched
}

TEST_CASE("block MIDI: frames above the prepared size") {
  RealtimeHost host(romFrom({kAciaReceiver}));
  host.prepare(256, 48000);
  const uint8_t a[] = {0x90, 1, 2}, b[] = {0x91, 3, 4}, c[] = {0x92, 5, 6};
  const BlockMidiEvent events[] = {{10, a}, {600, b}, {999, c}};
  std::vector<float> buf(1000);
  {
    AllocationScope scope;
    host.process(nullptr, buf.data(), 1000, 48000.0, events);
    CHECK(scope.count() == 0);
    CHECK(scope.frees() == 0);
  }
  for (int i = 0; i < 40; ++i) block(host, 64);
  const uint8_t want[] = {0x90, 1, 2, 0x91, 3, 4, 0x92, 5, 6};
  for (size_t i = 0; i < 9; ++i) CHECK(host.machineForTest().peek(uint16_t(0x8000 + i)) == want[i]);
  CHECK(host.status().midiDropped == 0);
}

TEST_CASE("block MIDI: zero-frame block") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  block(host, 64);
  const uint64_t cycles = host.machineForTest().cycles();
  const uint8_t on[] = {0x90, 60, 100}, off[] = {0x80, 60, 0};
  const BlockMidiEvent events[] = {{0, on}, {5, off}};
  REQUIRE(host.postKey(PanelKey::Cancel, true));  // the queues are still applied
  float unused = 0.0f;
  host.process(nullptr, &unused, 0, 48000.0, events);
  CHECK(host.machineForTest().keyDown(PanelKey::Cancel));
  CHECK(host.machineForTest().cycles() == cycles);
  CHECK(host.machineForTest().midiQueueDepth() == 6);
}

TEST_CASE("block MIDI: a 100 KB SysEx goes through the backlog") {
  RealtimeHost host(kIdleRom);
  constexpr size_t kBlock = 4096;
  host.prepare(kBlock, 48000);
  const auto sysex = sysexOf(100 * 1000);
  const uint8_t on[] = {0x90, 60, 100};
  const BlockMidiEvent events[] = {{0, sysex}, {1, on}};
  blockWith(host, 1, events);
  const size_t ring = Machine::kMidiQueueCapacity;
  CHECK(host.status().midiBacklog == sysex.size() + 3 - ring);
  size_t blocks = 1;
  while (host.status().midiBacklog != 0 && blocks < 1000) {
    CHECK(host.status().midiDropped == 0);
    block(host, kBlock);
    ++blocks;
  }
  CHECK(host.status().midiBacklog == 0);
  // 100003 bytes at 320 us: about 32 s of emulated time, i.e. 375 blocks of 4096.
  const double seconds = double(blocks * kBlock) / 48000.0;
  CHECK(seconds > 30.0);
  CHECK(seconds < 34.0);
  CHECK(host.status().midiDropped == 0);
  // The note-on went behind the SysEx's last byte: it is still in the ring.
  CHECK(host.machineForTest().midiQueueDepth() >= 3);
}

TEST_CASE("block MIDI: a note-on after a long SysEx is delivered after its last byte") {
  RealtimeHost host(romFrom({kAciaReceiver}));
  host.prepare(512, 48000);
  block(host, 512);
  const auto sysex = sysexOf(4200);  // more than the ring holds; the receiver stores from $8000
  const uint8_t on[] = {0x90, 60, 100};
  // The note-on is at frame 0 too, while the ring is still empty: only the
  // backlog-empty guard keeps it behind the SysEx.
  const BlockMidiEvent events[] = {{0, sysex}, {0, on}};
  blockWith(host, 512, events);
  CHECK(host.status().midiBacklog > 0);
  for (int i = 0; i < 1200; ++i) block(host, 512);  // 12.8 s, far more than 4203 x 320 us
  CHECK(host.status().midiBacklog == 0);
  CHECK(host.machineForTest().peek(0x8000) == 0xF0);
  CHECK(host.machineForTest().peek(uint16_t(0x8000 + 4199)) == 0xF7);
  CHECK(host.machineForTest().peek(uint16_t(0x8000 + 4200)) == 0x90);
  CHECK(host.machineForTest().peek(uint16_t(0x8000 + 4202)) == 100);
}

TEST_CASE("block MIDI: a 300 KB SysEx is dropped whole") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  const auto big = sysexOf(100 * 1000);
  const BlockMidiEvent first[] = {{0, big}};
  float unused = 0.0f;
  host.process(nullptr, &unused, 0, 48000.0, first);  // no time passes: nothing drains
  const size_t waiting = host.status().midiBacklog;
  REQUIRE(waiting > 90 * 1000);
  const auto huge = sysexOf(300 * 1000);
  const BlockMidiEvent second[] = {{0, huge}};
  host.process(nullptr, &unused, 0, 48000.0, second);
  CHECK(host.status().midiDropped == 300 * 1000);
  CHECK(host.status().midiBacklog == waiting);
}

TEST_CASE("block MIDI: panic and reset clear the backlog") {
  RealtimeHost host(romFrom({kAciaReceiver}));
  host.prepare(64, 48000);
  block(host, 64);
  const auto sysex = sysexOf(50 * 1000);
  const BlockMidiEvent events[] = {{0, sysex}};
  blockWith(host, 64, events);
  REQUIRE(host.status().midiBacklog > 40 * 1000);
  REQUIRE(host.postPanic());
  block(host, 64);
  CHECK(host.status().midiBacklog == 0);
  CHECK(host.status().midiDropped == 0);  // a flush, not a loss
  CHECK(host.machineForTest().midiQueueDepth() <= 48);

  blockWith(host, 64, events);
  REQUIRE(host.status().midiBacklog > 40 * 1000);
  REQUIRE(host.postReset());
  block(host, 64);
  CHECK(host.status().midiBacklog == 0);
  CHECK(host.machineForTest().midiQueueDepth() == 0);
}

TEST_CASE("block MIDI: an invalid rate drops the events and counts them") {
  RealtimeHost host(kIdleRom);
  host.prepare(64, 48000);
  const uint8_t on[] = {0x90, 60, 100};
  const BlockMidiEvent events[] = {{0, on}, {1, on}};
  blockWith(host, 64, events, 0.0);
  CHECK(host.status().midiDropped == 6);
  CHECK(host.machineForTest().midiQueueDepth() == 0);
}

TEST_CASE("block MIDI: process allocates nothing with events and a backlog") {
  RealtimeHost host(kIdleRom);
  constexpr size_t kBlock = 512;
  host.prepare(kBlock, 48000);
  const auto sysex = sysexOf(20 * 1000);
  const uint8_t on[] = {0x90, 60, 100}, off[] = {0x80, 60, 0};
  const BlockMidiEvent events[] = {{0, sysex}, {100, on}, {400, off}};
  std::vector<float> buf(kBlock);
  AllocationScope scope;
  for (int i = 0; i < 150; ++i) {
    if (i == 0) host.process(nullptr, buf.data(), kBlock, 48000.0, events);
    else host.process(nullptr, buf.data(), kBlock, 48000.0, std::span<const BlockMidiEvent>());
    if (i == 100) host.postPanic();
  }
  CHECK(scope.count() == 0);
  CHECK(scope.frees() == 0);
  CHECK(host.status().midiBacklog == 0);
}

TEST_CASE("block MIDI: each event enters at its frame, on a booted SND-A01", "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  RealtimeHost host(mirage_test::bootRomBytes());
  FfDriver d(host);
  REQUIRE(host.postInsert(sndA01()));
  Machine ref(mirage_test::bootRomBytes());
  ref.insertDisk(DiskImage::loadFile(assetPath("Factory Library/IMG/SND-A01.img")));
  ref.powerOn();
  ref.prepare(kFfBlock, kFfRate);
  REQUIRE(d.bootToSettled());
  std::vector<float> want(kFfBlock);
  for (uint64_t i = 0, n = host.status().blocks; i < n; ++i) ref.render(want.data(), kFfBlock, kFfRate);
  REQUIRE(host.machineForTest().cycles() == ref.cycles());

  const uint8_t on[] = {0x90, 60, 100}, cc[] = {0xB0, 1, 64}, off[] = {0x80, 60, 0};
  const BlockMidiEvent events[] = {{0, on}, {100, cc}, {511, off}};
  std::vector<float> got(kFfBlock);
  host.process(nullptr, got.data(), kFfBlock, kFfRate, events);
  ref.queueMidi(on);
  ref.render(want.data(), 100, kFfRate);
  ref.queueMidi(cc);
  ref.render(want.data() + 100, 411, kFfRate);
  ref.queueMidi(off);
  ref.render(want.data() + 511, 1, kFfRate);
  CHECK(got == want);
  CHECK(host.machineForTest().cycles() == ref.cycles());
  CHECK(host.machineForTest().midiQueueDepth() == ref.midiQueueDepth());
}

TEST_CASE("block MIDI: a zero-frame block never fast-forwards", "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  RealtimeHost host(mirage_test::bootRomBytes(), HostOptions{false, true});
  host.setOffline(true);
  FfDriver d(host);
  REQUIRE(host.postInsert(sndA01()));
  REQUIRE(d.untilDriveActive());
  const uint64_t cycles = host.machineForTest().cycles();
  const uint8_t on[] = {0x90, 60, 100};
  const BlockMidiEvent events[] = {{0, on}};
  float unused = 0.0f;
  host.process(nullptr, &unused, 0, kFfRate, events);
  CHECK(host.machineForTest().cycles() == cycles);
  CHECK_FALSE(host.status().fastForwarding);
  CHECK(host.machineForTest().midiQueueDepth() == 3);
  d.step();  // a real block still fast-forwards
  CHECK(host.status().fastForwarding);
}

// ---- Snapshots, machine swaps and the idle pump (Plan 5 Task 5) ---------------

namespace {

// Every HostStatus field except `blocks`, which counts this host's own process() calls.
void requireSameStatus(const HostStatus& a, const HostStatus& b) {
  REQUIRE(a.display.left == b.display.left);
  REQUIRE(a.display.right == b.display.right);
  REQUIRE(a.driveActive == b.driveActive);
  REQUIRE(a.diskPresent == b.diskPresent);
  REQUIRE(a.diskDirty == b.diskDirty);
  REQUIRE(a.diskWrites == b.diskWrites);
  REQUIRE(a.illegalOps == b.illegalOps);
  REQUIRE(a.midiDropped == b.midiDropped);
  REQUIRE(a.cycles == b.cycles);
  REQUIRE(a.midiBytesTaken == b.midiBytesTaken);
  REQUIRE(a.midiBytesFlushed == b.midiBytesFlushed);
  REQUIRE(a.midiQueueDepth == b.midiQueueDepth);
  REQUIRE(a.midiBacklog == b.midiBacklog);
  REQUIRE(std::memcmp(&a.inputPeak, &b.inputPeak, sizeof(float)) == 0);
  REQUIRE(a.outputMuted == b.outputMuted);
  REQUIRE(a.fastForwarding == b.fastForwarding);
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

std::unique_ptr<std::vector<uint8_t>> reservedBuffer(size_t bytes) {
  auto v = std::make_unique<std::vector<uint8_t>>();
  v->reserve(bytes);
  return v;
}

// Takes a snapshot of `host` through postSnapshot + pumpWhileIdle (no process()).
std::vector<uint8_t> idleSnapshot(RealtimeHost& host) {
  REQUIRE(host.postSnapshot(reservedBuffer(1 << 20)));
  REQUIRE(host.pumpWhileIdle());
  auto r = host.takeReturnedSnapshot();
  REQUIRE(r);
  REQUIRE(r->ok);
  return std::move(*r->bytes);
}

std::vector<float> sine(size_t frames, float amp = 0.3f) {
  std::vector<float> v(frames);
  for (size_t i = 0; i < frames; ++i) v[i] = amp * float(std::sin(0.05 * double(i)));
  return v;
}

}  // namespace

TEST_CASE("host: snapshot taken mid-boot restores and continues identically", "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  const auto rom = mirage_test::bootRomBytes();
  RealtimeHost a(rom), b(rom);
  a.prepare(1024, 48000);
  b.prepare(1024, 48000);
  REQUIRE(a.postInsert(sndA01()));
  const std::span<const BlockMidiEvent> none;
  std::vector<float> outA(1024), outB(1024);
  constexpr int kBootBlocks = 844;  // 9 s of 512-frame blocks at 48 kHz
  for (int i = 0; i < kBootBlocks; ++i) a.process(nullptr, outA.data(), 512, 48000.0, none);
  REQUIRE(a.status().cycles > 0);

  REQUIRE(a.postSnapshot(reservedBuffer(1 << 20)));
  a.process(nullptr, outA.data(), 512, 48000.0, none);  // the snapshot is taken at its start
  auto snap = a.takeReturnedSnapshot();
  REQUIRE(snap);
  REQUIRE(snap->ok);
  REQUIRE(snap->bytes->size() > size_t(DiskImage::kImageBytes));

  auto m = b.buildMachine(*snap->bytes);
  REQUIRE(m);
  REQUIRE(b.postRestore(std::move(m)));
  CHECK(b.takeReturnedMachine() == nullptr);  // not swapped until a block starts
  b.process(nullptr, outB.data(), 512, 48000.0, none);
  CHECK(b.takeReturnedMachine() != nullptr);
  CHECK(std::memcmp(outA.data(), outB.data(), 512 * sizeof(float)) == 0);
  requireSameStatus(a.status(), b.status());

  // 200 blocks of 1024 frames run from 9.01 s to 13.28 s: a note at 12 s, off at 12.5 s.
  const size_t start = size_t(kBootBlocks + 1) * 512;
  const size_t onAt = 12 * 48000 - start, offAt = 12 * 48000 + 24000 - start;
  const uint8_t on[] = {0x90, 60, 100}, off[] = {0x80, 60, 0};
  bool sawSound = false;
  for (size_t blk = 0; blk < 200; ++blk) {
    std::vector<BlockMidiEvent> events;
    if (onAt / 1024 == blk) events.push_back({uint32_t(onAt % 1024), on});
    if (offAt / 1024 == blk) events.push_back({uint32_t(offAt % 1024), off});
    a.process(nullptr, outA.data(), 1024, 48000.0, events);
    b.process(nullptr, outB.data(), 1024, 48000.0, events);
    REQUIRE(sameBits(outA, outB));
    requireSameStatus(a.status(), b.status());
    for (float s : outA) sawSound = sawSound || s != 0.0f;
  }
  CHECK(sawSound);
  CHECK(a.machineForTest().saveState() == b.machineForTest().saveState());
}

TEST_CASE("host: snapshot and restore complete with no process() calls") {
  RealtimeHost host(kIdleRom);
  host.prepare(256, 48000);
  CHECK_FALSE(host.takeReturnedSnapshot().has_value());
  REQUIRE(host.postSnapshot(reservedBuffer(1 << 20)));
  CHECK_FALSE(host.takeReturnedSnapshot().has_value());  // nothing has run yet
  REQUIRE(host.pumpWhileIdle());
  auto snap = host.takeReturnedSnapshot();
  REQUIRE(snap);
  CHECK(snap->ok);
  CHECK(*snap->bytes == host.machineForTest().saveState());

  // Too small: handed back with ok false and the same buffer, never grown.
  auto small = reservedBuffer(16);
  const auto* raw = small.get();
  REQUIRE(host.postSnapshot(std::move(small)));
  REQUIRE(host.pumpWhileIdle());
  auto refused = host.takeReturnedSnapshot();
  REQUIRE(refused);
  CHECK_FALSE(refused->ok);
  CHECK(refused->bytes.get() == raw);
  CHECK(refused->bytes->capacity() == 16);
  CHECK_FALSE(host.postSnapshot(nullptr));

  // A disk swap in its gap: the restore hands the waiting insert back Unused
  // (the restored machine has its own drive contents).
  REQUIRE(host.postInsert(filledDisk(1)));
  REQUIRE(host.postInsert(filledDisk(2)));
  REQUIRE(host.pumpWhileIdle());
  auto ejected = host.takeReturnedDisk();
  REQUIRE(ejected);
  CHECK(ejected->why == DiskReturn::Ejected);

  auto m = host.buildMachine(*snap->bytes);
  const Machine* fresh = m.get();
  const Machine* old = &host.machineForTest();
  REQUIRE(host.postRestore(std::move(m)));
  CHECK_FALSE(host.postRestore(nullptr));
  CHECK(host.takeReturnedMachine() == nullptr);
  REQUIRE(host.pumpWhileIdle());
  auto back = host.takeReturnedMachine();
  CHECK(back.get() == old);
  CHECK(&host.machineForTest() == fresh);
  CHECK(host.status().blocks == 0);
  CHECK_FALSE(host.status().diskPresent);  // the snapshot's empty drive
  auto unused = host.takeReturnedDisk();
  REQUIRE(unused);
  CHECK(unused->why == DiskReturn::Unused);
  CHECK(unused->disk->toBytes()[0] == 2);
}

TEST_CASE("host: pumpWhileIdle excludes process") {
  // The jack rig makes every rendered block audible, so a silent block is one
  // that found the machine claimed. A block that ran bumps status().blocks;
  // the pump publishes status too but never changes the count.
  RealtimeHost host(mirage_test::adcLoopRom(1));
  constexpr size_t kFrames = 256;
  host.prepare(kFrames, 48000);
  const auto in = sine(kFrames);
  std::vector<float> warm(kFrames);
  for (int i = 0; i < 20; ++i) host.process(in.data(), warm.data(), kFrames, 48000.0, 0.0);

  // 300 us, spinning: sleep_for is too coarse on Windows.
  const auto gap = [] {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(300);
    while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
  };
  std::atomic<bool> stop{false};
  uint64_t ran = 0, claimed = 0, silentRan = 0, loudClaimed = 0;  // Catch2 is not thread-safe
  std::thread audio([&] {
    std::vector<float> out(kFrames);
    while (!stop.load()) {
      const uint64_t before = host.status().blocks;
      std::fill(out.begin(), out.end(), 1.0f);
      host.process(in.data(), out.data(), kFrames, 48000.0, 0.0);
      const bool silent = std::all_of(out.begin(), out.end(), [](float s) { return s == 0.0f; });
      if (host.status().blocks == before) {
        ++claimed;
        if (!silent) ++loudClaimed;
      } else {
        ++ran;
        if (silent) ++silentRan;
      }
      gap();  // between callbacks
    }
  });

  uint64_t posted = 0, returned = 0, notOk = 0, pumped = 0, refused = 0;
  for (int i = 0; i < 1000; ++i) {
    if (i % 4 == 0 && host.postSnapshot(reservedBuffer(1 << 20))) ++posted;
    if (host.pumpWhileIdle()) ++pumped;
    else ++refused;
    while (auto r = host.takeReturnedSnapshot()) {
      ++returned;
      if (!r->ok) ++notOk;
    }
    gap();
  }
  stop.store(true);
  audio.join();
  // With process() stopped, the pump always gets the machine; a few rounds
  // empty the state queue even if its returns were full.
  for (int i = 0; i < 4; ++i) {
    REQUIRE(host.pumpWhileIdle());
    while (auto r = host.takeReturnedSnapshot()) {
      ++returned;
      if (!r->ok) ++notOk;
    }
  }
  INFO("posted " << posted << " pumped " << pumped << " refused " << refused << " ran " << ran
                 << " claimed " << claimed);

  CHECK(posted > 100);  // of 250 (a full queue refuses some)
  CHECK(returned == posted);
  CHECK(notOk == 0);
  CHECK(pumped + refused == 1000);
  CHECK(ran > 10);
  CHECK(pumped > 10);
  CHECK(claimed > 0);
  CHECK(loudClaimed == 0);
  CHECK(silentRan == 0);
}

TEST_CASE("host: a restored machine renders at a new rate after prepare") {
  // The new machine is built (and prepared) at 64 frames, 48 kHz; the DAW then
  // prepares the host for larger blocks at 96 kHz before the swap. 4096 frames
  // would overrun what the new machine reserved if it were rendered in
  // host-sized chunks.
  const size_t frames = GENERATE(size_t(1024), size_t(4096));
  RealtimeHost host(mirage_test::adcLoopRom(1));
  host.prepare(64, 48000);
  const auto in48 = sine(64);
  std::vector<float> out48(64);
  for (int i = 0; i < 80; ++i) host.process(in48.data(), out48.data(), 64, 48000.0, 0.0);

  auto m = host.buildMachine(idleSnapshot(host));
  const Machine* fresh = m.get();
  REQUIRE(fresh->preparedFrames() == 64);
  REQUIRE(host.postRestore(std::move(m)));
  host.prepare(frames, 96000);
  REQUIRE(host.postSnapshot(reservedBuffer(1 << 20)));  // and a snapshot request behind it

  const auto in = sine(frames);
  std::vector<float> out(frames);
  size_t allocations = 0, frees = 0;
  float peak = 0.0f;
  for (int i = 0; i < 50; ++i) {
    {
      AllocationScope scope;
      host.process(in.data(), out.data(), frames, 96000.0, 0.0);
      allocations += scope.count();
      frees += scope.frees();
    }
    for (float s : out) peak = std::max(peak, std::fabs(s));
  }
  CHECK(allocations == 0);
  CHECK(frees == 0);
  CHECK(&host.machineForTest() == fresh);
  CHECK(host.takeReturnedMachine() != nullptr);
  auto snap = host.takeReturnedSnapshot();
  REQUIRE(snap);
  CHECK(snap->ok);
  CHECK(peak > 0.01f);  // audio renders at 96 kHz
}

TEST_CASE("host: wrong ROM") {
  RealtimeHost host(kIdleRom);
  Machine other(romWith({0x20, 0xFE, 0x01}));  // one byte differs
  other.powerOn();
  const auto bytes = other.saveState();
  REQUIRE_THROWS_MATCHES(host.buildMachine(bytes), StateError,
                         Catch::Matchers::Message("snapshot was taken with a different boot ROM"));
}
