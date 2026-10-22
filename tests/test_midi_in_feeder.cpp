// MidiInFeeder (host/include/mirage/midi_in_feeder.h): long MIDI input is paced
// to the machine instead of overflowing its 4096-byte input ring.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "mirage/midi_in_feeder.h"
#include "mirage/realtime_host.h"
#include "test_roms.h"

using namespace mirage;
using mirage_test::kAciaReceiver;
using mirage_test::romFrom;
using mirage_test::romWith;

namespace {

constexpr size_t kFrames = 256;
constexpr double kRate = 48000.0;

// A host and its feeder, driven from the test thread: pump() between blocks.
struct Rig {
  RealtimeHost host;
  MidiInFeeder feeder{host};
  double now = 1000.0;
  size_t maxDepth = 0;  // the largest Machine MIDI queue depth after any block
  size_t maxFlight = 0;  // the largest (pushed - taken) + depth

  explicit Rig(const std::vector<uint8_t>& rom) : host(rom) { host.prepare(kFrames, kRate); }

  void block() {
    std::vector<float> out(kFrames);
    feeder.pump();
    host.process(out.data(), kFrames, kRate, now);
    now += double(kFrames) / kRate;
    maxDepth = std::max(maxDepth, host.machineForTest().midiQueueDepth());
  }
  void run(int blocks) {
    for (int i = 0; i < blocks; ++i) block();
  }
  // Until the feeder has nothing left and the host has taken `total` bytes.
  void runUntilTaken(uint64_t total, int maxBlocks) {
    for (int i = 0; i < maxBlocks && host.status().midiBytesTaken < total; ++i) block();
  }
};

std::vector<uint8_t> sysex(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = uint8_t(0x10 + i % 0x60);
  v.front() = 0xF0;
  v.back() = 0xF7;
  return v;
}

const std::vector<uint8_t> kIdleRom = romWith({0x20, 0xFE});  // BRA *

}  // namespace

TEST_CASE("feeder delivers a 200 KB SysEx whole and in order") {
  Rig rig(kIdleRom);
  const auto msg = sysex(200000);
  rig.feeder.push(msg, rig.now);
  CHECK(rig.feeder.backlog() > 100000);  // most of it waits
  // 200000 bytes at 320 us each is 64 s; 12000 blocks of 5.3 ms is 64 s.
  rig.runUntilTaken(msg.size(), 14000);
  CHECK(rig.host.status().midiBytesTaken == msg.size());
  CHECK(rig.feeder.backlog() == 0);
  CHECK(rig.host.machineForTest().midiOverflows() == 0);
  CHECK(rig.host.status().midiDropped == 0);
  CHECK(rig.feeder.dropped() == 0);
  CHECK(rig.maxDepth <= MidiInFeeder::kMaxInFlight);
}

TEST_CASE("feeder keeps a note after a long SysEx behind it") {
  // The receiver ROM stores each byte it reads at $8000, X+, so the stream shows
  // in RAM in the order the ACIA delivered it.
  Rig rig(romFrom({kAciaReceiver}));
  rig.run(2);  // the receiver sets up the ACIA
  auto stream = sysex(3500);
  const std::vector<uint8_t> note = {0x90, 60, 100};
  rig.feeder.push(stream, rig.now);
  rig.feeder.push(note, rig.now);  // behind the dump: the backlog is not empty
  stream.insert(stream.end(), note.begin(), note.end());
  for (int i = 0; i < 1000 && rig.host.status().midiBytesTaken < stream.size(); ++i) {
    rig.block();
    // The note's three bytes are the last ones the host takes.
    CHECK((rig.host.status().midiBytesTaken <= stream.size() - 3 || rig.feeder.backlog() == 0));
  }
  rig.run(400);  // the last bytes through the 320 us wire
  REQUIRE(rig.host.status().midiBytesTaken == stream.size());
  CHECK(rig.host.machineForTest().midiOverflows() == 0);
  for (size_t i = 0; i < stream.size(); ++i)
    REQUIRE(rig.host.machineForTest().peek(uint16_t(0x8000 + i)) == stream[i]);
}

TEST_CASE("feeder never has more than kMaxInFlight bytes in flight") {
  Rig rig(kIdleRom);
  rig.feeder.push(sysex(60000), rig.now);
  uint64_t pushed = 60000;
  for (int i = 0; i < 1500; ++i) {
    if (i % 100 == 50) {  // notes keep arriving meanwhile
      const std::vector<uint8_t> note = {0x90, 60, 100};
      rig.feeder.push(note, rig.now);
      pushed += 3;
    }
    rig.block();
    const HostStatus s = rig.host.status();
    // Posted so far = pushed - backlog; of that, taken or still queued.
    const uint64_t posted = pushed - rig.feeder.backlog();
    const uint64_t inFlight = posted - s.midiBytesTaken + s.midiQueueDepth;
    CHECK(inFlight <= MidiInFeeder::kMaxInFlight);
    CHECK(s.midiQueueDepth <= MidiInFeeder::kMaxInFlight);
  }
  CHECK(rig.maxDepth <= MidiInFeeder::kMaxInFlight);
  CHECK(rig.maxDepth > 1500);  // and it does use the room
  CHECK(rig.host.machineForTest().midiOverflows() == 0);
}

TEST_CASE("feeder clear drops the backlog") {
  Rig rig(kIdleRom);
  rig.feeder.push(sysex(100000), rig.now);
  rig.run(10);
  const uint64_t taken = rig.host.status().midiBytesTaken;
  REQUIRE(rig.feeder.backlog() > 90000);
  rig.feeder.clear();
  CHECK(rig.feeder.backlog() == 0);
  CHECK(rig.feeder.dropped() == 0);  // a deliberate clear is not a drop
  REQUIRE(rig.host.postPanic());
  rig.run(20);
  // Nothing more from the dump; only the panic's 48 bytes enter the machine.
  const HostStatus s = rig.host.status();
  CHECK(s.midiBytesTaken == taken);
  CHECK(s.midiQueueDepth <= 48);
  CHECK(s.midiDropped == 0);
  // The flow accounting recovers: a fresh message goes out at once.
  const std::vector<uint8_t> note = {0x90, 60, 100};
  rig.feeder.push(note, rig.now);
  rig.run(2);
  CHECK(rig.host.status().midiBytesTaken == taken + 3);
}

TEST_CASE("feeder clear after posted-but-untaken bytes does not stall the flow") {
  // Bytes in the host queue when the panic arrives are discarded, never taken.
  Rig rig(kIdleRom);
  rig.feeder.push(sysex(10000), rig.now);
  rig.feeder.pump();  // posts 2048 bytes; no block has run
  rig.feeder.push(sysex(50000), rig.now);
  rig.feeder.clear();
  REQUIRE(rig.host.postPanic());
  rig.run(20);
  const std::vector<uint8_t> note = {0x90, 60, 100};
  rig.feeder.push(sysex(5000), rig.now);
  rig.feeder.push(note, rig.now);
  rig.runUntilTaken(rig.host.status().midiBytesTaken + 5003, 1500);
  CHECK(rig.feeder.backlog() == 0);
  CHECK(rig.host.machineForTest().midiOverflows() == 0);
}

TEST_CASE("feeder drops a message bigger than kMaxBacklog and counts it") {
  Rig rig(kIdleRom);
  const auto huge = sysex(MidiInFeeder::kMaxBacklog + 1);
  rig.feeder.push(huge, rig.now);
  CHECK(rig.feeder.dropped() == 1);
  CHECK(rig.feeder.droppedBytes() == huge.size());
  CHECK(rig.feeder.backlog() == 0);
  // One that exactly fills the cap is kept; the next that does not fit is not.
  rig.feeder.push(sysex(MidiInFeeder::kMaxBacklog), rig.now);
  CHECK(rig.feeder.dropped() == 1);
  const std::vector<uint8_t> note = {0x90, 60, 100};
  rig.feeder.push(note, rig.now);
  CHECK(rig.feeder.dropped() == 2);
  CHECK(rig.feeder.droppedBytes() == huge.size() + 3);
  CHECK(rig.feeder.backlog() > MidiInFeeder::kMaxBacklog - 3000);
}

TEST_CASE("feeder: short messages with an empty backlog keep their timestamps") {
  // Same setup as "host places MIDI by timestamp": the saved state shows when
  // the byte was queued.
  const auto rom = romFrom({kAciaReceiver});
  Rig rig(rom);
  Machine ref(rom);
  ref.powerOn();
  ref.prepare(kFrames, kRate);
  std::vector<float> want(kFrames);
  for (int i = 0; i < 4; ++i) {
    rig.block();
    ref.render(want.data(), kFrames, kRate);
  }
  REQUIRE(rig.host.machineForTest().saveState() == ref.saveState());
  const std::vector<uint8_t> on = {0x90, 60, 100};
  // Stamped 2 ms before the block's `now`: 96 frames before its end.
  rig.feeder.push(on, rig.now - 0.002);
  CHECK(rig.feeder.backlog() == 0);
  rig.block();
  ref.render(want.data(), kFrames - 96, kRate);
  ref.queueMidi(on);
  ref.render(want.data(), 96, kRate);
  CHECK(rig.host.machineForTest().saveState() == ref.saveState());
}

TEST_CASE("host status counts MIDI bytes taken, discarded and flushed") {
  RealtimeHost host(kIdleRom);
  host.prepare(kFrames, kRate);
  std::vector<float> out(kFrames);
  const std::vector<uint8_t> a(30, 0xF8), b(20, 0xF8);
  REQUIRE(host.postMidi(a, 0.0));
  host.process(out.data(), kFrames, kRate, 0.0);
  CHECK(host.status().midiBytesTaken == 30);
  CHECK(host.status().midiQueueDepth <= 30);
  CHECK(host.status().midiQueueDepth >= 29);
  // An invalid rate discards (and counts the loss in midiDropped).
  REQUIRE(host.postMidi(b, 0.0));
  host.process(out.data(), kFrames, 0.0, 0.0);
  CHECK(host.status().midiBytesTaken == 50);
  CHECK(host.status().midiDropped == 20);
  // A panic flush is neither taken nor dropped.
  REQUIRE(host.postMidi(b, 0.0));
  REQUIRE(host.postPanic());
  host.process(out.data(), kFrames, kRate, 0.0);
  CHECK(host.status().midiBytesTaken == 50);
  CHECK(host.status().midiBytesFlushed == 20);
  CHECK(host.status().midiDropped == 20);
}
