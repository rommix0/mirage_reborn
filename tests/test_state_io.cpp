#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "mirage/state_io.h"

using namespace mirage;

TEST_CASE("state round-trips all widths") {
  StateWriter w; w.tag(fourcc("TEST")); w.u8(0xAB); w.u16(0xBEEF); w.u32(0xDEADBEEF);
  w.u64(0x0123456789ABCDEFull); w.i32(-5); w.f64(1.5);
  auto buf = w.take();
  StateReader r(buf); r.tag(fourcc("TEST"));
  REQUIRE(r.u8()==0xAB); REQUIRE(r.u16()==0xBEEF); REQUIRE(r.u32()==0xDEADBEEF);
  REQUIRE(r.u64()==0x0123456789ABCDEFull); REQUIRE(r.i32()==-5); REQUIRE(r.f64()==1.5);
  REQUIRE(r.atEnd());
}

TEST_CASE("state tag mismatch and overrun throw") {
  StateWriter w; w.tag(fourcc("AAAA")); auto buf = w.take();
  StateReader r(buf); REQUIRE_THROWS_AS(r.tag(fourcc("BBBB")), StateError);
  StateReader r2(buf); r2.tag(fourcc("AAAA")); REQUIRE_THROWS_AS(r2.u8(), StateError);
}

TEST_CASE("state is little-endian and bytes round-trip") {
  StateWriter w; w.u32(0x04030201); w.u16(0x0605);
  const uint8_t raw[3] = {7, 8, 9};
  w.bytes(raw);
  auto buf = w.take();
  const std::vector<uint8_t> expected = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  REQUIRE(buf == expected);

  StateReader r(buf); REQUIRE(r.u32()==0x04030201); REQUIRE(r.u16()==0x0605);
  uint8_t out[3] = {};
  r.bytes(out);
  REQUIRE(out[0]==7); REQUIRE(out[2]==9);
  REQUIRE(r.atEnd());
  REQUIRE_THROWS_AS(r.bytes(out), StateError);
}

TEST_CASE("StateReader cannot be built from a temporary vector") {
  // The reader only views its bytes, so a temporary would leave it dangling.
  static_assert(!std::is_constructible_v<StateReader, std::vector<uint8_t>&&>);
  static_assert(std::is_constructible_v<StateReader, const std::vector<uint8_t>&>);
  static_assert(std::is_constructible_v<StateReader, std::vector<uint8_t>&>);
  const std::vector<uint8_t> buf = {1, 2};
  StateReader r(buf);
  REQUIRE(r.u16() == 0x0201);
}

TEST_CASE("state_io: bounded writer fails instead of growing") {
  std::vector<uint8_t> out = {9, 9, 9};
  out.reserve(8);
  const uint8_t* data = out.data();
  const size_t capacity = out.capacity();
  {
    StateWriter w(out);  // clears out first
    CHECK(out.empty());
    w.u32(0x04030201);
    w.u16(0x0605);
    CHECK(w.ok());
    CHECK(out == std::vector<uint8_t>{1, 2, 3, 4, 5, 6});
    const uint8_t two[2] = {7, 8};
    w.bytes(two);  // exactly fills the capacity
    CHECK(w.ok());
    w.u8(0xAA);  // one byte too many: refused, never grown
    CHECK_FALSE(w.ok());
    w.bytes(std::span<const uint8_t>());  // still failed afterwards
    CHECK_FALSE(w.ok());
  }
  CHECK(out.capacity() == capacity);
  CHECK(out.data() == data);
  CHECK(out.size() <= capacity);

  // Every width refuses past the capacity.
  std::vector<uint8_t> small;
  small.reserve(7);
  StateWriter w2(small);
  w2.u64(1);
  CHECK_FALSE(w2.ok());
  CHECK(small.capacity() == 7);

  // The unbounded form is unchanged: it always succeeds.
  StateWriter w3;
  for (int i = 0; i < 1000; ++i) w3.u64(uint64_t(i));
  CHECK(w3.ok());
  CHECK(w3.take().size() == 8000);
}
