// mirage_cli run: option parsing and the event schedule (tools/mirage_cli/run_options.*).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>
#include <vector>

#include "mirage/clocks.h"
#include "mirage/machine.h"
#include "run_options.h"

using namespace mirage;
using cli::RunEvent;

namespace {

constexpr uint64_t kSecond = kCpuClockHz;
using Kind = RunEvent::Kind;

cli::RunOptions parseOk(std::vector<std::string> args) {
  auto r = cli::parseRunArgs(args, "default.bin");
  REQUIRE(std::holds_alternative<cli::RunOptions>(r));
  return std::get<cli::RunOptions>(r);
}

cli::RunArgsError parseFails(std::vector<std::string> args) {
  auto r = cli::parseRunArgs(args, "default.bin");
  REQUIRE(std::holds_alternative<cli::RunArgsError>(r));
  return std::get<cli::RunArgsError>(r);
}

// The key events of a schedule as (cycle, down?) pairs.
std::vector<std::pair<uint64_t, bool>> keyEdges(const cli::RunOptions& o, PanelKey k) {
  std::vector<std::pair<uint64_t, bool>> out;
  for (const auto& e : o.events)
    if ((e.kind == Kind::KeyDown || e.kind == Kind::KeyUp) && e.key == k)
      out.emplace_back(e.cycle, e.kind == Kind::KeyDown);
  return out;
}

}  // namespace

TEST_CASE("cli run defaults: 10 s, default ROM, no disk, no events") {
  const auto o = parseOk({});
  CHECK(o.endCycle == 10 * kSecond);
  CHECK(o.rom == "default.bin");
  CHECK(o.disk.empty());
  CHECK(o.trace.empty());
  CHECK(o.events.empty());
}

TEST_CASE("cli run --key presses at T and releases after HOLD (default 0.1 s)") {
  const auto o = parseOk({"--key", "enter@1", "--key", "LoadUpper@2:0.5"});
  CHECK(keyEdges(o, PanelKey::Enter) ==
        std::vector<std::pair<uint64_t, bool>>{{kSecond, true}, {kSecond + kSecond / 10, false}});
  CHECK(keyEdges(o, PanelKey::LoadUpper) ==
        std::vector<std::pair<uint64_t, bool>>{{2 * kSecond, true}, {5 * kSecond / 2, false}});
}

TEST_CASE("cli run overlapping and touching holds of one key are one press") {
  using Edges = std::vector<std::pair<uint64_t, bool>>;
  SECTION("a shorter hold inside a longer one does not release early") {
    const auto o = parseOk({"--key", "Enter@1:2", "--key", "Enter@2:0.5"});
    CHECK(keyEdges(o, PanelKey::Enter) == Edges{{kSecond, true}, {3 * kSecond, false}});
  }
  SECTION("the same, given in the other order") {
    const auto o = parseOk({"--key", "Enter@2:0.5", "--key", "Enter@1:2"});
    CHECK(keyEdges(o, PanelKey::Enter) == Edges{{kSecond, true}, {3 * kSecond, false}});
  }
  SECTION("back-to-back holds merge") {
    const auto o = parseOk({"--key", "Enter@2:1", "--key", "Enter@1:1"});
    CHECK(keyEdges(o, PanelKey::Enter) == Edges{{kSecond, true}, {3 * kSecond, false}});
  }
  SECTION("separate holds stay separate presses, and other keys are independent") {
    const auto o = parseOk({"--key", "Enter@1:0.5", "--key", "Enter@2:0.5", "--key", "Cancel@1:5"});
    CHECK(keyEdges(o, PanelKey::Enter) == Edges{{kSecond, true},
                                                 {3 * kSecond / 2, false},
                                                 {2 * kSecond, true},
                                                 {5 * kSecond / 2, false}});
    CHECK(keyEdges(o, PanelKey::Cancel) == Edges{{kSecond, true}, {6 * kSecond, false}});
  }
}

TEST_CASE("cli run --note queues note-on at T and note-off at T+DUR") {
  const auto o = parseOk({"--note", "60:100@1:0.5"});
  REQUIRE(o.events.size() == 2);
  CHECK(o.events[0].cycle == kSecond);
  CHECK(o.events[0].kind == Kind::Midi);
  CHECK(o.events[0].midi == std::vector<uint8_t>{0x90, 60, 100});
  CHECK(o.events[1].cycle == 3 * kSecond / 2);
  CHECK(o.events[1].midi == std::vector<uint8_t>{0x80, 60, 0});
}

TEST_CASE("cli run --midi-hex takes spaced or comma-separated bytes; ties keep order") {
  const auto o = parseOk({"--midi-hex", "F8,fe@2", "--midi-hex", "90 3C 64@2", "--midi-hex", "B0 01 7F@1"});
  REQUIRE(o.events.size() == 3);
  CHECK(o.events[0].cycle == kSecond);
  CHECK(o.events[0].midi == std::vector<uint8_t>{0xB0, 0x01, 0x7F});
  CHECK(o.events[1].midi == std::vector<uint8_t>{0xF8, 0xFE});
  CHECK(o.events[2].midi == std::vector<uint8_t>{0x90, 0x3C, 0x64});
}

TEST_CASE("cli run drops events after --seconds") {
  const auto o = parseOk({"--seconds", "2", "--midi-hex", "F8@3", "--key", "Enter@1.5:1",
                          "--note", "60:1@1:5"});
  CHECK(o.endCycle == 2 * kSecond);
  for (const auto& e : o.events) CHECK(e.cycle <= o.endCycle);
  // What is left: Enter down at 1.5 s, note-on at 1 s (release and note-off are later).
  REQUIRE(o.events.size() == 2);
  CHECK(o.events[0].kind == Kind::Midi);
  CHECK(o.events[1].kind == Kind::KeyDown);
}

TEST_CASE("cli run --trace-from schedules the trace before other events at that time") {
  const auto o = parseOk({"--trace", "t.txt", "--trace-from", "1", "--midi-hex", "F8@1"});
  REQUIRE(o.events.size() == 2);
  CHECK(o.events[0].kind == Kind::TraceOn);
  CHECK(o.events[0].cycle == kSecond);
  CHECK(o.events[1].kind == Kind::Midi);
}

TEST_CASE("cli run rejects bad values with exit code 2") {
  const std::vector<std::vector<std::string>> bad = {
      {"--seconds", "-1"},       {"--seconds", "abc"},      {"--seconds", ""},
      {"--key", "bogus@1"},      {"--key", "Enter"},        {"--key", "Enter@x"},
      {"--key", "Enter@1:-2"},   {"--note", "60:100@1"},    {"--note", "128:1@1:1"},
      {"--note", "60:-1@1:1"},   {"--note", "60@1:1"},      {"--midi-hex", "ZZ@1"},
      {"--midi-hex", "100@1"},   {"--midi-hex", "@1"},      {"--midi-hex", "90 3C"},
      {"--trace-from", "-0.5"}};
  for (const auto& args : bad) {
    INFO(args[0] << " " << args[1]);
    const auto e = parseFails(args);
    CHECK(e.exitCode == 2);
    CHECK(e.message == "bad value for " + args[0] + ": " + args[1]);
  }
}

TEST_CASE("cli run usage errors") {
  SECTION("unknown option") {
    const auto e = parseFails({"--bogus", "1"});
    CHECK(e.exitCode == 2);
    CHECK(e.message.empty());  // print usage
  }
  SECTION("missing value") {
    const auto e = parseFails({"--disk"});
    CHECK(e.exitCode == 2);
    CHECK(e.message.empty());
  }
}

TEST_CASE("cli run --snapshot-in and --snapshot-out") {
  SECTION("defaults: neither") {
    const auto o = parseOk({});
    CHECK(o.snapshotIn.empty());
    CHECK(o.snapshotOut.empty());
  }
  SECTION("both given, with events relative to the loaded state") {
    const auto o = parseOk({"--snapshot-in", "a.snap", "--snapshot-out", "b.snap", "--seconds",
                            "2", "--note", "60:100@0.5:1"});
    CHECK(o.snapshotIn == "a.snap");
    CHECK(o.snapshotOut == "b.snap");
    REQUIRE(o.events.size() == 2);
    CHECK(o.events[0].cycle == kSecond / 2);
    CHECK(o.endCycle == 2 * kSecond);
  }
  SECTION("empty file names are usage errors") {
    for (const char* opt : {"--snapshot-in", "--snapshot-out"}) {
      const auto e = parseFails({opt, ""});
      CHECK(e.exitCode == 2);
      CHECK(e.message == std::string("bad value for ") + opt + ": ");
    }
  }
  SECTION("--disk cannot be combined with --snapshot-in, but can with --snapshot-out") {
    const auto e = parseFails({"--disk", "d.img", "--snapshot-in", "a.snap"});
    CHECK(e.exitCode == 2);
    CHECK(e.message ==
          "--disk cannot be combined with --snapshot-in: the snapshot holds its own disk");
    const auto o = parseOk({"--disk", "d.img", "--snapshot-out", "b.snap"});
    CHECK(o.disk == "d.img");
  }
}

TEST_CASE("cli run --save-disk") {
  CHECK(parseOk({}).saveDisk.empty());
  const auto o = parseOk({"--disk", "d.img", "--save-disk", "out.img"});
  CHECK(o.disk == "d.img");
  CHECK(o.saveDisk == "out.img");
  const auto e = parseFails({"--save-disk", ""});
  CHECK(e.exitCode == 2);
  CHECK(e.message == "bad value for --save-disk: ");
}

TEST_CASE("cli run --wav and --rate") {
  SECTION("defaults: no WAV, 48 kHz") {
    const auto o = parseOk({});
    CHECK(o.wav.empty());
    CHECK(o.rate == 48000);
  }
  SECTION("both given") {
    const auto o = parseOk({"--wav", "out.wav", "--rate", "44100"});
    CHECK(o.wav == "out.wav");
    CHECK(o.rate == 44100);
  }
  SECTION("bad values are usage errors") {
    for (const char* rate : {"0", "999", "768001", "-5", "abc", "", "44100.5"}) {
      INFO(rate);
      const auto e = parseFails({"--rate", rate});
      CHECK(e.exitCode == 2);
      CHECK(e.message == std::string("bad value for --rate: ") + rate);
    }
    CHECK(parseFails({"--wav", ""}).message == "bad value for --wav: ");
  }
}

TEST_CASE("cli runSchedule with audio renders every sample and lands events on time") {
  std::vector<uint8_t> rom(4096, 0x12);
  rom[0] = 0x20;
  rom[1] = 0xFE;
  rom[0xFFE] = 0xF0;
  rom[0xFFF] = 0x00;
  Machine m(rom);
  m.powerOn();
  const auto o = parseOk({"--seconds", "0.5", "--rate", "44100", "--wav", "x.wav", "--midi-hex",
                          "F8@0.1", "--key", "Enter@0.25:0.01"});
  std::vector<float> audio;
  cli::runSchedule(m, o, nullptr, &audio);
  CHECK(audio.size() == 22050);
  CHECK(m.cycles() >= o.endCycle);
  CHECK(m.cycles() < o.endCycle + 8);
}

TEST_CASE("cli runSchedule runs to the end in slices and applies events") {
  // $F000: BRA * ; enough for timing.
  std::vector<uint8_t> rom(4096, 0x12);
  rom[0] = 0x20;
  rom[1] = 0xFE;
  rom[0xFFE] = 0xF0;
  rom[0xFFF] = 0x00;
  Machine m(rom);
  m.powerOn();
  const auto o = parseOk({"--seconds", "0.01", "--trace", "x", "--trace-from", "0.005",
                          "--midi-hex", "F8@0.001", "--key", "Enter@0.002:0.001"});
  uint64_t traced = 0, firstTraceCycle = 0;
  cli::runSchedule(m, o, [&](uint16_t, const Regs&) {
    if (traced++ == 0) firstTraceCycle = m.cycles();
  });
  CHECK(m.cycles() >= o.endCycle);
  CHECK(m.cycles() < o.endCycle + 8);  // BRA is 3 cycles
  CHECK(traced > 0);
  CHECK(firstTraceCycle >= kSecond / 200);
  CHECK(firstTraceCycle < kSecond / 200 + 8);
  // The trace is removed at the end.
  const uint64_t before = traced;
  m.runCycles(100);
  CHECK(traced == before);
}
