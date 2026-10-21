#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

#include "mirage/realtime_host.h"

namespace mirage {

// The only way MIDI input reaches a RealtimeHost. The Machine's MIDI input
// ring holds RealtimeHost's 4096 bytes and the host queue takes 12 KB at most,
// but a SysEx message (a wavesample dump) can be hundreds of KB and arrives
// all at once. The feeder keeps the rest back and posts it as the machine
// consumes it, so that no more than kMaxInFlight bytes are between the feeder
// and the ACIA at any time (posted to the host and not yet delivered to the
// ACIA: waiting in the host queue or in the machine's ring).
//
// While nothing is backlogged, a message that fits the in-flight budget goes
// straight to postMidi with its own timestamp, so playing is unchanged. A
// message that does not fit, or any message while a backlog exists, waits in
// order behind the backlog and is posted by pump() in chunks. Chunks carry
// their message's own timestamp: once it is old the audio thread places the
// chunk at the start of its block, which is as good as "now" for bytes that
// the machine paces at 320 us each anyway.
//
// A mutex serialises push, pump and clear, so postMidi keeps one producer at a
// time. Never call any of them from the audio thread: they lock and allocate.
class MidiInFeeder {
 public:
  static constexpr size_t kMaxInFlight = 2048;
  static constexpr size_t kMaxBacklog = 1 << 20;  // 1 MiB

  explicit MidiInFeeder(RealtimeHost& host);

  // MIDI-input thread. A message that would take the backlog past kMaxBacklog
  // is dropped whole and counted in dropped().
  void push(std::span<const uint8_t> msg, double timeSeconds);
  // Any non-audio thread, at least 200 Hz (the app's MIDI-out thread): posts as
  // much of the backlog as the in-flight budget allows.
  void pump();
  // UI thread, together with RealtimeHost::postPanic: drops the backlog. Call it
  // first, so that nothing is posted behind the panic.
  void clear();

  size_t backlog() const;     // bytes waiting
  uint64_t dropped() const;   // whole messages over kMaxBacklog
  uint64_t droppedBytes() const;  // the bytes of those messages

 private:
  struct Pending {
    std::vector<uint8_t> bytes;
    size_t sent = 0;  // bytes already posted
    double time = 0;
  };
  // Bytes posted to the host and not yet delivered to the ACIA. Locked.
  size_t inFlight() const;

  RealtimeHost& host_;
  mutable std::mutex mutex_;
  std::deque<Pending> queue_;
  size_t backlogBytes_ = 0;
  uint64_t posted_ = 0;  // bytes the host accepted from this feeder
  uint64_t dropped_ = 0, droppedBytes_ = 0;
};

}  // namespace mirage
