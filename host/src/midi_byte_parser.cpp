#include "mirage/midi_byte_parser.h"

namespace mirage {

namespace {
// Total length of a channel message with this status (80-EF).
uint8_t channelLength(uint8_t status) {
  const uint8_t hi = status & 0xF0;
  return (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
}
}  // namespace

void MidiByteParser::feed(uint8_t b, const std::function<void(std::span<const uint8_t>)>& emit) {
  if (b >= 0xF8) {  // real-time: passes through alone, state untouched
    emit(std::span<const uint8_t>(&b, 1));
    return;
  }
  if (b == 0xF7 && inSysex_) {  // end of SysEx: emit it whole
    msg_.push_back(b);
    emit(std::span<const uint8_t>(msg_));
    inSysex_ = false;
    msg_.clear();
    return;
  }
  if (b >= 0x80) {
    // Any other status byte abandons what was in progress (an unfinished SysEx is dropped).
    inSysex_ = false;
    msg_.clear();
    need_ = 0;
    if (b < 0xF0) {
      running_ = b;
      msg_.push_back(b);
      need_ = channelLength(b);
      return;
    }
    running_ = 0;  // system common cancels running status
    switch (b) {
      case 0xF0:
        inSysex_ = true;
        msg_.push_back(b);
        break;
      case 0xF1: case 0xF3:
        msg_.push_back(b);
        need_ = 2;
        break;
      case 0xF2:
        msg_.push_back(b);
        need_ = 3;
        break;
      case 0xF6:
        emit(std::span<const uint8_t>(&b, 1));
        break;
      default:
        break;  // F4/F5 undefined and a stray F7: dropped
    }
    return;
  }
  // Data byte.
  if (inSysex_) {
    msg_.push_back(b);
    return;
  }
  if (msg_.empty()) {
    if (running_ == 0) return;  // stray data byte
    msg_.push_back(running_);
    need_ = channelLength(running_);
  }
  msg_.push_back(b);
  if (msg_.size() == need_) {
    emit(std::span<const uint8_t>(msg_));
    msg_.clear();
  }
}

}  // namespace mirage
