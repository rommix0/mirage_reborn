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
// or overflowing the MIDI-out queue (it survives a reset).
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
};

// Why a disk came back from the audio thread (takeReturnedDisk).
//  Ejected: taken out of the drive (an eject, or the first half of a swap).
//  Unused: an insert that never went in (replaced or ejected during the swap gap).
//  Copy: a postCopyDisk buffer now holding the inserted disk; writeCount is
//    the disk's write count at the moment of the copy.
//  NoDiskToCopy: a postCopyDisk buffer handed back untouched (the drive was empty).
enum class DiskReturn : uint8_t { Ejected, Unused, Copy, NoDiskToCopy };
struct ReturnedDisk {
  std::unique_ptr<DiskImage> disk;
  DiskReturn why;
  uint64_t writeCount = 0;
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
struct MidiOutByte {
  uint8_t byte;
  uint64_t cycle;  // Machine::cycles() when the byte starts on the wire
};

// Runs a Machine for a real-time audio callback. The audio thread owns the
// Machine: it is the only thread that touches it, inside process(). The UI
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
  static constexpr size_t kUiQueueCommands = 256;
  static constexpr size_t kReturnQueueDisks = 64;
  // Wave writes in flight each way (UI -> audio, and back).
  static constexpr size_t kWaveQueueWrites = 16;

  // Throws std::invalid_argument unless bootRom is 4096 bytes. The machine is
  // powered on with the drive empty.
  explicit RealtimeHost(std::span<const uint8_t> bootRom);
  ~RealtimeHost();  // frees anything still queued, in either direction
  RealtimeHost(const RealtimeHost&) = delete;
  RealtimeHost& operator=(const RealtimeHost&) = delete;

  std::array<uint8_t, 20> romSha1() const { return romSha1_; }  // any thread

  // prepare() allocates (Machine::prepare), so it must never run concurrently
  // with process() or render(): call it before the first block and whenever
  // the device's block size or rate changes (JUCE calls audioDeviceAboutToStart
  // before the callbacks start, and again after stopping them).
  void prepare(size_t maxFrames, double hostRate);
  // Renders `frames` mono samples at hostRate:
  //  1. applies the UI queue (keys, insert, eject, reset, panic, copy, mark saved),
  //     then the wave writes;
  //  2. takes up to kMidiCommandsPerBlock MIDI commands, each placed at frame
  //     frames - llround((nowSeconds - t) * hostRate), clamped to
  //     [the previous event's frame, frames - 1];
  //  3. renders the block in segments split at those frames, queueing each
  //     event's bytes into the Machine at its frame;
  //  4. at the end of every segment, releases any key whose minimum hold has
  //     passed and makes any swap insert that is due;
  //  5. publishes status().
  // An invalid hostRate (0, NaN, out of Machine's range) still applies the UI
  // queue and the wave writes and publishes status, but writes silence and
  // advances nothing. MIDI commands waiting meanwhile are discarded and counted
  // in midiDropped (stale input, as MidiMessageCollector drops it), so old notes
  // do not replay when the rate recovers.
  void process(float* out, size_t frames, double hostRate, double nowSeconds);

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
  // writeCount (no write since the copy that was saved).
  bool postMarkSaved(uint64_t writeCount);
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

  // MIDI-output thread: the next byte the OS transmitted, in cycle order. If
  // the queue (kMidiOutQueueBytes) is full the audio thread drops the byte and
  // counts it in midiDropped.
  bool popMidiOut(MidiOutByte& out);

  HostStatus status() const;  // any thread

  // Tests only: the Machine, for use on the thread that calls process() while
  // no process() call is running.
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
  void countRingOverflows();  // adds new Machine::midiOverflows() to midiDropped
  size_t discardMidiInput();  // empties the MIDI queue; returns the bytes discarded
  uint64_t midiBytesTaken_ = 0, midiBytesFlushed_ = 0;  // audio thread; published by publishStatus
  void publishStatus();

  std::array<uint8_t, 20> romSha1_{};
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

  // Published status (relaxed: each is a separate value; see HostStatus).
  std::atomic<uint16_t> display_{0};  // left | right << 8
  std::atomic<bool> driveActive_{false}, diskPresent_{false}, diskDirty_{false};
  std::atomic<uint64_t> diskWrites_{0}, illegalOps_{0}, cycles_{0};
  std::atomic<uint64_t> midiDropped_{0};  // added to by the MIDI-input and audio threads
  std::atomic<uint64_t> midiTakenPub_{0}, midiFlushedPub_{0};
  std::atomic<size_t> midiDepthPub_{0};
};

}  // namespace mirage
