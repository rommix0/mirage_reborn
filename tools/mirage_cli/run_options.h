#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include "mirage/cpu6809.h"
#include "mirage/panel.h"

namespace mirage {
class Machine;
}

namespace mirage::cli {

// One scheduled action of `mirage_cli run`, at an E-cycle count from power-on.
struct RunEvent {
  enum class Kind : uint8_t { TraceOn, KeyDown, KeyUp, Midi };
  uint64_t cycle = 0;
  Kind kind = Kind::Midi;
  PanelKey key{};              // KeyDown / KeyUp
  std::vector<uint8_t> midi;   // Midi
};

struct RunOptions {
  std::string disk;  // empty: no disk in the drive
  std::string rom;
  std::string trace;  // empty: no trace
  std::string wav;    // empty: no audio; else the WAV file runSchedule's audio goes to
  int rate = 48000;   // --rate: the WAV sample rate (1000..768000)
  // --snapshot-in: a file written by --snapshot-out (Machine::saveState). It is
  // loaded after power-on, replacing the boot (and the disk: it holds its own),
  // before any event is scheduled; event times and --seconds are then relative
  // to the loaded state. --snapshot-out: written with Machine::saveState() at
  // the end of the run. Empty: not used.
  std::string snapshotIn, snapshotOut;
  uint64_t endCycle = 0;
  // Sorted by cycle (ties: trace, keys, then MIDI in command-line order), none
  // after endCycle. Holds of the same key that overlap or touch are merged into
  // one press, so a key is released only when no --key still holds it.
  std::vector<RunEvent> events;
};

// Why parsing stopped: exit code 2 = usage error (message empty: print usage).
struct RunArgsError {
  int exitCode;
  std::string message;
};

// Parses the arguments after `run`. Never throws.
std::variant<RunOptions, RunArgsError> parseRunArgs(const std::vector<std::string>& args,
                                                    const std::string& defaultRom);

// Runs `m` (already powered on, or restored from a snapshot) for o.endCycle in
// slices, applying each event when its cycle is reached; cycles count from the
// call, not from the machine's own cycle count. `trace` is installed at the TraceOn event and
// removed at the end. With `audio` non-null the slices go through
// Machine::render at o.rate and the samples are appended to *audio (round(
// endCycle * rate / 2 MHz) of them); an event is then applied at the nearest
// sample boundary, within half a sample (10 us at 48 kHz) of its time.
void runSchedule(Machine& m, const RunOptions& o,
                 const std::function<void(uint16_t pc, const Regs&)>& trace,
                 std::vector<float>* audio = nullptr);

}  // namespace mirage::cli
