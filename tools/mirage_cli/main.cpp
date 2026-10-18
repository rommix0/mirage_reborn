#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mirage/disasm6809.h"

using namespace mirage;

namespace {

int usage() {
  std::fputs(
      "usage:\n"
      "  mirage_cli disasm <file> [--org ADDR] [--from ADDR --to ADDR]\n"
      "      Linear 6809 disassembly of a raw binary. --org defaults to 0xF000 for\n"
      "      a 4096-byte file (the boot ROM), else 0. --from/--to (inclusive)\n"
      "      default to the whole file. Addresses accept 0x/$ hex or decimal.\n",
      stderr);
  return 2;
}

std::optional<uint32_t> parseAddr(std::string_view s) {
  std::string t(s);
  int base = 0;
  if (!t.empty() && t[0] == '$') {
    t.erase(0, 1);
    base = 16;
  }
  if (t.empty()) return std::nullopt;
  char* end = nullptr;
  const unsigned long v = std::strtoul(t.c_str(), &end, base);
  if (*end != '\0' || v > 0xFFFF) return std::nullopt;
  return uint32_t(v);
}

int disasm(int argc, char** argv) {
  if (argc < 1) return usage();
  const std::string path = argv[0];
  std::optional<uint32_t> org, from, to;
  for (int i = 1; i < argc; ++i) {
    const std::string_view opt = argv[i];
    if (i + 1 >= argc) return usage();
    const auto v = parseAddr(argv[++i]);
    if (!v) {
      std::fprintf(stderr, "bad address for %s: %s\n", argv[i - 1], argv[i]);
      return 2;
    }
    if (opt == "--org") org = v;
    else if (opt == "--from") from = v;
    else if (opt == "--to") to = v;
    else return usage();
  }

  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return 1;
  }
  const std::vector<uint8_t> data{std::istreambuf_iterator<char>(f),
                                  std::istreambuf_iterator<char>()};
  if (data.empty()) {
    std::fprintf(stderr, "%s is empty\n", path.c_str());
    return 1;
  }
  const uint32_t base = org.value_or(data.size() == 4096 ? 0xF000u : 0u);
  const uint32_t last = base + uint32_t(data.size()) - 1;
  if (last > 0xFFFF) {
    std::fprintf(stderr, "%zu bytes at $%04X do not fit in 64 KB\n", data.size(), base);
    return 1;
  }
  const uint32_t start = from.value_or(base);
  const uint32_t stop = to.value_or(last);
  if (start < base || stop > last || start > stop) {
    std::fprintf(stderr, "range $%04X-$%04X is outside the file ($%04X-$%04X)\n", start, stop,
                 base, last);
    return 1;
  }

  // Bytes past the end of the file read as $FF (erased EPROM).
  const auto peek = [&](uint16_t a) -> uint8_t {
    return (a >= base && a <= last) ? data[a - base] : 0xFF;
  };
  for (uint32_t pc = start; pc <= stop;) {
    const Disasm d = disassemble6809(peek, uint16_t(pc));
    std::string bytes;
    for (int i = 0; i < d.length; ++i) {
      char b[4];
      std::snprintf(b, sizeof b, "%02X ", peek(uint16_t(pc + i)));
      bytes += b;
    }
    std::printf("%04X  %-15s %s\n", pc, bytes.c_str(), d.text.c_str());
    pc += uint32_t(d.length);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "disasm") return disasm(argc - 2, argv + 2);
  return usage();
}
