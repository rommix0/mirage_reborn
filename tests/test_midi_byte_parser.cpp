// MidiByteParser: whole messages from a raw MIDI byte stream
// (host/include/mirage/midi_byte_parser.h).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <vector>

#include "alloc_counter.h"
#include "mirage/midi_byte_parser.h"

using mirage::MidiByteParser;
using Msg = std::vector<uint8_t>;

namespace {
std::vector<Msg> parse(MidiByteParser& p, std::initializer_list<uint8_t> bytes) {
  std::vector<Msg> out;
  for (uint8_t b : bytes)
    p.feed(b, [&](std::span<const uint8_t> m) { out.emplace_back(m.begin(), m.end()); });
  return out;
}
std::vector<Msg> parse(std::initializer_list<uint8_t> bytes) {
  MidiByteParser p;
  return parse(p, bytes);
}
}  // namespace

TEST_CASE("note on with running status emits two messages", "[midi_parser]") {
  const auto out = parse({0x90, 0x3C, 0x64, 0x3E, 0x50});
  REQUIRE(out == std::vector<Msg>{{0x90, 0x3C, 0x64}, {0x90, 0x3E, 0x50}});
}

TEST_CASE("running status survives across feed calls and a new status replaces it",
          "[midi_parser]") {
  MidiByteParser p;
  REQUIRE(parse(p, {0x90, 0x3C}).empty());
  REQUIRE(parse(p, {0x64, 0x3E, 0x50, 0x80, 0x3C, 0x00, 0x3E, 0x00}) ==
          std::vector<Msg>{
              {0x90, 0x3C, 0x64}, {0x90, 0x3E, 0x50}, {0x80, 0x3C, 0x00}, {0x80, 0x3E, 0x00}});
}

TEST_CASE("real-time byte inside a message is emitted alone and the message completes",
          "[midi_parser]") {
  const auto out = parse({0x90, 0x3C, 0xF8, 0x64});
  REQUIRE(out == std::vector<Msg>{{0xF8}, {0x90, 0x3C, 0x64}});
  // Between running-status messages it does not disturb the status either.
  const auto out2 = parse({0x90, 0x3C, 0x64, 0xFE, 0x3E, 0x50});
  REQUIRE(out2 == std::vector<Msg>{{0x90, 0x3C, 0x64}, {0xFE}, {0x90, 0x3E, 0x50}});
}

TEST_CASE("sysex is emitted whole", "[midi_parser]") {
  REQUIRE(parse({0xF0, 0x0F, 0x01, 0x02, 0xF7}) ==
          std::vector<Msg>{{0xF0, 0x0F, 0x01, 0x02, 0xF7}});
  // Real-time bytes inside it are emitted alone and leave it intact.
  REQUIRE(parse({0xF0, 0x01, 0xF8, 0x02, 0xF7}) ==
          std::vector<Msg>{{0xF8}, {0xF0, 0x01, 0x02, 0xF7}});
}

TEST_CASE("a status byte interrupting sysex drops the partial sysex", "[midi_parser]") {
  // Chosen behaviour: the unfinished F0.. message is discarded (never emitted), then the
  // new message parses normally.
  REQUIRE(parse({0xF0, 0x01, 0x02, 0x90, 0x3C, 0x64}) == std::vector<Msg>{{0x90, 0x3C, 0x64}});
  // A new F0 restarts the sysex.
  REQUIRE(parse({0xF0, 0x01, 0xF0, 0x02, 0xF7}) == std::vector<Msg>{{0xF0, 0x02, 0xF7}});
  // A stray F7 outside sysex is dropped.
  REQUIRE(parse({0xF7}).empty());
}

TEST_CASE("stray data bytes are dropped", "[midi_parser]") {
  REQUIRE(parse({0x3C, 0x64, 0x10}).empty());
  // System common cancels running status, so data after it is stray again.
  REQUIRE(parse({0x90, 0x3C, 0x64, 0xF6, 0x3E, 0x50}) ==
          std::vector<Msg>{{0x90, 0x3C, 0x64}, {0xF6}});
  // End of sysex cancels running status too.
  REQUIRE(parse({0x90, 0x3C, 0x64, 0xF0, 0x01, 0xF7, 0x3E, 0x50}) ==
          std::vector<Msg>{{0x90, 0x3C, 0x64}, {0xF0, 0x01, 0xF7}});
}

TEST_CASE("program change is two bytes", "[midi_parser]") {
  REQUIRE(parse({0xC3, 0x05, 0x06}) == std::vector<Msg>{{0xC3, 0x05}, {0xC3, 0x06}});
  REQUIRE(parse({0xD0, 0x40}) == std::vector<Msg>{{0xD0, 0x40}});  // channel pressure
}

TEST_CASE("system common messages have their own lengths", "[midi_parser]") {
  REQUIRE(parse({0xF1, 0x23}) == std::vector<Msg>{{0xF1, 0x23}});
  REQUIRE(parse({0xF2, 0x01, 0x02}) == std::vector<Msg>{{0xF2, 0x01, 0x02}});
  REQUIRE(parse({0xF3, 0x07}) == std::vector<Msg>{{0xF3, 0x07}});
  REQUIRE(parse({0xF6}) == std::vector<Msg>{{0xF6}});
  REQUIRE(parse({0xF4, 0x01, 0xF5, 0x02}).empty());  // undefined statuses are dropped
}

TEST_CASE("a new status interrupting a partial channel message drops it", "[midi_parser]") {
  REQUIRE(parse({0x90, 0x3C, 0x80, 0x3C, 0x00}) == std::vector<Msg>{{0x80, 0x3C, 0x00}});
  REQUIRE(parse({0xE0, 0x00, 0x40}) == std::vector<Msg>{{0xE0, 0x00, 0x40}});  // pitch bend
}

TEST_CASE("midi parser: a bounded parser never allocates", "[midi_parser]") {
  MidiByteParser p(64 * 1024);
  size_t emitted = 0, sysexSize = 0;
  const std::function<void(std::span<const uint8_t>)> emit = [&](std::span<const uint8_t> m) {
    ++emitted;
    if (m[0] == 0xF0) sysexSize = m.size();
  };
  mirage_test::AllocationScope scope;
  p.feed(0xF0, emit);
  for (int i = 0; i < 60 * 1024 - 2; ++i) p.feed(0x01, emit);
  p.feed(0xF7, emit);
  const uint8_t notes[] = {0x90, 0x3C, 0x64, 0x3E, 0x50};
  for (uint8_t b : notes) p.feed(b, emit);
  CHECK(scope.count() == 0);
  CHECK(emitted == 3);
  CHECK(sysexSize == 60 * 1024);
  CHECK(p.dropped() == 0);
}

TEST_CASE("midi parser: a SysEx past the bound is dropped", "[midi_parser]") {
  MidiByteParser p(8);
  const std::vector<uint8_t> in = {0xF0, 1, 2, 3, 4, 5, 6, 0xF7,     // 8 bytes: fits
                                   0xF0, 1, 2, 3, 4, 5, 6, 7, 0xF7,  // 9: dropped
                                   0x90, 0x3C, 0x64};
  std::vector<Msg> out;
  for (uint8_t b : in)
    p.feed(b, [&](std::span<const uint8_t> m) { out.emplace_back(m.begin(), m.end()); });
  REQUIRE(out.size() == 2);
  CHECK(out[0].size() == 8);
  CHECK(out[1] == Msg{0x90, 0x3C, 0x64});
  CHECK(p.dropped() == 1);
  CHECK(MidiByteParser().dropped() == 0);  // the default stays unbounded
}

TEST_CASE("midi parser: tiny bounds never allocate and count what is cut off", "[midi_parser]") {
  const std::function<void(std::span<const uint8_t>)> emit = [](std::span<const uint8_t>) {};
  {  // maxSysex 2: F0 F7 fits, a channel message (3 bytes) still needs no allocation
    MidiByteParser p(2);
    mirage_test::AllocationScope scope;
    for (uint8_t b : {uint8_t(0x90), uint8_t(0x3C), uint8_t(0x64), uint8_t(0xF0), uint8_t(0xF7)})
      p.feed(b, emit);
    CHECK(scope.count() == 0);
    CHECK(p.dropped() == 0);
  }
  {  // maxSysex 1: even F0 F7 is too long
    MidiByteParser p(1);
    p.feed(0xF0, emit);
    p.feed(0xF7, emit);
    CHECK(p.dropped() == 1);
  }
  {  // an oversize SysEx ended by a status byte or a fresh F0 is counted too
    MidiByteParser p(4);
    for (uint8_t b : {uint8_t(0xF0), uint8_t(1), uint8_t(2), uint8_t(3), uint8_t(4)}) p.feed(b, emit);
    p.feed(0x90, emit);
    CHECK(p.dropped() == 1);
    for (uint8_t b : {uint8_t(0xF0), uint8_t(1), uint8_t(2), uint8_t(3), uint8_t(4)}) p.feed(b, emit);
    p.feed(0xF0, emit);
    CHECK(p.dropped() == 2);
    p.feed(0xF7, emit);  // that F0 F7 pair fits
    CHECK(p.dropped() == 2);
  }
}
