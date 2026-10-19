// MidiByteParser: whole messages from a raw MIDI byte stream
// (host/include/mirage/midi_byte_parser.h).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <vector>

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
