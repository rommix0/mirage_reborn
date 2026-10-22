// mirage_cli run: option parsing and the event schedule (tools/mirage_cli/run_options.*).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "mirage/clocks.h"
#include "mirage/machine.h"
#include "mirage/wav_writer.h"
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

// ---- --import ---------------------------------------------------------------

namespace {
const RunEvent& onlyImport(const cli::RunOptions& o) {
  const RunEvent* found = nullptr;
  for (const RunEvent& e : o.events)
    if (e.kind == Kind::Import) {
      REQUIRE(found == nullptr);
      found = &e;
    }
  REQUIRE(found != nullptr);
  return *found;
}

std::vector<uint8_t> idleRom() {
  std::vector<uint8_t> rom(4096, 0x12);
  rom[0] = 0x20;
  rom[1] = 0xFE;
  rom[0xFFE] = 0xF0;
  rom[0xFFF] = 0x00;
  return rom;
}
}  // namespace

TEST_CASE("cli run --import: defaults") {
  const auto o = parseOk({"--import", "a.wav@2.5"});
  const RunEvent& e = onlyImport(o);
  CHECK(e.file == "a.wav");
  CHECK(e.cycle == 5 * kSecond / 2);
  CHECK(e.import.rate == kDefaultImportRate);
  CHECK(e.import.startSeconds == 0);
  CHECK(e.import.endSeconds == 0);
  CHECK(e.import.half == WaveHalf::Lower);
  CHECK(e.import.startPage == 0);
  CHECK(e.import.normalize);
}

TEST_CASE("cli run --import: every field") {
  const auto o = parseOk({"--import",
                          "C:/some dir/a@b.wav@1,rate=20000,start=0.25,end=1.5,half=upper,"
                          "page=17,normalize=0"});
  const RunEvent& e = onlyImport(o);
  CHECK(e.file == "C:/some dir/a@b.wav");
  CHECK(e.cycle == kSecond);
  CHECK(e.import.rate == 20000);
  CHECK(e.import.startSeconds == 0.25);
  CHECK(e.import.endSeconds == 1.5);
  CHECK(e.import.half == WaveHalf::Upper);
  CHECK(e.import.startPage == 17);
  CHECK_FALSE(e.import.normalize);
  CHECK(onlyImport(parseOk({"--import", "a.wav@0,half=lower,normalize=1"})).import.normalize);
}

TEST_CASE("cli run --import is repeatable and sorted with the other events") {
  const auto o = parseOk({"--seconds", "5", "--note", "60:100@2:1", "--import", "b.wav@3",
                          "--import", "a.wav@1", "--import", "c.wav@1"});
  std::vector<std::pair<uint64_t, std::string>> got;
  for (const RunEvent& e : o.events)
    got.emplace_back(e.cycle, e.kind == Kind::Import ? e.file : std::string("midi"));
  CHECK(got == std::vector<std::pair<uint64_t, std::string>>{{kSecond, "a.wav"},
                                                             {kSecond, "c.wav"},
                                                             {2 * kSecond, "midi"},
                                                             {3 * kSecond, "b.wav"},
                                                             {3 * kSecond, "midi"}});
}

TEST_CASE("cli run --import: bad values are usage errors") {
  for (const char* v : {"", "a.wav", "@1", "a.wav@", "a.wav@x", "a.wav@-1", "a.wav@1,",
                        "a.wav@1,rate", "a.wav@1,rate=", "a.wav@1,rate=fast",
                        "a.wav@1,start=-1", "a.wav@1,end=x", "a.wav@1,half=middle",
                        "a.wav@1,page=256", "a.wav@1,page=-1", "a.wav@1,page=1.5",
                        "a.wav@1,normalize=2", "a.wav@1,normalize=yes", "a.wav@1,volume=3",
                        "a.wav@1,,rate=20000"}) {
    INFO(v);
    const auto e = parseFails({"--import", v});
    CHECK(e.exitCode == 2);
    CHECK(e.message == std::string("bad value for --import: ") + v);
  }
}

TEST_CASE("cli prepareImports reports a missing file and refused settings") {
  auto o = parseOk({"--import", "no-such-file.wav@1"});
  CHECK(cli::prepareImports(o) == "no-such-file.wav: The WAV file could not be opened.");

  const auto path = std::filesystem::temp_directory_path() / "mirage_cli_import_test.wav";
  writeWav(path, std::vector<float>(22050, 0.5f), 22050);  // 1 s
  const std::string file = path.generic_string();
  o = parseOk({"--import", file + "@1,rate=5000"});
  CHECK(cli::prepareImports(o) == file + ": The sample rate must be between 10101 and 33333 Hz.");
  o = parseOk({"--import", file + "@1,start=2"});
  CHECK(cli::prepareImports(o) == file + ": The start point is past the end of the file.");
  std::filesystem::remove(path);
}

TEST_CASE("cli runSchedule writes an import into wave RAM at its time and logs it") {
  const auto path = std::filesystem::temp_directory_path() / "mirage_cli_import_test2.wav";
  writeWav(path, std::vector<float>(22050, 0.5f), 22050);  // 1 s, constant
  const std::string file = path.generic_string();
  auto o = parseOk({"--seconds", "0.2", "--import",
                    file + "@0.1,rate=20000,end=0.5,half=upper,page=2"});
  REQUIRE(cli::prepareImports(o).empty());
  std::filesystem::remove(path);

  Machine m(idleRom());
  m.powerOn();
  std::ostringstream log;
  cli::runSchedule(m, o, nullptr, nullptr, &log);
  CHECK(log.str() == "import: " + file + " 10000 bytes at $10200, last page 41\n");
  // A constant +0.5 normalised (its peak is the filter's overshoot at the file's start
  // edge) lands near the top; the bytes around the range stay as they were.
  CHECK(m.sampleRam(0x101FF) == 0);
  CHECK(m.sampleRam(0x10200 + 5000) > 0xE0);
  CHECK(m.sampleRam(0x10200 + 10000) == 0);
  CHECK(m.sampleRam(0) == 0);
}

TEST_CASE("cli runSchedule logs a truncated import") {
  const auto path = std::filesystem::temp_directory_path() / "mirage_cli_import_test3.wav";
  writeWav(path, std::vector<float>(66000, 0.25f), 22050);  // 3 s
  const std::string file = path.generic_string();
  auto o = parseOk({"--seconds", "0.01", "--import", file + "@0,rate=30000,page=250"});
  REQUIRE(cli::prepareImports(o).empty());
  std::filesystem::remove(path);
  Machine m(idleRom());
  m.powerOn();
  std::ostringstream log;
  cli::runSchedule(m, o, nullptr, nullptr, &log);
  CHECK(log.str() == "import: " + file + " 1536 bytes at $0FA00, last page 255, truncated\n");
}

TEST_CASE("cli runSchedule refuses an import while the drive is busy") {
  const auto path = std::filesystem::temp_directory_path() / "mirage_cli_import_test4.wav";
  writeWav(path, std::vector<float>(22050, 0.5f), 22050);
  const std::string file = path.generic_string();
  auto o = parseOk({"--seconds", "0.01", "--import", file + "@0.005"});
  REQUIRE(cli::prepareImports(o).empty());
  std::filesystem::remove(path);

  Machine m(idleRom());
  m.powerOn();
  m.debugWrite(0xE202, 0x1F);  // VIA DDRB; PB4 low = drive motor on
  m.debugWrite(0xE200, 0x00);
  REQUIRE(m.driveActive());
  std::ostringstream log;
  cli::runSchedule(m, o, nullptr, nullptr, &log);
  CHECK(log.str() == "import: " + file + " refused, the drive is busy\n");
  CHECK(m.sampleRam(0) == 0);
}

// ---- --audio-in -------------------------------------------------------------

namespace {
const RunEvent& audioInAt(const cli::RunOptions& o, size_t nth) {
  size_t seen = 0;
  for (const RunEvent& e : o.events)
    if (e.kind == Kind::AudioIn && seen++ == nth) return e;
  FAIL("no such --audio-in entry");
  return o.events.front();
}

// A mono WAV of `frames` samples, ramping 0 to just under 1, in the temp folder.
std::string rampWav(const char* name, size_t frames, int rate) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::vector<float> v(frames);
  for (size_t i = 0; i < frames; ++i) v[i] = float(i) / float(frames);
  writeWav(path, v, rate);
  return path.generic_string();
}
}  // namespace

TEST_CASE("cli run --audio-in: defaults") {
  const auto o = parseOk({"--audio-in", "a.wav@2.5"});
  REQUIRE(o.events.size() == 1);
  const RunEvent& e = audioInAt(o, 0);
  CHECK(e.file == "a.wav");
  CHECK(e.cycle == 5 * kSecond / 2);
  CHECK(e.gain == 1.0);
  CHECK_FALSE(e.loop);
}

TEST_CASE("cli run --audio-in: every field") {
  const auto o = parseOk({"--audio-in", "C:/some dir/a@b.wav@1,gain=0.25,loop=1"});
  const RunEvent& e = audioInAt(o, 0);
  CHECK(e.file == "C:/some dir/a@b.wav");
  CHECK(e.cycle == kSecond);
  CHECK(e.gain == 0.25);
  CHECK(e.loop);
  CHECK_FALSE(audioInAt(parseOk({"--audio-in", "a.wav@0,loop=0,gain=3"}), 0).loop);
  CHECK(audioInAt(parseOk({"--audio-in", "a.wav@0,loop=0,gain=3"}), 0).gain == 3.0);
}

TEST_CASE("cli run --audio-in is repeatable and sorted with the other events") {
  const auto o = parseOk({"--seconds", "5", "--note", "60:100@2:1", "--audio-in", "b.wav@3",
                          "--audio-in", "a.wav@1", "--audio-in", "c.wav@1"});
  std::vector<std::pair<uint64_t, std::string>> got;
  for (const RunEvent& e : o.events)
    got.emplace_back(e.cycle, e.kind == Kind::AudioIn ? e.file : std::string("midi"));
  CHECK(got == std::vector<std::pair<uint64_t, std::string>>{{kSecond, "a.wav"},
                                                             {kSecond, "c.wav"},
                                                             {2 * kSecond, "midi"},
                                                             {3 * kSecond, "b.wav"},
                                                             {3 * kSecond, "midi"}});
}

TEST_CASE("cli run --audio-in: bad values are usage errors") {
  for (const char* v : {"", "a.wav", "@1", "a.wav@", "a.wav@x", "a.wav@-1", "a.wav@1,",
                        "a.wav@1,gain", "a.wav@1,gain=", "a.wav@1,gain=loud", "a.wav@1,gain=-1",
                        "a.wav@1,loop", "a.wav@1,loop=", "a.wav@1,loop=2", "a.wav@1,loop=yes",
                        "a.wav@1,rate=22050", "a.wav@1,,gain=1"}) {
    INFO(v);
    const auto e = parseFails({"--audio-in", v});
    CHECK(e.exitCode == 2);
    CHECK(e.message == std::string("bad value for --audio-in: ") + v);
  }
}

TEST_CASE("cli prepareAudioIn reports a missing file") {
  auto o = parseOk({"--audio-in", "no-such-file.wav@1"});
  CHECK(cli::prepareAudioIn(o) == "no-such-file.wav: The WAV file could not be opened.");
}

TEST_CASE("cli prepareAudioIn resamples to --rate and applies the gain") {
  const std::string file = rampWav("mirage_cli_audio_in_test.wav", 1000, 22050);
  SECTION("the same rate: the file's samples times the gain") {
    auto o = parseOk({"--rate", "22050", "--audio-in", file + "@0,gain=0.5"});
    REQUIRE(cli::prepareAudioIn(o).empty());
    const auto& s = audioInAt(o, 0).samples;
    REQUIRE(s.size() == 1000);
    CHECK(s[0] == 0.0f);
    CHECK(s[500] == Catch::Approx(0.25f));
    CHECK(s[999] == Catch::Approx(0.4995f));
  }
  SECTION("twice the rate: twice the frames, a linear ramp through the same values") {
    auto o = parseOk({"--rate", "44100", "--audio-in", file + "@0"});
    REQUIRE(cli::prepareAudioIn(o).empty());
    const auto& s = audioInAt(o, 0).samples;
    REQUIRE(s.size() == 2000);
    CHECK(s[1000] == Catch::Approx(0.5f).margin(1e-5));
    CHECK(s[1001] == Catch::Approx(0.5005f).margin(1e-5));  // halfway to the next sample
    CHECK(s[1999] == Catch::Approx(0.999f).margin(1e-5));  // past the last sample it holds
  }
  SECTION("half the rate") {
    auto o = parseOk({"--rate", "11025", "--audio-in", file + "@0"});
    REQUIRE(cli::prepareAudioIn(o).empty());
    const auto& s = audioInAt(o, 0).samples;
    REQUIRE(s.size() == 500);
    CHECK(s[250] == Catch::Approx(0.5f).margin(1e-5));
  }
  std::filesystem::remove(file);
}

TEST_CASE("cli runSchedule plays --audio-in from its time and logs the frames played") {
  const std::string file = rampWav("mirage_cli_audio_in_test2.wav", 22050, 22050);  // 1 s
  auto o = parseOk({"--seconds", "0.5", "--rate", "22050", "--audio-in", file + "@0.1"});
  REQUIRE(cli::prepareAudioIn(o).empty());
  SECTION("without --wav the run still renders the input") {
    Machine m(idleRom());
    m.powerOn();
    std::ostringstream log;
    cli::runSchedule(m, o, nullptr, nullptr, &log);
    CHECK(log.str() == "audio-in: " + file + " 8820 frames played\n");  // 0.1 s to 0.5 s
    CHECK(m.jackConnected());
    CHECK(m.cycles() >= o.endCycle);
    CHECK(m.cycles() < o.endCycle + 8);
  }
  SECTION("with --wav the output is the usual length") {
    Machine m(idleRom());
    m.powerOn();
    std::vector<float> audio;
    std::ostringstream log;
    cli::runSchedule(m, o, nullptr, &audio, &log);
    CHECK(audio.size() == 11025);
    CHECK(log.str() == "audio-in: " + file + " 8820 frames played\n");
  }
  std::filesystem::remove(file);
}

TEST_CASE("cli runSchedule: a later --audio-in replaces the earlier; loop repeats; a short "
          "file leaves the jack silent") {
  const std::string longFile = rampWav("mirage_cli_audio_in_test3a.wav", 22050, 22050);  // 1 s
  const std::string shortFile = rampWav("mirage_cli_audio_in_test3b.wav", 5000, 22050);
  auto o = parseOk({"--seconds", "1", "--rate", "22050", "--audio-in", longFile + "@0",
                    "--audio-in", shortFile + "@0.2,loop=1", "--audio-in", shortFile + "@0.6"});
  REQUIRE(cli::prepareAudioIn(o).empty());
  Machine m(idleRom());
  m.powerOn();
  std::ostringstream log;
  cli::runSchedule(m, o, nullptr, nullptr, &log);
  // long: 0 to 0.2 s = 4410; short looping: 0.2 to 0.6 s = 8820 (more than one pass);
  // short once: 0.6 s to 1 s = 8820 asked for, the file has 5000.
  CHECK(log.str() == "audio-in: " + longFile + " 4410 frames played\n" + "audio-in: " + shortFile +
                         " 8820 frames played\n" + "audio-in: " + shortFile +
                         " 5000 frames played\n");
  std::filesystem::remove(longFile);
  std::filesystem::remove(shortFile);
}

TEST_CASE("cli runSchedule without --audio-in leaves the jack unplugged") {
  const auto o = parseOk({"--seconds", "0.1", "--rate", "22050", "--wav", "x.wav"});
  Machine m(idleRom());
  m.powerOn();
  std::vector<float> audio;
  std::ostringstream log;
  cli::runSchedule(m, o, nullptr, &audio, &log);
  CHECK_FALSE(m.jackConnected());
  CHECK(log.str().empty());
}
