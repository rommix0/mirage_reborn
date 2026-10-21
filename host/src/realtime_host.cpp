#include "mirage/realtime_host.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "mirage/spsc_queue.h"

namespace mirage {

namespace {

constexpr auto kRelaxed = std::memory_order_relaxed;

// All Notes Off (CC 123, value 0) on channels 1-16.
constexpr std::array<uint8_t, 48> kPanicBytes = [] {
  std::array<uint8_t, 48> a{};
  for (uint8_t ch = 0; ch < 16; ++ch) {
    a[3 * ch] = uint8_t(0xB0 + ch);
    a[3 * ch + 1] = 0x7B;
    a[3 * ch + 2] = 0x00;
  }
  return a;
}();

}  // namespace

struct RealtimeHost::Queues {
  struct UiCommand {
    enum class Type : uint8_t { Key, Insert, Eject, Reset, Panic, Copy, MarkSaved };
    Type type = Type::Key;
    PanelKey key = PanelKey::LoadUpper;
    bool down = false;
    uint64_t writeCount = 0;
    std::unique_ptr<DiskImage> disk;  // Insert, Copy
  };
  struct MidiCommand {
    double time = 0;
    uint8_t size = 0;
    std::array<uint8_t, kMidiCommandBytes> bytes{};
  };

  SpscQueue<UiCommand, kUiQueueCommands> ui;           // UI -> audio
  SpscQueue<MidiCommand, kMidiQueueCommands> midi;     // MIDI input -> audio
  SpscQueue<MidiOutByte, kMidiOutQueueBytes> midiOut;  // audio -> MIDI output
  SpscQueue<ReturnedDisk, kReturnQueueDisks> returns;  // audio -> UI
  SpscQueue<std::unique_ptr<WaveWrite>, kWaveQueueWrites> waves;  // UI -> audio
  SpscQueue<ReturnedWave, kWaveQueueWrites> waveReturns;          // audio -> UI
  // process() scratch: this block's MIDI events and their frame offsets.
  std::array<MidiCommand, kMidiCommandsPerBlock> events{};
  std::array<size_t, kMidiCommandsPerBlock> offsets{};
};

RealtimeHost::RealtimeHost(std::span<const uint8_t> bootRom)
    : q_(std::make_unique<Queues>()), machine_(std::make_unique<Machine>(bootRom)) {
  romSha1_ = machine_->romSha1();
  machine_->onMidiOut = [this](uint8_t byte, uint64_t cycle) {
    if (!q_->midiOut.push(MidiOutByte{byte, cycle})) midiDropped_.fetch_add(1, kRelaxed);
  };
  machine_->powerOn();
  publishStatus();
}

RealtimeHost::~RealtimeHost() = default;

void RealtimeHost::prepare(size_t maxFrames, double hostRate) {
  machine_->prepare(maxFrames, hostRate);
}

void RealtimeHost::process(float* out, size_t frames, double hostRate, double nowSeconds) {
  flushStalled();
  applyUi();
  applyWaves();
  if (!(hostRate >= Machine::kMinHostRate && hostRate <= Machine::kMaxHostRate)) {  // NaN too
    std::fill_n(out, frames, 0.0f);
    const size_t stale = discardMidiInput();  // stale by the time audio resumes
    midiDropped_.fetch_add(stale, kRelaxed);
    midiBytesTaken_ += stale;
    publishStatus();
    return;
  }

  // Place this block's MIDI events: frames - round((now - t) * rate), kept in
  // [previous offset, frames - 1]. Clamping the distance to [-1, frames + 1]
  // first keeps llround in range for timestamps from a stalled or mismatched
  // clock; a NaN timestamp counts as `now`.
  const size_t last = frames ? frames - 1 : 0;
  size_t count = 0, prev = 0;
  while (count < kMidiCommandsPerBlock && q_->midi.pop(q_->events[count])) {
    double delta = (nowSeconds - q_->events[count].time) * hostRate;
    if (std::isnan(delta)) delta = 0.0;
    delta = std::clamp(delta, -1.0, double(frames) + 1.0);
    const long long at = static_cast<long long>(frames) - std::llround(delta);
    prev = std::min(at < static_cast<long long>(prev) ? prev : size_t(at), last);
    q_->offsets[count] = prev;
    midiBytesTaken_ += q_->events[count++].size;
  }

  // Render up to each event, queue it, carry on; due releases and swap
  // inserts are applied at the end of every segment.
  size_t pos = 0;
  for (size_t i = 0; i < count; ++i) {
    if (q_->offsets[i] > pos) {
      machine_->render(out + pos, q_->offsets[i] - pos, hostRate);
      pos = q_->offsets[i];
      applyDue();
    }
    const auto& e = q_->events[i];
    machine_->queueMidi(std::span<const uint8_t>(e.bytes.data(), e.size));
  }
  if (pos < frames) {
    machine_->render(out + pos, frames - pos, hostRate);
    applyDue();
  }
  publishStatus();
}

void RealtimeHost::applyUi() {
  using Type = Queues::UiCommand::Type;
  Queues::UiCommand c;
  // Each command hands back at most one disk. While there is nowhere to put
  // one, the rest wait in the queue for a later block.
  while (stalledCount_ < kStalledSlots && q_->ui.pop(c)) {
    const uint64_t now = machine_->cycles();
    switch (c.type) {
      case Type::Key: {
        const size_t k = size_t(c.key);
        if (c.down) {
          machine_->setKey(c.key, true);
          pressCycle_[k] = now;
          releaseDue_[k] = false;
        } else if (machine_->keyDown(c.key) && now - pressCycle_[k] < kMinKeyHoldCycles) {
          releaseDue_[k] = true;
        } else {
          machine_->setKey(c.key, false);
          releaseDue_[k] = false;
        }
        break;
      }
      case Type::Insert:
        if (!pendingInsert_ && !machine_->disk()) {
          returnDisk(machine_->insertDisk(std::move(c.disk)), DiskReturn::Ejected);  // null
          break;
        }
        if (pendingInsert_) {
          returnDisk(std::move(pendingInsert_), DiskReturn::Unused);
        } else {
          returnDisk(machine_->ejectDisk(), DiskReturn::Ejected);
          ejectCycle_ = now;
        }
        pendingInsert_ = std::move(c.disk);
        break;
      case Type::Eject:
        if (pendingInsert_) returnDisk(std::move(pendingInsert_), DiskReturn::Unused);
        else returnDisk(machine_->ejectDisk(), DiskReturn::Ejected);
        break;
      case Type::Reset:
        countRingOverflows();
        machine_->powerOn();  // cycles() restarts at 0: rebase the pending timers
        ringOverflowsSeen_ = 0;
        pressCycle_.fill(0);
        ejectCycle_ = 0;
        break;
      case Type::Panic:
        midiBytesFlushed_ += discardMidiInput();  // or it would follow the All Notes Off in this block
        machine_->flushMidi();
        machine_->queueMidi(kPanicBytes);
        break;
      case Type::Copy:
        if (const DiskImage* d = machine_->disk()) {
          d->copyTo(*c.disk);
          returnDisk(std::move(c.disk), DiskReturn::Copy, d->writeCount());
        } else {
          returnDisk(std::move(c.disk), DiskReturn::NoDiskToCopy);
        }
        break;
      case Type::MarkSaved:
        if (DiskImage* d = machine_->disk(); d && d->writeCount() == c.writeCount) d->clearDirty();
        break;
    }
  }
}

void RealtimeHost::applyWaves() {
  // Take a write only when its return has room: from the producer side
  // sizeApprox() can only overstate the fill, so the push cannot fail. Until
  // the UI takes some back, the rest wait in the wave queue.
  std::unique_ptr<WaveWrite> w;
  while (q_->waveReturns.sizeApprox() < kWaveQueueWrites && q_->waves.pop(w)) {
    WaveReturn why = WaveReturn::RefusedDriveBusy;
    if (!machine_->driveActive()) {  // the OS may be loading a sound into wave RAM
      machine_->writeWaveRam(w->address, w->bytes);
      why = WaveReturn::Written;
    }
    q_->waveReturns.push(ReturnedWave{std::move(w), why});
  }
}

void RealtimeHost::applyDue() {
  const uint64_t now = machine_->cycles();
  for (size_t k = 0; k < kPanelKeyCount; ++k) {
    if (releaseDue_[k] && now - pressCycle_[k] >= kMinKeyHoldCycles) {
      machine_->setKey(PanelKey(k), false);
      releaseDue_[k] = false;
    }
  }
  if (pendingInsert_ && now >= ejectCycle_ + kSwapGapCycles && stalledCount_ < kStalledSlots)
    returnDisk(machine_->insertDisk(std::move(pendingInsert_)), DiskReturn::Ejected);  // null
}

void RealtimeHost::returnDisk(std::unique_ptr<DiskImage> disk, DiskReturn why,
                              uint64_t writeCount) {
  if (!disk) return;
  ReturnedDisk r{std::move(disk), why, writeCount};
  // Behind any stalled ones, to keep the order. Callers make sure a slot is free.
  if (stalledCount_ == 0 && q_->returns.push(std::move(r))) return;
  stalled_[stalledCount_++] = std::move(r);
}

void RealtimeHost::flushStalled() {
  size_t sent = 0;
  while (sent < stalledCount_ && q_->returns.push(std::move(stalled_[sent]))) ++sent;
  for (size_t i = sent; i < stalledCount_; ++i) stalled_[i - sent] = std::move(stalled_[i]);
  stalledCount_ -= sent;
}

void RealtimeHost::countRingOverflows() {
  const uint64_t overflows = machine_->midiOverflows();
  if (overflows > ringOverflowsSeen_) midiDropped_.fetch_add(overflows - ringOverflowsSeen_, kRelaxed);
  ringOverflowsSeen_ = overflows;
}

size_t RealtimeHost::discardMidiInput() {
  // We are the consumer, so popping is safe. At most one queue's worth: the
  // producer may keep posting while we drain.
  size_t bytes = 0;
  for (size_t i = 0; i < kMidiQueueCommands && q_->midi.pop(q_->events[0]); ++i)
    bytes += q_->events[0].size;
  return bytes;
}

void RealtimeHost::publishStatus() {
  countRingOverflows();
  const DisplayState d = machine_->display();
  display_.store(uint16_t(d.left | d.right << 8), kRelaxed);
  driveActive_.store(machine_->driveActive(), kRelaxed);
  const DiskImage* disk = machine_->disk();
  diskPresent_.store(disk != nullptr, kRelaxed);
  // diskWrites before diskDirty, the latter with release; status() loads them
  // in the opposite order with acquire. A reader that sees a block's diskDirty
  // therefore sees that block's diskWrites or a later one, never an older count
  // (which DiskSession::modified would compare against a saved copy's).
  diskWrites_.store(disk ? disk->writeCount() : 0, kRelaxed);
  diskDirty_.store(disk && disk->dirty(), std::memory_order_release);
  illegalOps_.store(machine_->illegalOps(), kRelaxed);
  cycles_.store(machine_->cycles(), kRelaxed);
  // Depth first, then taken and flushed with release; status() loads them in
  // the opposite order with acquire. A reader that sees a block's taken count
  // therefore sees that block's depth or a later one, never an older one, so
  // posted - taken - flushed + depth can only overstate the bytes in flight
  // (MidiInFeeder relies on that: an older depth with a newer taken count would
  // hide bytes that moved from the host queue into the Machine's ring).
  midiDepthPub_.store(machine_->midiQueueDepth(), kRelaxed);
  midiFlushedPub_.store(midiBytesFlushed_, std::memory_order_release);
  midiTakenPub_.store(midiBytesTaken_, std::memory_order_release);
}

bool RealtimeHost::postMidi(std::span<const uint8_t> bytes, double timeSeconds) {
  if (bytes.empty()) return true;
  const size_t need = (bytes.size() + kMidiCommandBytes - 1) / kMidiCommandBytes;
  // From the producer side sizeApprox() can only overstate the fill, so the
  // pushes below cannot fail.
  if (kMidiQueueCommands - q_->midi.sizeApprox() < need) {
    midiDropped_.fetch_add(bytes.size(), kRelaxed);
    return false;
  }
  for (size_t i = 0; i < bytes.size(); i += kMidiCommandBytes) {
    Queues::MidiCommand c;
    c.time = timeSeconds;
    c.size = uint8_t(std::min(kMidiCommandBytes, bytes.size() - i));
    std::copy_n(bytes.begin() + std::ptrdiff_t(i), c.size, c.bytes.begin());
    q_->midi.push(std::move(c));
  }
  return true;
}

bool RealtimeHost::postKey(PanelKey key, bool down) {
  if (size_t(key) >= kPanelKeyCount) return false;
  Queues::UiCommand c;
  c.type = Queues::UiCommand::Type::Key;
  c.key = key;
  c.down = down;
  return q_->ui.push(std::move(c));
}

bool RealtimeHost::postInsert(std::unique_ptr<DiskImage>&& disk) {
  if (!disk) return false;
  Queues::UiCommand c;
  c.type = Queues::UiCommand::Type::Insert;
  c.disk = std::move(disk);
  if (q_->ui.push(std::move(c))) return true;
  disk = std::move(c.disk);  // a full queue leaves c untouched
  return false;
}

bool RealtimeHost::postEject() {
  Queues::UiCommand c;
  c.type = Queues::UiCommand::Type::Eject;
  return q_->ui.push(std::move(c));
}

bool RealtimeHost::postReset() {
  Queues::UiCommand c;
  c.type = Queues::UiCommand::Type::Reset;
  return q_->ui.push(std::move(c));
}

bool RealtimeHost::postPanic() {
  Queues::UiCommand c;
  c.type = Queues::UiCommand::Type::Panic;
  return q_->ui.push(std::move(c));
}

bool RealtimeHost::postCopyDisk(std::unique_ptr<DiskImage>&& dest) {
  if (!dest) return false;
  Queues::UiCommand c;
  c.type = Queues::UiCommand::Type::Copy;
  c.disk = std::move(dest);
  if (q_->ui.push(std::move(c))) return true;
  dest = std::move(c.disk);
  return false;
}

bool RealtimeHost::postMarkSaved(uint64_t writeCount) {
  Queues::UiCommand c;
  c.type = Queues::UiCommand::Type::MarkSaved;
  c.writeCount = writeCount;
  return q_->ui.push(std::move(c));
}

bool RealtimeHost::postWaveWrite(std::unique_ptr<WaveWrite>&& w) {
  // Checked here so the audio thread never meets writeWaveRam's throw.
  if (!w || w->address > Machine::kSampleRamSize ||
      w->bytes.size() > Machine::kSampleRamSize - w->address)
    return false;
  return q_->waves.push(std::move(w));  // a full queue leaves w untouched
}

std::optional<ReturnedWave> RealtimeHost::takeReturnedWave() {
  ReturnedWave r{};
  if (!q_->waveReturns.pop(r)) return std::nullopt;
  return r;
}

std::optional<ReturnedDisk> RealtimeHost::takeReturnedDisk() {
  ReturnedDisk r{};
  if (!q_->returns.pop(r)) return std::nullopt;
  return r;
}

bool RealtimeHost::popMidiOut(MidiOutByte& out) { return q_->midiOut.pop(out); }

HostStatus RealtimeHost::status() const {
  HostStatus s;
  const uint16_t d = display_.load(kRelaxed);
  s.display.left = uint8_t(d);
  s.display.right = uint8_t(d >> 8);
  s.driveActive = driveActive_.load(kRelaxed);
  s.diskPresent = diskPresent_.load(kRelaxed);
  s.diskDirty = diskDirty_.load(std::memory_order_acquire);  // before diskWrites: see publishStatus
  s.diskWrites = diskWrites_.load(kRelaxed);
  s.illegalOps = illegalOps_.load(kRelaxed);
  s.midiDropped = midiDropped_.load(kRelaxed);
  s.cycles = cycles_.load(kRelaxed);
  // taken and flushed (acquire) before depth: see publishStatus.
  s.midiBytesTaken = midiTakenPub_.load(std::memory_order_acquire);
  s.midiBytesFlushed = midiFlushedPub_.load(std::memory_order_acquire);
  s.midiQueueDepth = midiDepthPub_.load(kRelaxed);
  return s;
}

}  // namespace mirage
