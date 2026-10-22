// Real-time safety of Machine::render (Plan 2 Task 4): prepare(), the MIDI
// ring buffer and the output soft clip.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "alloc_counter.h"
#include "asset_paths.h"
#include "booted_machine.h"
#include "mirage/machine.h"
#include "mirage/state_io.h"

using namespace mirage;
using mirage_test::AllocationScope;
using mirage_test::bootedMachine;

namespace {

// A ROM that loops on BRA *: no assets needed.
std::vector<uint8_t> idleRom() {
  std::vector<uint8_t> rom(4096, 0x12);
  rom[0] = 0x20;
  rom[1] = 0xFE;
  rom[0xFFE] = 0xF0;
  rom[0xFFF] = 0x00;
  return rom;
}

}  // namespace

TEST_CASE("softClip is identity to 0.75, odd, monotonic, below 1.0") {
  CHECK(Machine::softClip(0.5f) == 0.5f);
  CHECK(Machine::softClip(0.75f) == 0.75f);
  CHECK(Machine::softClip(-0.6f) == -0.6f);
  CHECK(Machine::softClip(0.0f) == 0.0f);
  const float big = Machine::softClip(10.0f);
  CHECK(big > 0.99f);
  CHECK(big <= 1.0f);
  CHECK(Machine::softClip(-10.0f) == -big);
  float prev = Machine::softClip(-2.0f);
  for (float x = -1.99f; x <= 2.0f; x += 0.01f) {  // beyond ~3 float rounding reaches 1.0
    const float y = Machine::softClip(x);
    CHECK(y > prev);
    CHECK(std::fabs(y) < 1.0f);
    CHECK(Machine::softClip(-x) == -y);
    prev = y;
  }
}

TEST_CASE("render after prepare allocates nothing", "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  auto m = bootedMachine("SND-A01.img");
  m->prepare(512, 48000);
  std::vector<float> buf(512);
  const uint8_t on[] = {0x90, 60, 100}, off[] = {0x80, 60, 0};
  float peak = 0.0f;
  size_t allocations, frees;
  {
    AllocationScope scope;
    for (int i = 0; i < 200; ++i) {
      if (i == 20) m->queueMidi(on);
      if (i == 120) m->queueMidi(off);
      m->render(buf.data(), 512, 48000);
      for (float s : buf) peak = std::fmax(peak, std::fabs(s));
    }
    m->flushMidi();
    allocations = scope.count();
    frees = scope.frees();
  }
  CHECK(allocations == 0);
  CHECK(frees == 0);
  CHECK(peak > 0.01f);  // the note really played through the audited path
}

TEST_CASE("render allocates nothing after loadState either", "[assets]") {
  REQUIRE_ASSET(romPath());
  REQUIRE_ASSET(assetPath("Factory Library/IMG/SND-A01.img"));
  auto m = bootedMachine("SND-A01.img");
  const std::vector<uint8_t> snap = m->saveState();
  m->prepare(256, 44100);
  m->loadState(snap);
  std::vector<float> buf(256);
  const uint8_t on[] = {0x90, 64, 100};
  m->queueMidi(on);
  AllocationScope scope;
  for (int i = 0; i < 100; ++i) m->render(buf.data(), 256, 44100);
  CHECK(scope.count() == 0);
  CHECK(scope.frees() == 0);
}

TEST_CASE("AllocationScope counts allocations and frees on its own thread") {
  auto* outside = new int(1);  // allocated before the scope, freed inside it
  AllocationScope scope;
  CHECK(scope.count() == 0);
  CHECK(scope.frees() == 0);
  auto* v = new int(7);
  CHECK(scope.count() == 1);
  CHECK(scope.frees() == 0);
  delete v;
  CHECK(scope.frees() == 1);
  delete outside;
  CHECK(scope.frees() == 2);
  int* none = nullptr;
  delete none;  // a null delete frees nothing
  CHECK(scope.frees() == 2);
  std::vector<int> vec(100);
  CHECK(scope.count() > 1);  // the debug runtime may add container-proxy allocations
}

TEST_CASE("MIDI ring drops bytes beyond capacity and counts them") {
  Machine m(idleRom());
  m.powerOn();
  const std::vector<uint8_t> bytes(5000, 0xF8);
  m.queueMidi(bytes);
  CHECK(m.midiQueueDepth() == Machine::kMidiQueueCapacity);
  CHECK(m.midiQueueDepth() == 4096);
  CHECK(m.midiOverflows() == 904);
  m.powerOn();
  CHECK(m.midiQueueDepth() == 0);
  CHECK(m.midiOverflows() == 0);
}

TEST_CASE("MIDI ring wraps and delivers in order") {
  Machine m(idleRom());
  m.powerOn();
  // Fill, drain part of it by running, then queue past the end of the array.
  const std::vector<uint8_t> first(4000, 0xF8);
  m.queueMidi(first);
  m.runCycles(640 * 3000);  // about 3000 byte times
  const size_t left = m.midiQueueDepth();
  CHECK(left < 1100);
  CHECK(left > 900);
  const std::vector<uint8_t> more(3000, 0xFE);
  m.queueMidi(more);
  CHECK(m.midiQueueDepth() == left + 3000);
  CHECK(m.midiOverflows() == 0);
}

TEST_CASE("flushMidi empties the queue") {
  Machine m(idleRom());
  m.powerOn();
  const uint8_t bytes[] = {0x90, 60, 100};
  m.queueMidi(bytes);
  CHECK(m.midiQueueDepth() == 3);
  m.flushMidi();
  CHECK(m.midiQueueDepth() == 0);
  m.runCycles(640 * 10);
  CHECK(m.midiQueueDepth() == 0);
  m.queueMidi(bytes);  // the ring is still usable
  CHECK(m.midiQueueDepth() == 3);
}

TEST_CASE("snapshot rejects a MIDI queue longer than the ring") {
  Machine m(idleRom());
  m.powerOn();
  const uint8_t marker[] = {0xA1, 0xB2, 0xC3};
  m.queueMidi(marker);
  const std::vector<uint8_t> good = m.saveState();

  // The queue section is u64 nextMidiCycle, u32 count, the bytes. Find the
  // bytes, then the count sits just before them.
  size_t at = 0;
  for (size_t i = 4; i + 3 <= good.size(); ++i)
    if (good[i] == 0xA1 && good[i + 1] == 0xB2 && good[i + 2] == 0xC3) {
      at = i;
      break;
    }
  REQUIRE(at >= 4);
  REQUIRE(good[at - 4] == 3);

  const auto withCount = [&](uint32_t count) {
    std::vector<uint8_t> b = good;
    for (int i = 0; i < 4; ++i) b[at - 4 + size_t(i)] = uint8_t(count >> (8 * i));
    return b;
  };
  // A count past the ring is refused (the bytes that follow are irrelevant) and
  // the machine is untouched.
  CHECK_THROWS_AS(m.loadState(withCount(Machine::kMidiQueueCapacity + 1)), StateError);
  CHECK(m.midiQueueDepth() == 3);
  CHECK_THROWS_AS(m.loadState(withCount(0xFFFFFFFFu)), StateError);
  CHECK(m.midiQueueDepth() == 3);
  CHECK_NOTHROW(m.loadState(good));

  // A completely full ring round-trips.
  Machine full(idleRom());
  full.powerOn();
  std::vector<uint8_t> bytes(Machine::kMidiQueueCapacity);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i * 7);
  full.queueMidi(bytes);
  const std::vector<uint8_t> snap = full.saveState();
  Machine other(idleRom());
  other.loadState(snap);
  CHECK(other.midiQueueDepth() == Machine::kMidiQueueCapacity);
  CHECK(other.saveState() == snap);
}
