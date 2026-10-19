#include "mirage/wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "path_text.h"

namespace mirage {

namespace {
void putLE(std::vector<uint8_t>& b, uint32_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void putTag(std::vector<uint8_t>& b, const char (&t)[5]) {
  for (int i = 0; i < 4; ++i) b.push_back(uint8_t(t[i]));
}
}  // namespace

void writeWav(const std::filesystem::path& path, std::span<const float> samples, int rate) {
  const uint64_t dataBytes = uint64_t(samples.size()) * 2;
  if (rate <= 0 || dataBytes > 0xFFFFFFFFull - 36)
    throw std::runtime_error("writeWav: bad rate or too many samples");
  std::vector<uint8_t> b;
  b.reserve(44 + dataBytes);
  putTag(b, "RIFF");
  putLE(b, uint32_t(36 + dataBytes), 4);
  putTag(b, "WAVE");
  putTag(b, "fmt ");
  putLE(b, 16, 4);
  putLE(b, 1, 2);  // PCM
  putLE(b, 1, 2);  // mono
  putLE(b, uint32_t(rate), 4);
  putLE(b, uint32_t(rate) * 2, 4);  // byte rate
  putLE(b, 2, 2);                   // block align
  putLE(b, 16, 2);                  // bits per sample
  putTag(b, "data");
  putLE(b, uint32_t(dataBytes), 4);
  for (float x : samples) {
    // NaN saturates to 0 rather than reaching the integer conversion.
    const double v = std::isnan(x) ? 0.0 : std::nearbyint(double(x) * 32768.0);
    putLE(b, uint16_t(int16_t(std::clamp(v, -32768.0, 32767.0))), 2);
  }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  f.flush();
  if (!f) throw std::runtime_error("cannot write " + pathText(path));
}

}  // namespace mirage
