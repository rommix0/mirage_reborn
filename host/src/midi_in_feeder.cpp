#include "mirage/midi_in_feeder.h"

#include <algorithm>

namespace mirage {

namespace {
// What one postMidi call can take.
constexpr size_t kMaxPost = RealtimeHost::kMidiQueueCommands * RealtimeHost::kMidiCommandBytes;
}  // namespace

MidiInFeeder::MidiInFeeder(RealtimeHost& host) : host_(host) {}

size_t MidiInFeeder::inFlight() const {
  // posted - (taken + flushed) is what is still in the host queue; the depth is
  // what the machine holds. The status is up to a block old, and status()
  // never pairs a taken count with an older depth (see publishStatus), so a
  // stale or torn read can only overstate: a stale taken count counts bytes as
  // still queued after they moved on, and a depth from a later block than the
  // taken count counts bytes both ways. It must never understate, or the
  // limit could be broken.
  const HostStatus s = host_.status();
  const uint64_t gone = s.midiBytesTaken + s.midiBytesFlushed;
  return size_t(posted_ > gone ? posted_ - gone : 0) + s.midiQueueDepth;
}

void MidiInFeeder::push(std::span<const uint8_t> msg, double timeSeconds) {
  if (msg.empty()) return;
  std::lock_guard lock(mutex_);
  if (queue_.empty() && msg.size() <= kMaxPost && inFlight() + msg.size() <= kMaxInFlight &&
      host_.postMidi(msg, timeSeconds)) {
    posted_ += msg.size();
    return;
  }
  if (backlogBytes_ + msg.size() > kMaxBacklog) {
    ++dropped_;
    droppedBytes_ += msg.size();
    return;
  }
  queue_.push_back({std::vector<uint8_t>(msg.begin(), msg.end()), 0, timeSeconds});
  backlogBytes_ += msg.size();
}

void MidiInFeeder::pump() {
  std::lock_guard lock(mutex_);
  while (!queue_.empty()) {
    const size_t used = inFlight();
    if (used >= kMaxInFlight) return;
    Pending& p = queue_.front();
    const size_t left = p.bytes.size() - p.sent;
    const size_t room = kMaxInFlight - used;
    // Not less than a full host command unless the message ends: many tiny
    // chunks would use up the host queue's commands, not its bytes.
    if (room < std::min(left, RealtimeHost::kMidiCommandBytes)) return;
    const size_t n = std::min({left, room, kMaxPost});
    if (!host_.postMidi(std::span<const uint8_t>(p.bytes).subspan(p.sent, n), p.time)) return;
    posted_ += n;
    p.sent += n;
    backlogBytes_ -= n;
    if (p.sent == p.bytes.size()) queue_.pop_front();
  }
}

void MidiInFeeder::clear() {
  std::lock_guard lock(mutex_);
  queue_.clear();
  backlogBytes_ = 0;
}

size_t MidiInFeeder::backlog() const {
  std::lock_guard lock(mutex_);
  return backlogBytes_;
}

uint64_t MidiInFeeder::dropped() const {
  std::lock_guard lock(mutex_);
  return dropped_;
}

uint64_t MidiInFeeder::droppedBytes() const {
  std::lock_guard lock(mutex_);
  return droppedBytes_;
}

}  // namespace mirage
