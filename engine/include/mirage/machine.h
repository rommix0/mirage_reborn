#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "mirage/acia6850.h"
#include "mirage/bus.h"
#include "mirage/cpu6809.h"
#include "mirage/disk_image.h"
#include "mirage/doc5503.h"
#include "mirage/panel.h"
#include "mirage/stream_resampler.h"
#include "mirage/via6522.h"
#include "mirage/voice_filter.h"
#include "mirage/wd1772.h"

namespace mirage {

// The whole Mirage: 6809 + RAM + banked sample RAM + boot ROM + I/O decode.
// The VIA, ACIA (MIDI), WD1772 floppy controller, DOC 5503, the voice filters
// with their CV path ($E400-$E41F, write only) and the front panel are wired
// in. The analog chain (docs/hardware/filters.md): each DOC channel goes
// through its own filter, the filter outputs are summed, and the DOC A/D reads
// the input the VIA PB2-3 mux selects.
class Machine : private Bus {
 public:
  static constexpr uint32_t kBootRomSize = 0x1000;
  static constexpr uint32_t kSampleRamSize = 0x20000;
  // Version of the saveState() layout (after the "MIRG" magic). Bumped whenever
  // the layout changes; loadState() rejects any other value.
  static constexpr uint32_t kSnapshotVersion = 2;

  // Throws std::invalid_argument unless bootRom is exactly 4096 bytes.
  explicit Machine(std::span<const uint8_t> bootRom);
  Machine(const Machine&) = delete;
  Machine& operator=(const Machine&) = delete;

  // Clears RAM and sample RAM to 0, resets the chips and the CPU. Never
  // touches an inserted disk.
  void powerOn();
  // Steps the CPU until the requested E-cycles have elapsed. The last
  // instruction may overshoot; the excess is credited against the next call,
  // so cycles() stays exact and long-run totals track the requests.
  void runCycles(uint64_t eCycles);
  uint64_t cycles() const { return cycles_; }
  uint8_t peek(uint16_t addr);  // CPU view, no I/O side effects (I/O reads $FF)
  uint8_t sampleRam(uint32_t addr) const { return sample_[addr & (kSampleRamSize - 1)]; }
  Cpu6809& cpu() { return cpu_; }
  // Debug trace: when set, called before each instruction (and before each
  // interrupt entry, with the interrupted PC) with the PC about to execute and
  // the registers. Steps spent waiting in SYNC/CWAI are not reported. Pass an
  // empty function to stop. Unset costs one branch per step.
  void setTrace(std::function<void(uint16_t pc, const Regs&)> fn) { trace_ = std::move(fn); }

  // Front panel. Held keys survive powerOn() like a finger on a button.
  void setKey(PanelKey key, bool down) { panel_.setKey(key, down); }
  bool keyDown(PanelKey key) const { return panel_.keyDown(key); }
  DisplayState display() const { return panel_.display(cycles_); }

  // MIDI. Queued bytes reach the ACIA one per MIDI byte time (320 us) of
  // emulated time, whatever the ACIA's divide setting. The input is a fixed
  // ring of kMidiQueueCapacity bytes, so queueMidi never allocates and the
  // audio thread may call it. Bytes that do not fit are dropped (the newest
  // first) and counted in midiOverflows(); an OS that reads too slowly gets
  // OVRN as on hardware. midiQueueDepth() is the bytes queued and not yet
  // delivered to the ACIA. flushMidi() discards them (a byte already in the
  // ACIA stays). powerOn() discards queued bytes and zeroes midiOverflows(),
  // which is host diagnostics and not saved; loadState() leaves it alone.
  // Bytes the OS transmits are passed to onMidiOut, paced as on the wire: one
  // byte per 640 E cycles (Acia6850::kByteCycles). `cycle` is cycles() at the
  // moment the byte starts on the wire (for a byte that waited in the ACIA's
  // holding register, when the previous byte finished), so a host can place
  // the output in time. It runs on the thread that runs the machine.
  static constexpr size_t kMidiQueueCapacity = 4096;
  void queueMidi(std::span<const uint8_t> bytes);
  size_t midiQueueDepth() const { return midiCount_; }
  uint64_t midiOverflows() const { return midiOverflows_; }
  uint64_t illegalOps() const { return cpu_.illegalCount(); }  // undefined opcodes executed (host diagnostics)
  void flushMidi();
  std::function<void(uint8_t byte, uint64_t cycle)> onMidiOut;

  // Floppy drive. insertDisk replaces any disk already in the drive (aborting a
  // command in flight, as an eject does) and returns the disk it replaced, or
  // null; a null argument acts as an eject. ejectDisk hands the disk back (null
  // if the drive was empty). The unique_ptr overloads swap pointers only: they
  // never allocate or free, so the audio thread may use them. The DiskImage
  // overload heap-allocates. VIA PB6 reads 1 while a disk is in. powerOn()
  // never touches the disk. driveActive() is true while the motor is on (VIA
  // PB4 low).
  std::unique_ptr<DiskImage> insertDisk(std::unique_ptr<DiskImage> disk);
  std::unique_ptr<DiskImage> insertDisk(DiskImage disk);
  std::unique_ptr<DiskImage> ejectDisk();
  DiskImage* disk() { return disk_.get(); }
  const DiskImage* disk() const { return disk_.get(); }
  bool driveActive() const { return wd_.motor(); }
  // Read-only views of the chips, for diagnostics (sectorsRead(), diskChanged(),
  // sampleRate()). They are const so a host cannot change chip state behind
  // the Machine's back (its IRQ/NMI wiring would go stale); see debugWrite().
  const Wd1772& fdc() const { return wd_; }
  // The DOC sees all 128 KB of sample RAM and is clocked by the 8 MHz master
  // clock. Its onFrame and adcRead callbacks belong to the Machine (they drive
  // the filters and the A/D mux); do not replace them.
  const Doc5503& doc() const { return doc_; }
  // The CPU's view of the address space, with the same I/O side effects as a
  // real CPU access, for tests and diagnostics. A read of $ECE0 pops the DOC
  // interrupt FIFO, a read of the FDC data register clears DRQ, a write to a
  // chip register acts on the chip, and the IRQ/NMI/FIRQ lines are updated
  // afterwards. peek() is the side-effect-free read. DOC register r is at
  // $EC00 + r. Not for the audio thread while the machine is running on
  // another thread.
  uint8_t debugRead(uint16_t addr) { return read(addr); }
  void debugWrite(uint16_t addr, uint8_t value) { write(addr, value); }
  const FilterBank& filters() const { return filters_; }
  // Host audio tap (not saved): the filtered voice mix, once per DOC scan at
  // doc().sampleRate(). One full-volume oscillator through an open filter
  // peaks at about 1.0. Raw: no DC blocking, gain or resampling.
  std::function<void(float)> onMix;

  // Audio output. Runs the emulation for exactly frames / hostRate seconds
  // (the fractional E cycles of each call are carried to the next, so the
  // total over many calls tracks the requested time) and writes `frames` mono
  // samples at hostRate. The mix is DC-blocked (one-pole high-pass, about
  // 5 Hz), scaled by kOutputGain, resampled from doc().sampleRate(), which
  // can change while running, by a StreamResampler, and passed through
  // softClip(). Headroom: a single full-volume oscillator through an open
  // filter peaks at kOutputGain (0.5) and one factory-sound voice at about
  // 0.26, so normal play stays inside softClip()'s linear region (to 0.75)
  // and is untouched; a loud chord rounds off towards +-1.0 and never passes
  // it. The CLI's WAVs and the app therefore match.
  // The resampler's group delay (32 DOC scans, about 1 ms) means the first
  // samples after power-on are silence. Mix samples made by runCycles() outside
  // render() are not part of the audio stream. hostRate may differ between
  // calls. hostRate must be finite and within kMinHostRate..kMaxHostRate
  // (8 kHz-384 kHz); otherwise (NaN, infinity, 0, negative, out of range, as a
  // host may report before prepareToPlay) render() writes `frames` zeros,
  // advances nothing and returns.
  // Real time: after prepare(maxFrames, hostRate), render() with
  // frames <= maxFrames never allocates, frees or locks (a larger request still
  // works but may allocate). prepare() itself allocates, so it must never run
  // concurrently with render() (call it before the first block, as a host does
  // when a device starts); it only reserves capacity (and sets the output
  // rate) and changes no emulated state. powerOn() and loadState() keep the
  // reserved capacity.
  void prepare(size_t maxFrames, double hostRate);
  void render(float* out, size_t frames, double hostRate);
  // Output scale of render(), before the soft clip.
  static constexpr float kOutputGain = 0.5f;
  // Linear to +-0.75, then 0.75 + 0.25 tanh((|x| - 0.75) / 0.25): odd,
  // monotonic, continuous in value and slope, and never beyond +-1.0 for any finite
  // input (float rounding makes it exactly 1.0 past |x| of about 3). render()
  // applies it to every sample it hands out.
  static float softClip(float x);
  static constexpr double kMinHostRate = 8000.0, kMaxHostRate = 384000.0;

  // Whole-machine snapshot, for restoring a session exactly (a DAW project).
  // Layout: fourcc("MIRG"), u32 kSnapshotVersion, the 20-byte SHA-1 of the boot
  // ROM (romSha1()), then one tagged section per
  // part: the machine core (bank, cycle count and credit, main/expansion/sample
  // RAM), CPU (registers, wait state, interrupt lines), VIA, DOC (oscillators,
  // FIFO, A/D, master clock remainder), filter bank, audio path (resampler,
  // DC blocker, fractional cycles, pending samples), WD1772 (registers and any
  // command in flight), the inserted disk with its dirty flag (or "no disk"),
  // ACIA (with the transmit holding register and shifter), the MIDI input queue with its next-delivery cycle, and the panel
  // (held keys and display history). Restoring a snapshot into any Machine built
  // with the same boot ROM, then running the same inputs, gives bit-identical
  // state and audio.
  // Deliberately not saved, because it is not emulated state: the boot ROM
  // (fixed by the constructor), the host callbacks onMidiOut, onMix and the
  // trace (and the Machine's own chip wiring), and rendering_, which is only
  // true inside render().
  // A snapshot of a different version, one taken with a different boot ROM
  // (StateError "snapshot was taken with a different boot ROM"), a wrong magic, a truncated or corrupt
  // blob or trailing bytes throw StateError and leave the machine untouched, as do
  // values no running machine can have: a cycle credit over 64, or (with MIDI
  // queued) a next-delivery cycle outside (cycles - 64, cycles + 640].
  std::vector<uint8_t> saveState() const;
  void loadState(std::span<const uint8_t> data);
  // SHA-1 of the boot ROM, computed once by the constructor.
  std::array<uint8_t, 20> romSha1() const { return romSha1_; }

 private:
  uint8_t read(uint16_t addr) override;
  void write(uint16_t addr, uint8_t value) override;
  uint8_t ioRead(uint16_t addr);
  void ioWrite(uint16_t addr, uint8_t value);
  void tickDevices(uint32_t cycles);
  // The CPU IRQ line is the OR of the VIA, WD1772 DRQ and DOC IRQ outputs.
  // Call after anything that can change one of them.
  void updateIrq();
  // The CPU NMI line is the WD1772 INTRQ output (the CPU latches rising edges).
  void updateNmi();
  // The CPU FIRQ line is the ACIA IRQ output.
  void updateFirq();
  // render(): one raw mix sample from a DOC scan into the audio stream.
  void pushMix(float mix);
  void saveAudio(StateWriter&) const;
  void loadAudio(StateReader&);  // commits only on success

  std::array<uint8_t, kBootRomSize> rom_{};
  std::array<uint8_t, 20> romSha1_{};
  std::array<uint8_t, 0x4000> ram_{};   // $8000-$BFFF
  std::array<uint8_t, 0x2000> exp_{};   // $C000-$DFFF
  std::array<uint8_t, kSampleRamSize> sample_{};
  uint8_t bank_ = 0;  // VIA PB1:PB0
  uint64_t cycles_ = 0;
  uint32_t credit_ = 0;  // cycles executed beyond what was requested
  Cpu6809 cpu_;
  Via6522 via_;
  Panel panel_;
  Acia6850 acia_;
  Wd1772 wd_;
  Doc5503 doc_;
  FilterBank filters_;
  uint32_t masterRem_ = 0;  // master clocks owed to the DOC, x kCpuClockHz
  std::unique_ptr<DiskImage> disk_;  // wd_ points at it; the Machine is not movable
  std::array<uint8_t, kMidiQueueCapacity> midiRing_{};  // MIDI input ring (saved, oldest first)
  size_t midiHead_ = 0;         // index of the oldest queued byte
  size_t midiCount_ = 0;        // bytes queued
  uint64_t midiOverflows_ = 0;  // bytes queueMidi dropped (host diagnostics, not saved)
  uint64_t nextMidiCycle_ = 0;  // cycles() at which the next queued byte arrives
  std::function<void(uint16_t, const Regs&)> trace_;  // host-side, not saved

  // Audio path (saved).
  StreamResampler resampler_{48000.0};
  float dcIn_ = 0.0f, dcOut_ = 0.0f;  // DC blocker: previous input and output
  double renderCycleFrac_ = 0.0;      // E cycles owed from render() calls, < 1
  std::vector<float> pending_;        // resampled samples not yet handed out
  float lastOut_ = 0.0f;              // last sample handed out (held on an underrun)
  bool rendering_ = false;            // inside render(): pushMix feeds pending_
};

}  // namespace mirage
