#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace mirage {

// Rebuilds whole MIDI messages from a raw byte stream (the engine's MIDI-out bytes).
// For the MIDI-out thread, not the audio thread: it may allocate.
//  - Channel messages (80-EF) honour running status; a complete message is emitted at once.
//  - System common: F1/F3 two bytes, F2 three, F6 one; F4/F5 (undefined) are dropped.
//    Any system common status cancels running status.
//  - Real-time F8-FF is emitted immediately as a single byte, even inside another message or
//    SysEx, without disturbing it.
//  - SysEx F0..F7 is emitted whole (F0 and F7 included) and cancels running status. A status
//    byte arriving mid-SysEx drops the unfinished SysEx (nothing is emitted for it) and is then
//    handled normally; a second F0 restarts it.
//  - Data bytes with no status to attach to, and a stray F7, are dropped.
class MidiByteParser {
 public:
  void feed(uint8_t b, const std::function<void(std::span<const uint8_t>)>& emit);

 private:
  uint8_t running_ = 0;       // running status (80-EF), 0 = none
  uint8_t need_ = 0;          // total length of the message in msg_
  bool inSysex_ = false;
  std::vector<uint8_t> msg_;  // message under construction; empty = none in progress
};

}  // namespace mirage
