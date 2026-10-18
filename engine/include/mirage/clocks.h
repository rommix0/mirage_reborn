#pragma once
#include <cstdint>

namespace mirage {

inline constexpr uint32_t kMasterClockHz = 8'000'000;
inline constexpr uint32_t kCpuClockHz = 2'000'000;
// One MIDI byte on the wire: 10 bits (start + 8 data + stop) at 31250 baud.
inline constexpr double kMidiByteSeconds = 10.0 / 31250.0;
inline constexpr uint8_t kPitchWheelRest = 0x80;
inline constexpr uint8_t kModWheelRest = 0x00;

}  // namespace mirage
