#include "mirage/state_io.h"

#include <algorithm>
#include <bit>
#include <string>

namespace mirage {

namespace {
void putLE(std::vector<uint8_t>& buf, uint64_t v, size_t n) {
  for (size_t i = 0; i < n; ++i) buf.push_back(uint8_t(v >> (8 * i)));
}
}  // namespace

void StateWriter::u8(uint8_t v) { buf_.push_back(v); }
void StateWriter::u16(uint16_t v) { putLE(buf_, v, 2); }
void StateWriter::u32(uint32_t v) { putLE(buf_, v, 4); }
void StateWriter::u64(uint64_t v) { putLE(buf_, v, 8); }
void StateWriter::i32(int32_t v) { u32(std::bit_cast<uint32_t>(v)); }
void StateWriter::f64(double v) { u64(std::bit_cast<uint64_t>(v)); }
void StateWriter::bytes(std::span<const uint8_t> data) {
  buf_.insert(buf_.end(), data.begin(), data.end());
}
void StateWriter::tag(uint32_t fourcc) { u32(fourcc); }
std::vector<uint8_t> StateWriter::take() { return std::move(buf_); }

uint64_t StateReader::readLE(size_t n) {
  if (data_.size() - pos_ < n) throw StateError("state read past end");
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) v |= uint64_t(data_[pos_ + i]) << (8 * i);
  pos_ += n;
  return v;
}

uint8_t StateReader::u8() { return uint8_t(readLE(1)); }
uint16_t StateReader::u16() { return uint16_t(readLE(2)); }
uint32_t StateReader::u32() { return uint32_t(readLE(4)); }
uint64_t StateReader::u64() { return readLE(8); }
int32_t StateReader::i32() { return std::bit_cast<int32_t>(u32()); }
double StateReader::f64() { return std::bit_cast<double>(u64()); }

void StateReader::bytes(std::span<uint8_t> out) {
  if (data_.size() - pos_ < out.size()) throw StateError("state read past end");
  std::copy_n(data_.begin() + pos_, out.size(), out.begin());
  pos_ += out.size();
}

void StateReader::tag(uint32_t expected) {
  const uint32_t got = u32();
  if (got != expected) {
    throw StateError("state tag mismatch: expected " + std::to_string(expected) +
                     ", got " + std::to_string(got));
  }
}

}  // namespace mirage
