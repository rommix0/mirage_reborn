#pragma once
#include <filesystem>
#include <span>

namespace mirage {

// Writes mono 16-bit PCM. A sample x becomes round(x * 32768) clamped to
// [-32768, 32767], so 0.5 is 16384 and values at or beyond +-1 saturate.
// Throws std::runtime_error when the file cannot be written.
void writeWav(const std::filesystem::path& path, std::span<const float> samples, int rate);

}  // namespace mirage
