#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "mirage/clocks.h"
#include "mirage/disk_image.h"
#include "mirage/machine.h"
#include "mirage/panel.h"

namespace mirage {

// A snapshot of the machine for display, published by the audio thread at the
// end of every process() call. Each field is read from its own atomic, so a
// status() taken while a block finishes may mix fields from two consecutive
// blocks; every field on its own is whole. The one pairing that is kept:
// diskWrites is never older than the block whose diskDirty was read. midiDropped counts every MIDI byte
// lost on the way: rejected by postMidi, overflowing the Machine's input ring,
// overflowing the MIDI-out queue, overflowing the block-MIDI backlog, or block
// events given at an unusable rate (it survives a reset).
struct HostStatus {
  DisplayState display;
  bool driveActive = false, diskPresent = false, diskDirty = false;
  uint64_t diskWrites = 0;  // the inserted disk's DiskImage::writeCount(); 0 with no disk
  uint64_t illegalOps = 0, midiDropped = 0, cycles = 0;
  // MIDI input flow, for MidiInFeeder. midiBytesTaken: cumulative bytes
  // process() has taken from the MIDI queue (queued into the Machine, or
  // discarded while the audio rate is invalid); midiBytesFlushed: bytes
  // postPanic discarded from the queue. A panic flush is in neither
  // midiDropped nor midiBytesTaken. midiQueueDepth: Machine::midiQueueDepth() at
  // the end of the block (bytes queued and not yet delivered to the ACIA).
  // status() never returns a taken or flushed count from a block newer than the
  // depth, so (posted - taken - flushed + depth) can only overstate.
  uint64_t midiBytesTaken = 0, midiBytesFlushed = 0;
  size_t midiQueueDepth = 0;
  // Bytes waiting in the host's backlog for room in the Machine's input ring
  // (block MIDI only; see kMidiBacklogBytes).
  size_t midiBacklog = 0;
  // Audio input level: the largest |sample| of the last block after the input
  // gain (a plain multiply, so it can exceed 1.0). NaN and infinite samples
  // count as 0 (as at the jack), so the peak is always finite. 0 when the last
  // block had no input (or its rate was unusable).
  float inputPeak = 0.0f;
  // outputMuted: Machine::outputMuted() at the end of the block. fastForwarding:
  // the last block was fast-forwarded, entirely or in part. blocks: process()
  // calls that ran the machine (not those skipped for an unusable rate).
  bool outputMuted = false, fastForwarding = false;
  uint64_t blocks = 0;
};

// Opt-in behaviours, both off by default (the standalone never turns them on).
//  muteCalibration: arm the output gate over the ROM's filter-calibration
//    routine, only if the ROM is the known one (isKnownBootRom).
//  fastDisk: while the drive motor is on or the gate is closed, run the machine
//    faster than real time with silent output (see process()).
struct HostOptions {
  bool muteCalibration = false;
  bool fastDisk = false;
};

// Why a disk came back from the audio thread (takeReturnedDisk).
//  Ejected: taken out of the drive (an eject, or the first half of a swap).
//  Unused: an insert that never went in (replaced or ejected during the swap gap).
//  Copy: a postCopyDisk buffer now holding the inserted disk; writeCount is
//    the disk's write count at the moment of the copy, and generation names
//    the machine it came from (each restore swaps in a new one). Both go back
//    to postMarkSaved.
//  NoDiskToCopy: a postCopyDisk buffer handed back untouched (the drive was empty).
enum class DiskReturn : uint8_t { Ejected, Unused, Copy, NoDiskToCopy };
struct ReturnedDisk {
  std::unique_ptr<DiskImage> disk;
  DiskReturn why;
  uint64_t writeCount = 0;
  uint64_t generation = 0;  // restores applied before the disk came back
};
// WAV import (spec 6.7): bytes to copy into wave RAM at `address`, made on the
// UI thread (convertForWaveRam) and handed back to it after the attempt.
struct WaveWrite {
  uint32_t address = 0;
  std::vector<uint8_t> bytes;
};
// Why a wave write came back (takeReturnedWave).
//  Written: copied into wave RAM before the block was rendered.
//  RefusedDriveBusy: nothing written, because the drive was active (the OS may
//    be loading a sound into wave RAM).
enum class WaveReturn : uint8_t { Written, RefusedDriveBusy };
struct ReturnedWave {
  std::unique_ptr<WaveWrite> write;
  WaveReturn why;
};
// A postSnapshot buffer handed back: ok, it holds Machine::saveState()'s bytes
// as of the start of the block (or pump) that took it; not ok, it was too small
// (its contents are unspecified): post one with twice the capacity.
struct ReturnedSnapshot {
  std::unique_ptr<std::vector<uint8_t>> bytes;
  bool ok = false;
};
// One MIDI message at a frame of the block, as a DAW delivers it.
struct BlockMidiEvent {
  uint32_t frame;
  std::span<const uint8_t> bytes;
};
struct MidiOutByte {
  uint8_t byte;
  uint64_t cycle;  // Machine::cycles() when the byte starts on the wire
};

// Runs a Machine for a real-time audio callback. The audio thread owns the
// Machine: it is the only thread that touches it, inside process() (or, while
// no process() runs, a thread in pumpWhileIdle(), which claims it). The UI
// thread and the MIDI-input thread talk to it through lock-free single-producer
// queues that process() drains at the start of each block, and things come
// back the same way (disks to the UI, transmitted MIDI bytes to a MIDI-output
// thread). After prepare(), process() never allocates, frees, locks or does I/O
// for blocks of up to the prepared size: disks travel as unique_ptrs that only
// change hands on the audio thread, and every disk leaving the Machine goes
// back to the UI to be freed there.
//
// The host only presses keys, queues MIDI, inserts or ejects disks and writes
// wave RAM for WAV import (plus power-on, copying the disk out and clearing its
// dirty flag).
//
// Timestamps: postMidi's timeSeconds and process's nowSeconds must come from
// the same clock (the app uses juce::Time::getMillisecondCounterHiRes() *
// 0.001). A block places each MIDI event `(now - t) * hostRate` frames before
// its end, MidiMessageCollector-style: one block of latency, no jitter.
class RealtimeHost {
 public:
  // A disk swap leaves the drive empty this long before the new disk goes in,
  // so the OS sees the disk-present line (VIA PB6) drop.
  static constexpr uint64_t kSwapGapCycles = kCpuClockHz / 2;      // 0.5 s
  // A panel key stays down at least this long: the OS scans one keypad column
  // per main-loop pass, and a quick click could fall between scans.
  static constexpr uint64_t kMinKeyHoldCycles = kCpuClockHz / 10;  // 100 ms
  // MIDI input queue: commands of up to kMidiCommandBytes each. process()
  // takes at most kMidiCommandsPerBlock of them per block.
  static constexpr size_t kMidiQueueCommands = 1024;
  static constexpr size_t kMidiCommandBytes = 12;
  static constexpr size_t kMidiCommandsPerBlock = 256;
  static constexpr size_t kMidiOutQueueBytes = 8192;
  // Block MIDI that does not fit the Machine's 4096-byte input ring waits here
  // (a ring allocated in the constructor) and is moved in as room appears.
  static constexpr size_t kMidiBacklogBytes = 256 * 1024;
  static constexpr size_t kUiQueueCommands = 256;
  static constexpr size_t kReturnQueueDisks = 64;
  // Wave writes in flight each way (UI -> audio, and back).
  static constexpr size_t kWaveQueueWrites = 16;
  // Snapshot requests and restores in flight (state thread -> audio), and each
  // kind's returns (audio -> state thread).
  static constexpr size_t kStateQueueCommands = 8;
  // Fast-forward: the machine runs in chunks of kFastChunkCycles; in real time
  // it may use kFastBudgetFraction of a block's duration, offline at most
  // kFastOfflineCapCycles per block. kFadeInSeconds: the gain ramp 0 -> 1 over
  // the rendered output after a fast-forward.
  static constexpr uint64_t kFastChunkCycles = kCpuClockHz / 1000;
  static constexpr uint64_t kFastOfflineCapCycles = 60 * kCpuClockHz;
  static constexpr double kFastBudgetFraction = 0.5;
  static constexpr double kFadeInSeconds = 0.010;

  // Throws std::invalid_argument unless bootRom is 4096 bytes. The machine is
  // powered on with the drive empty.
  explicit RealtimeHost(std::span<const uint8_t> bootRom, HostOptions options = {});
  ~RealtimeHost();  // frees anything still queued, in either direction
  RealtimeHost(const RealtimeHost&) = delete;
  RealtimeHost& operator=(const RealtimeHost&) = delete;

  std::array<uint8_t, 20> romSha1() const { return romSha1_; }  // any thread
  // True when the calibration gate is armed (options.muteCalibration and a
  // known ROM). Any thread.
  bool calibrationMuteArmed() const { return calibrationMuteArmed_; }
  // Any thread. Offline pacing for the fast-forward (bounded by cycles per
  // block, not wall-clock); only matters with fastDisk.
  void setOffline(bool offline) { offline_.store(offline, std::memory_order_relaxed); }

  // prepare() allocates (Machine::prepare), so it must never run concurrently
  // with process() or render(): call it before the first block and whenever
  // the device's block size or rate changes (JUCE calls audioDeviceAboutToStart
  // before the callbacks start, and again after stopping them).
  // It also reserves the buffer that holds the gain-applied input (maxFrames
  // samples). It claims the machine for its duration, waiting (yielding) while
  // a pumpWhileIdle() on another thread holds it, so the two never overlap.
  void prepare(size_t maxFrames, double hostRate);
  // Renders `frames` mono samples at hostRate:
  //  1. applies the UI queue (keys, insert, eject, reset, panic, copy, mark saved),
  //     then the wave writes, then the state queue (snapshots and restores);
  //  2. takes up to kMidiCommandsPerBlock MIDI commands, each placed at frame
  //     frames - llround((nowSeconds - t) * hostRate), clamped to
  //     [the previous event's frame, frames - 1];
  //  3. renders the block in segments split at those frames, queueing each
  //     event's bytes into the Machine at its frame;
  //  4. at the end of every segment, releases any key whose minimum hold has
  //     passed and makes any swap insert that is due;
  //  5. publishes status().
  // With HostOptions::fastDisk, if the drive is active or the output gate is
  // closed once the MIDI has been placed (step 2), the block's MIDI is queued at once (in order) and the
  // machine runs in kFastChunkCycles chunks (applyDue() after each) until the
  // trigger ends, the budget is used (real time: half the block's duration of
  // wall-clock) or, offline, kFastOfflineCapCycles have run. If the trigger
  // ended the block is then rendered normally, faded in; otherwise it is
  // silence. Emulated state is the same as without fast-forward.
  // An invalid hostRate (0, NaN, out of Machine's range) still applies the UI
  // queue and the wave writes and publishes status, but writes silence and
  // advances nothing. MIDI commands waiting meanwhile are discarded and counted
  // in midiDropped (stale input, as MidiMessageCollector drops it), so old notes
  // do not replay when the rate recovers.
  void process(float* out, size_t frames, double hostRate, double nowSeconds) {
    process(nullptr, out, frames, hostRate, nowSeconds);
  }
  // The same with the device's audio input: `in` holds `frames` mono samples at
  // hostRate (the app averages the active input channels first), or is null
  // when nothing is plugged in (no input device, or the device went away):
  // the jack is then disconnected and goes silent. `in` may be the same buffer
  // as `out`. Each block's input is copied, times the input gain
  // (setInputGain), into a buffer reserved by prepare(), with NaN and infinite
  // samples made 0, and rendered from there (Machine::render(in, out, ...)); the
  // segments split at MIDI offsets take their slice of it. A block longer than
  // the prepared size is rendered in chunks of that size, with or without input,
  // so nothing allocates (the sound is the same apart from the engine's
  // start-up transient, which depends on the block length). If prepare() was
  // never called the buffer grows on the audio thread, as Machine's does. A
  // rate change between blocks re-plugs the jack (Machine::render). An unusable
  // hostRate reads no input and leaves the jack as it was.
  void process(const float* in, float* out, size_t frames, double hostRate, double nowSeconds);
  // The same with MIDI events at frame offsets in the block (a plugin host's),
  // in frame order. Each frame is clamped to [the previous frame, frames - 1]
  // (all events land at the current cycle in a zero-frame block); the block is
  // rendered in segments split at the frames, and each event's bytes are fed at
  // its frame. The timestamped MIDI queue (postMidi) is not touched. A message
  // goes straight into the Machine's ring only if the backlog is empty and the
  // ring has room for all of it; otherwise it is appended to the backlog
  // (dropped whole, and counted in midiDropped, if that is full too). The
  // backlog is moved into the ring as room allows at every segment end and
  // between fast-forward chunks; postPanic and postReset clear it. An unusable
  // hostRate drops the events (counted in midiDropped). Otherwise as above.
  void process(const float* in, float* out, size_t frames, double hostRate,
               std::span<const BlockMidiEvent> events);
  // Any thread. Linear gain applied to the input before the jack, clamped to
  // 0..4 (default 1); NaN is ignored. Takes effect from the next segment
  // the audio thread renders.
  void setInputGain(float gain);

  // One producer at a time (MidiInFeeder serialises its callers; the app goes
  // through it, and it paces long messages). Messages
  // longer than kMidiCommandBytes (SysEx) travel as consecutive commands with
  // the same timestamp. A message over 12 KB (kMidiQueueCommands x
  // kMidiCommandBytes) can never fit and is always rejected. All or nothing: if the queue lacks room for the whole
  // message, nothing is queued, the bytes are added to midiDropped and it
  // returns false. An empty message is accepted and does nothing.
  bool postMidi(std::span<const uint8_t> bytes, double timeSeconds);

  // UI thread only (the single producer of the UI queue). Each returns false
  // when the queue is full (or, for the disk calls, the disk is null), and then
  // leaves its argument untouched.
  // Key release earlier than kMinKeyHoldCycles after the press is deferred.
  bool postKey(PanelKey key, bool down);
  // Into an empty drive with no swap pending: goes in at once. Otherwise the
  // current disk is ejected at once (returned Ejected) and this one goes in
  // kSwapGapCycles after that eject; an insert already waiting comes back Unused.
  bool postInsert(std::unique_ptr<DiskImage>&& disk);
  // Ejects the disk (returned Ejected) or, during a swap gap, cancels the
  // pending insert (returned Unused).
  bool postEject();
  bool postReset();  // Machine::powerOn(); the disk stays in, held keys stay held
  // Discards all MIDI input not yet played: the commands waiting in the host's
  // MIDI queue (not counted in midiDropped: a deliberate flush) and the
  // Machine's input queue. Then queues All Notes Off (CC 123) on all 16
  // channels: B0+ch 7B 00, 48 bytes, at the start of the block.
  bool postPanic();
  // `dest` must be a writable image buffer (DiskImage::blank()), not a
  // moved-from DiskImage, so the copy needs no allocation. It comes back as
  // Copy holding the inserted disk, or as NoDiskToCopy.
  bool postCopyDisk(std::unique_ptr<DiskImage>&& dest);
  // Clears the inserted disk's dirty flag if its write count still equals
  // writeCount (no write since the copy that was saved) and no restore has
  // swapped in another machine since the copy (`generation`, the Copy's): a
  // restored disk counts its writes from 0 again, so its count proves nothing.
  bool postMarkSaved(uint64_t writeCount, uint64_t generation = 0);
  // UI thread: the next disk handed back, oldest first. Freeing it is the
  // caller's job (never the audio thread's).
  [[nodiscard]] std::optional<ReturnedDisk> takeReturnedDisk();

  // UI thread only (the single producer of the wave queue). Returns false, and
  // leaves `w` untouched, when the queue is full, `w` is null or the write runs
  // past the end of wave RAM (Machine::kSampleRamSize). process() copies it into
  // wave RAM before rendering, unless the drive is active (driveActive()), and
  // hands it back either way. While the return queue is full the writes wait in
  // the wave queue, so the audio thread never frees one.
  bool postWaveWrite(std::unique_ptr<WaveWrite>&& w);
  // UI thread: the next wave write handed back, oldest first; freed by the caller.
  [[nodiscard]] std::optional<ReturnedWave> takeReturnedWave();

  // ---- State (a DAW project): snapshots and restores ----------------------
  // The state queue has a single producer, the caller's state thread (the
  // plugin serialises its getState/setState). Requests are served in order at
  // the start of a block, after the UI queue and the wave writes (so a key or
  // insert posted just before is in a snapshot), or by pumpWhileIdle().
  //
  // `reserved` is an empty buffer with capacity reserved off the audio thread.
  // It comes back through takeReturnedSnapshot() holding the snapshot (ok) or,
  // if it was too small, not ok: retry with 2 x capacity. False, leaving it
  // untouched, when it is null or the queue is full.
  // The UI queue and wave writes are applied again just before it is taken.
  // In a block, a request waits (with everything behind it) while a swap
  // insert is waiting out its gap or a key release is deferred for the minimum
  // hold, so a snapshot never holds a half-done swap or a stuck key; both
  // settle within kSwapGapCycles of emulated time. In pumpWhileIdle(), where
  // no time passes, deferred key releases are made at once and a waiting swap
  // insert is left out: that snapshot has the drive empty.
  bool postSnapshot(std::unique_ptr<std::vector<uint8_t>>&& reserved);
  [[nodiscard]] std::optional<ReturnedSnapshot> takeReturnedSnapshot();
  // State thread. A new Machine from this host's boot ROM, restored from
  // `snapshot` (Machine::loadState: throws StateError as it does, e.g. "snapshot
  // was taken with a different boot ROM"), with the calibration gate armed if
  // it is armed here (before the load, so the gate starts closed mid-routine),
  // and prepared at the last prepare() size and rate, if any. Allocates.
  std::unique_ptr<Machine> buildMachine(std::span<const uint8_t> snapshot) const;
  // Posts a Machine (from buildMachine) to replace the running one. At the
  // start of a block (or pump) the audio thread wires its onMidiOut, swaps it
  // in, clears the host's per-machine state (deferred key releases, the block
  // MIDI backlog, the MIDI queue, counted in midiBytesFlushed, and the key and
  // swap timers), hands a pending swap insert back as Unused and returns the old
  // Machine. Render chunks never exceed the new machine's preparedFrames(), so
  // a larger block size prepared between buildMachine and the swap cannot make
  // it allocate.
  // False, leaving `m` untouched, when it is null or the queue is full.
  bool postRestore(std::unique_ptr<Machine>&& m);
  // State thread: the machine a restore replaced (null when none yet). Getting
  // it confirms the swap; freeing it is the caller's job.
  [[nodiscard]] std::unique_ptr<Machine> takeReturnedMachine();
  // Any thread but the audio thread, for when the DAW calls no process(). If
  // no process() call holds the machine, claims it, does a block's start-of-block
  // work (stalled disk returns, the UI queue, the wave writes, the state queue),
  // publishes status, releases it and returns true. Otherwise returns false at
  // once. A process() that finds the machine claimed writes zeros, advances and
  // publishes nothing, counts its block MIDI events in midiDropped, and returns.
  // Never blocks either side.
  bool pumpWhileIdle();

  // MIDI-output thread: the next byte the OS transmitted, in cycle order. If
  // the queue (kMidiOutQueueBytes) is full the audio thread drops the byte and
  // counts it in midiDropped.
  bool popMidiOut(MidiOutByte& out);

  HostStatus status() const;  // any thread

  // Tests only: the Machine, for use on the thread that calls process() while
  // no process() or pumpWhileIdle() call is running.
  Machine& machineForTest() { return *machine_; }

 private:
  struct Queues;  // the SPSC queues (large, so on the heap)

  void applyUi();
  void applyDue();  // key releases and the swap insert, at segment ends
  // Hands a disk leaving the Machine to the UI. Never frees on this thread: a
  // full return queue parks it in stalled_ until a later block.
  void returnDisk(std::unique_ptr<DiskImage> disk, DiskReturn why, uint64_t writeCount = 0);
  void flushStalled();
  void applyWaves();  // the wave queue, after the UI queue
  void applyState(bool idle);  // the state queue, after the wave queue; idle: in a pump
  void swapIn(std::unique_ptr<Machine> m);  // a restore; returns the old machine
  // True if no one else holds the machine; then it is ours until release().
  bool claim() { return !claimed_.exchange(true, std::memory_order_acquire); }
  void release() { claimed_.store(false, std::memory_order_release); }
  struct Release {  // release() however the claim's scope ends
    RealtimeHost* h;
    ~Release() { h->release(); }
  };
  void countRingOverflows();  // adds new Machine::midiOverflows() to midiDropped
  size_t discardMidiInput();  // empties the MIDI queue; returns the bytes discarded
  uint64_t midiBytesTaken_ = 0, midiBytesFlushed_ = 0;  // audio thread; published by publishStatus
  void publishStatus();
  // Renders in[0..n) -> out[0..n) (in null: nothing plugged in) through the gain
  // buffer in chunks, and raises blockPeak_.
  void renderSegment(const float* in, float* out, size_t n, double hostRate);
  // Shared by both process() overloads: `timestamped` takes the events from the
  // MIDI queue (placed by nowSeconds), otherwise from `events`.
  void runBlock(const float* in, float* out, size_t frames, double hostRate, double nowSeconds,
                std::span<const BlockMidiEvent> events, bool timestamped);
  // Block MIDI: into the ring if the backlog is empty and the whole message
  // fits, else into the backlog (or dropped).
  void sendBlockMidi(std::span<const uint8_t> bytes);
  void topUpBacklog();  // backlog -> the Machine's ring, as far as there is room
  bool fastTrigger() const;
  // Runs chunks while the trigger holds and the budget lasts; true if the
  // trigger ended.
  bool fastForward(size_t frames, double hostRate);

  std::array<uint8_t, 20> romSha1_{};
  std::array<uint8_t, Machine::kBootRomSize> rom_{};  // for buildMachine
  // q_ is declared before machine_ so machine_ is destroyed first: the
  // Machine's onMidiOut callback dereferences q_.
  std::unique_ptr<Queues> q_;
  std::unique_ptr<Machine> machine_;  // about 200 KB; touched by the audio thread only

  // Audio-thread state.
  std::array<uint64_t, kPanelKeyCount> pressCycle_{};  // cycles() at each key's last press
  std::array<bool, kPanelKeyCount> releaseDue_{};      // a release waits for the hold
  std::unique_ptr<DiskImage> pendingInsert_;           // swap: goes in after the gap
  uint64_t ejectCycle_ = 0;                            // swap: when the drive emptied
  static constexpr size_t kStalledSlots = 4;
  std::array<ReturnedDisk, kStalledSlots> stalled_{};  // returns waiting for queue room
  size_t stalledCount_ = 0;
  uint64_t ringOverflowsSeen_ = 0;  // Machine::midiOverflows() already counted
  uint64_t generation_ = 0;         // restores swapped in (ReturnedDisk::generation)
  // prepare()'s block size (0 = never prepared) and rate. Atomic only because
  // buildMachine reads them on the state thread; the pair may be torn by a
  // concurrent prepare(), which the swap's chunk limit makes harmless.
  std::atomic<size_t> maxFrames_{0};
  std::atomic<double> preparedRate_{0.0};
  std::vector<float> inputBuf_;     // the gain-applied input; sized by prepare()
  float blockPeak_ = 0.0f;          // this block's input peak so far
  bool fastDisk_ = false, calibrationMuteArmed_ = false;
  std::atomic<bool> offline_{false};
  bool fading_ = false;             // a fade-in is pending on the rendered output
  size_t fadePos_ = 0;              // frames faded so far
  bool fastForwarding_ = false;     // the current block was fast-forwarded
  uint64_t blocks_ = 0;
  std::vector<uint8_t> backlog_;  // ring of kMidiBacklogBytes
  size_t backlogHead_ = 0, backlogCount_ = 0;

  // Published status (relaxed: each is a separate value; see HostStatus).
  std::atomic<uint16_t> display_{0};  // left | right << 8
  std::atomic<bool> driveActive_{false}, diskPresent_{false}, diskDirty_{false};
  std::atomic<uint64_t> diskWrites_{0}, illegalOps_{0}, cycles_{0};
  std::atomic<uint64_t> midiDropped_{0};  // added to by the MIDI-input and audio threads
  std::atomic<uint64_t> midiTakenPub_{0}, midiFlushedPub_{0};
  std::atomic<size_t> midiDepthPub_{0}, midiBacklogPub_{0};
  std::atomic<float> inputGain_{1.0f}, inputPeakPub_{0.0f};
  std::atomic<bool> outputMutedPub_{false}, fastForwardingPub_{false};
  std::atomic<uint64_t> blocksPub_{0};

  // The machine claim: held by process() for a whole block, or by
  // pumpWhileIdle(). Taken with acquire and given back with release, so
  // everything one holder did to the machine and the audio-side state (and as
  // the consumer of the UI, wave, state and MIDI queues, and the producer of
  // the return queues) happens-before whatever the next holder does: the two
  // threads act as one audio thread, one at a time.
  std::atomic<bool> claimed_{false};
};

}  // namespace mirage
