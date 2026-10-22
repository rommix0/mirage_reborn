// SHA-1 (engine-internal) and the boot ROM's identity.
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>
#include <vector>

#include "asset_paths.h"
#include "booted_machine.h"
#include "sha1.h"

using namespace mirage;

namespace {
std::string hex(const std::array<uint8_t, 20>& d) {
  std::string s;
  char b[3];
  for (uint8_t x : d) {
    std::snprintf(b, sizeof b, "%02x", x);
    s += b;
  }
  return s;
}
std::string sha(const std::string& text) {
  return hex(sha1(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()),
                                           text.size())));
}
}  // namespace

TEST_CASE("sha1: published test vectors") {
  CHECK(sha("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(sha("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  CHECK(sha(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("sha1: the real boot ROM matches the known firmware hash", "[assets]") {
  REQUIRE_ASSET(romPath());
  const auto rom = mirage_test::bootRomBytes();
  CHECK(hex(sha1(rom)) == "ec6ea5613eeafd21d8f3a7431a35a6ff16eed56d");
  CHECK(hex(Machine(rom).romSha1()) == "ec6ea5613eeafd21d8f3a7431a35a6ff16eed56d");
}
