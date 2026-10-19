#pragma once
// Internal: SHA-1 (FIPS 180-4), used only to identify the boot ROM in
// snapshots. Not for security.
#include <array>
#include <cstdint>
#include <span>

namespace mirage {

std::array<uint8_t, 20> sha1(std::span<const uint8_t> data);

}  // namespace mirage
