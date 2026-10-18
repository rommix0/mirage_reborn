#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace mirage {

// Packs a four-character tag ("TEST") into a uint32_t, first char in the
// most significant byte, without using multichar literals.
constexpr uint32_t fourcc(const char (&s)[5]) {
  return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
         (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}

struct StateError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Serialises machine state. All multi-byte values are little-endian.
class StateWriter {
 public:
  void u8(uint8_t v);
  void u16(uint16_t v);
  void u32(uint32_t v);
  void u64(uint64_t v);
  void i32(int32_t v);
  void f64(double v);
  void bytes(std::span<const uint8_t> data);  // raw, no length prefix
  void tag(uint32_t fourcc);
  std::vector<uint8_t> take();

 private:
  std::vector<uint8_t> buf_;
};

// Reads what StateWriter wrote. Overrun or tag mismatch throws StateError.
class StateReader {
 public:
  explicit StateReader(std::span<const uint8_t> data) : data_(data) {}
  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();
  int32_t i32();
  double f64();
  void bytes(std::span<uint8_t> out);  // fills out; no length prefix
  void tag(uint32_t expected);
  bool atEnd() const { return pos_ == data_.size(); }

 private:
  uint64_t readLE(size_t n);

  std::span<const uint8_t> data_;
  size_t pos_ = 0;
};

}  // namespace mirage
