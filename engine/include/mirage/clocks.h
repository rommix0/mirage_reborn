#pragma once
#include <cstdint>

namespace mirage {

inline constexpr uint32_t kMasterClockHz = 8'000'000;
inline constexpr uint32_t kCpuClockHz = 2'000'000;
// One MIDI byte on the wire: 10 bits (start + 8 data + stop) at 31250 baud.
inline constexpr double kMidiByteSeconds = 10.0 / 31250.0;
// Floppy: 250 kbit/s MFM = one data byte every 32 us (64 E cycles); 300 rpm =
// one index pulse every 200 ms (400000 E cycles).
inline constexpr uint32_t kFdcBitRate = 250'000;
inline constexpr uint32_t kFdcByteCycles = kCpuClockHz * 8 / kFdcBitRate;
inline constexpr uint32_t kDiskRpm = 300;
inline constexpr uint32_t kDiskIndexCycles = kCpuClockHz * 60 / kDiskRpm;
inline constexpr uint8_t kPitchWheelRest = 0x80;
inline constexpr uint8_t kModWheelRest = 0x00;

}  // namespace mirage
