// RealtimeHost (host/include/mirage/realtime_host.h): the audio thread owns the
// Machine; the UI and MIDI threads post through lock-free queues.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
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
