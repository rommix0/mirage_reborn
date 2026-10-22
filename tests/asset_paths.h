#pragma once
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string_view>

#ifndef MIRAGE_ASSET_ROOT
#error "MIRAGE_ASSET_ROOT must be defined by the build"
#endif
#ifndef MIRAGE_ROM
#error "MIRAGE_ROM must be defined by the build"
#endif

// Path of a user-owned disk image relative to MIRAGE_ASSET_ROOT.
inline std::filesystem::path assetPath(std::string_view rel) {
  return std::filesystem::path(MIRAGE_ASSET_ROOT) / std::filesystem::path(rel);
}

// The boot ROM (MIRAGE_ROM; emulator/mirage.bin by default).
inline std::filesystem::path romPath() { return std::filesystem::path(MIRAGE_ROM); }

// Skips the current test case when an asset file is missing.
#define REQUIRE_ASSET(p)                                         \
  do {                                                           \
    if (!std::filesystem::exists(p)) {                           \
      SKIP("missing asset: " << std::filesystem::path(p).string()); \
    }                                                            \
  } while (false)
