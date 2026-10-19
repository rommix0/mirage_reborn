#pragma once
// Internal: a path as UTF-8 text for error messages. path::string() converts
// through the ANSI code page on Windows and throws for names it cannot map.
#include <filesystem>
#include <string>

namespace mirage {

inline std::string pathText(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

}  // namespace mirage
