#pragma once
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <variant>
#include <vector>

#include "mirage/cpu6809.h"
#include "mirage/panel.h"
#include "mirage/wav_import.h"

namespace mirage {
class Machine;
}

namespace mirage::cli {

// One scheduled action of `mirage_cli run`, at an E-cycle count from power-on.
struct RunEvent {
  enum class Kind : uint8_t { TraceOn, KeyDown, KeyUp, Midi, Import, AudioIn };
  uint64_t cycle = 0;
  Kind kind = Kind::Midi;
  PanelKey key{};              // KeyDown / KeyUp
  std::vector<uint8_t> midi;   // Midi
  // Import (--import): the WAV file and its settings come from the command
  // line; prepareImports fills plan and wave before the run.
  std::string file;
  ImportSettings import;
  ImportPlan plan;
  std::vector<uint8_t> wave;  // convertForWaveRam output, written at plan.address
  // AudioIn (--audio-in): `file` plays into the input jack from `cycle` at its
  // own rate; prepareAudioIn fills samples (mono, at RunOptions::rate, gain applied).
  double gain = 1.0;
  bool loop = false;
  std::vector<float> samples;
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
  // --save-disk: the inserted disk is written here (DiskImage::saveFile) at the
  // end of the run. Empty: not used.
  std::string saveDisk;
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

// Reads, plans and converts the WAV file of every Import event (the files are
// not touched while parsing). Returns "" on success, else "FILE: message" for
// the first file that cannot be read or whose plan has a problem (the run must
// not start).
std::string prepareImports(RunOptions& o);

// Reads every AudioIn event's WAV file and resamples it (linear interpolation)
// to o.rate, applying its gain. Returns "" on success, else "FILE: message" for
// the first file that cannot be read (the run must not start). Call after the
// options are final (o.rate is parsed).
std::string prepareAudioIn(RunOptions& o);

// Runs `m` (already powered on, or restored from a snapshot) for o.endCycle in
// slices, applying each event when its cycle is reached; cycles count from the
// call, not from the machine's own cycle count. `trace` is installed at the TraceOn event and
// removed at the end. With `audio` non-null the slices go through
// Machine::render at o.rate and the samples are appended to *audio (round(
// endCycle * rate / 2 MHz) of them); an event is then applied at the nearest
// sample boundary, within half a sample (10 us at 48 kHz) of its time. An
// Import event writes its wave (prepareImports) with Machine::writeWaveRam and
// logs "import: FILE N bytes at $XXXXX, last page P[, truncated]" to `log`; if
// the drive is busy then (Machine::driveActive) nothing is written and it logs
// "import: FILE refused, the drive is busy".
// An AudioIn event plugs the jack in and plays its samples from its time (a
// later one replaces the earlier from its own time; once a non-looping file has
// ended the jack carries silence). With any AudioIn event the slices are always
// rendered (into *audio, or into a discarded buffer when audio is null) with
// Machine::render(in, out, ...), so the input lands sample-exactly: it starts
// at the same frame boundary an event at that time would be applied. At the end
// `log` gets "audio-in: FILE N frames played" per AudioIn event, N the frames
// of that entry fed to the jack (at o.rate; a loop counts every pass).
void runSchedule(Machine& m, const RunOptions& o,
                 const std::function<void(uint16_t pc, const Regs&)>& trace,
                 std::vector<float>* audio = nullptr, std::ostream* log = nullptr);

}  // namespace mirage::cli
