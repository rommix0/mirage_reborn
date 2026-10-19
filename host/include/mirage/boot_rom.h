#pragma once
#include <array>
#include <cstdint>

namespace mirage {

// SHA-1 of the known-good Mirage boot ROM image (mirage.bin, 4096 bytes).
inline constexpr std::array<uint8_t, 20> kKnownBootRomSha1 = {
    0xec, 0x6e, 0xa5, 0x61, 0x3e, 0xea, 0xfd, 0x21, 0xd8, 0xf3,
    0xa7, 0x43, 0x1a, 0x35, 0xa6, 0xff, 0x16, 0xee, 0xd5, 0x6d};

// True when `sha1` (Machine::romSha1()) is the known boot ROM's hash.
bool isKnownBootRom(const std::array<uint8_t, 20>& sha1);

}  // namespace mirage
