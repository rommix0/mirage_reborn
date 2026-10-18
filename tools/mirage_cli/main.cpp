#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <ostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "mirage/clocks.h"
#include "mirage/disasm6809.h"
#include "mirage/disk_image.h"
#include "mirage/machine.h"
#include "mirage/panel.h"
#include "non_interactive.h"
#include "run_options.h"

#ifndef MIRAGE_ASSET_ROOT
#error "MIRAGE_ASSET_ROOT must be defined by the build"
#endif

using namespace mirage;

namespace {

int usage() {
  std::fputs(
      "usage:\n"
      "  mirage_cli disasm <file> [--org ADDR] [--from ADDR --to ADDR]\n"
      "      Linear 6809 disassembly of a raw binary. --org defaults to 0xF000 for\n"
      "      a 4096-byte file (the boot ROM), else 0. --from/--to (inclusive)\n"
      "      default to the whole file. Addresses accept 0x/$ hex or decimal.\n"
      "\n"
      "  mirage_cli run [--disk <img>] [--rom <bin>] [--seconds S]\n"
      "                 [--key NAME@T[:HOLD]]... [--note N:VEL@T:DUR]...\n"
      "                 [--midi-hex \"90 3C 64\"@T]...\n"
      "                 [--wav out.wav --rate 48000] [--trace trace.txt [--trace-from T]]\n"
      "                 [--snapshot-out f] [--snapshot-in f]\n"
      "      Powers on and runs the machine headless for S emulated seconds\n"
      "      (default 10); without --disk the drive is empty. --rom defaults to\n"
      "      <asset root>/Ensoniq Mirage EPROM Firmware/mirage.bin. Times T are\n"
      "      seconds from power-on. NAME is a panel key in any case (LoadUpper,\n"
      "      K0..K9, Enter, Up, Down, Param, Value, Cancel, ...), held HOLD s\n"
      "      (default 0.1). --note sends note-on N VEL (channel 1) at T and\n"
      "      note-off at T+DUR. --trace writes one line per instruction from\n"
      "      --trace-from (default 0). At the end prints the display segments,\n"
      "      the illegal-opcode count, the FDC sectors read and the E cycles.\n"
      "      --wav/--rate (Task 12) and --snapshot-* (Task 13) are not available\n"
      "      yet and fail.\n",
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

// ---- run ---------------------------------------------------------------------

std::vector<uint8_t> readAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// One trace line per instruction: cycle count, PC, instruction, registers.
void traceLine(std::ostream& out, Machine& m, uint16_t pc, const Regs& r) {
  const Disasm d = disassemble6809([&m](uint16_t a) { return m.peek(a); }, pc);
  char line[128];
  const int n = std::snprintf(
      line, sizeof line,
      "%10llu %04X  %-22s A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X DP=%02X CC=%02X\n",
      static_cast<unsigned long long>(m.cycles()), pc, d.text.c_str(), r.a, r.b, r.x, r.y, r.u,
      r.s, r.dp, r.cc);
  out.write(line, std::min<std::streamsize>(n, std::streamsize(sizeof line - 1)));
}

int run(int argc, char** argv) {
  const std::string defaultRom =
      (std::filesystem::path(MIRAGE_ASSET_ROOT) / "Ensoniq Mirage EPROM Firmware" / "mirage.bin")
          .string();
  auto parsed = cli::parseRunArgs(std::vector<std::string>(argv, argv + argc), defaultRom);
  if (const auto* err = std::get_if<cli::RunArgsError>(&parsed)) {
    if (err->message.empty()) return usage();
    std::fprintf(stderr, "%s%s", err->message.c_str(),
                 err->message.back() == '\n' ? "" : "\n");
    return err->exitCode;
  }
  const cli::RunOptions& o = std::get<cli::RunOptions>(parsed);

  const std::vector<uint8_t> rom = readAll(o.rom);
  if (rom.size() != Machine::kBootRomSize) {
    std::fprintf(stderr, "%s: cannot read a 4096-byte boot ROM\n", o.rom.c_str());
    return 1;
  }
  auto m = std::make_unique<Machine>(rom);  // large: keep it off the stack
  if (!o.disk.empty()) {
    try {
      m->insertDisk(DiskImage::loadFile(o.disk));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s: %s\n", o.disk.c_str(), e.what());
      return 1;
    }
  }
  m->powerOn();

  std::ofstream trace;
  if (!o.trace.empty()) {
    trace.open(o.trace, std::ios::binary);
    if (!trace) {
      std::fprintf(stderr, "cannot write %s\n", o.trace.c_str());
      return 1;
    }
  }
  Machine& mm = *m;
  cli::runSchedule(mm, o, [&trace, &mm](uint16_t pc, const Regs& r) {
    traceLine(trace, mm, pc, r);
  });

  const DisplayState d = m->display();
  std::printf("display: %02X %02X\n", d.left, d.right);
  std::printf("illegal: %llu\n", static_cast<unsigned long long>(m->cpu().illegalCount()));
  std::printf("sectors: %llu\n", static_cast<unsigned long long>(m->fdc().sectorsRead()));
  std::printf("cycles: %llu\n", static_cast<unsigned long long>(m->cycles()));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  cli::makeNonInteractive();
  if (argc >= 2 && std::string_view(argv[1]) == "disasm") return disasm(argc - 2, argv + 2);
  if (argc >= 2 && std::string_view(argv[1]) == "run") return run(argc - 2, argv + 2);
  return usage();
}
