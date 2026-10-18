#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <ostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mirage/clocks.h"
#include "mirage/disasm6809.h"
#include "mirage/disk_image.h"
#include "mirage/machine.h"
#include "mirage/panel.h"

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

constexpr const char* kKeyNames[] = {
    "LoadUpper", "LoadLower", "SampleUpper", "PlaySeq", "LoadSeq", "SaveSeq", "RecSeq",
    "SampleLower", "K3", "K6", "K9", "K5", "K8", "K0", "K2", "Enter",
    "K1", "K4", "K7", "Up", "Param", "Down", "Value", "Cancel"};
static_assert(std::size(kKeyNames) == size_t(PanelKey::Cancel) + 1);  // PanelKey order

std::optional<PanelKey> parseKey(std::string_view name) {
  const auto same = [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  };
  for (size_t i = 0; i < std::size(kKeyNames); ++i) {
    const std::string_view k = kKeyNames[i];
    if (k.size() == name.size() && std::equal(k.begin(), k.end(), name.begin(), same))
      return PanelKey(i);
  }
  return std::nullopt;
}

std::optional<double> parseSeconds(std::string_view s) {
  const std::string t(s);
  if (t.empty()) return std::nullopt;
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (*end != '\0' || !(v >= 0) || v > 1e6) return std::nullopt;
  return v;
}

std::optional<unsigned> parseUint(std::string_view s, unsigned max, int base) {
  const std::string t(s);
  if (t.empty()) return std::nullopt;
  char* end = nullptr;
  const unsigned long v = std::strtoul(t.c_str(), &end, base);
  if (*end != '\0' || v > max) return std::nullopt;
  return unsigned(v);
}

uint64_t toCycles(double seconds) { return uint64_t(seconds * kCpuClockHz + 0.5); }

// Splits "a<sep>b" at the last sep.
bool splitLast(std::string_view s, char sep, std::string_view& a, std::string_view& b) {
  const size_t p = s.rfind(sep);
  if (p == std::string_view::npos) return false;
  a = s.substr(0, p);
  b = s.substr(p + 1);
  return true;
}

struct Event {
  uint64_t cycle;
  int order;  // events at the same cycle run in command-line order
  std::function<void(Machine&)> act;
};

std::vector<uint8_t> readAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int badValue(std::string_view opt, std::string_view v) {
  std::fprintf(stderr, "bad value for %.*s: %.*s\n", int(opt.size()), opt.data(), int(v.size()),
               v.data());
  return 2;
}

// "90 3C 64" (spaces or commas between bytes) -> bytes; empty on error.
std::vector<uint8_t> parseHexBytes(std::string_view hex) {
  std::vector<uint8_t> bytes;
  size_t p = 0;
  const auto sep = [](char c) { return c == ' ' || c == ','; };
  while (p < hex.size()) {
    while (p < hex.size() && sep(hex[p])) ++p;
    size_t q = p;
    while (q < hex.size() && !sep(hex[q])) ++q;
    if (q > p) {
      const auto b = parseUint(hex.substr(p, q - p), 0xFF, 16);
      if (!b) return {};
      bytes.push_back(uint8_t(*b));
    }
    p = q;
  }
  return bytes;
}

// One trace line per CPU step: cycle count, PC, instruction, registers.
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
  std::string diskPath, tracePath;
  std::string romPath =
      (std::filesystem::path(MIRAGE_ASSET_ROOT) / "Ensoniq Mirage EPROM Firmware" / "mirage.bin")
          .string();
  double seconds = 10, traceFrom = 0;
  std::vector<Event> events;
  std::vector<std::string> unavailable;

  for (int i = 0; i < argc; ++i) {
    const std::string_view opt = argv[i];
    if (i + 1 >= argc) return usage();
    const std::string_view v = argv[++i];
    const int order = 2 * i;
    if (opt == "--disk") {
      diskPath = v;
    } else if (opt == "--rom") {
      romPath = v;
    } else if (opt == "--seconds") {
      const auto s = parseSeconds(v);
      if (!s) return badValue(opt, v);
      seconds = *s;
    } else if (opt == "--trace") {
      tracePath = v;
    } else if (opt == "--trace-from") {
      const auto s = parseSeconds(v);
      if (!s) return badValue(opt, v);
      traceFrom = *s;
    } else if (opt == "--key") {  // NAME@T[:HOLD]
      std::string_view name, when, hold = "0.1";
      if (!splitLast(v, '@', name, when)) return badValue(opt, v);
      const size_t colon = when.find(':');
      if (colon != std::string_view::npos) {
        hold = when.substr(colon + 1);
        when = when.substr(0, colon);
      }
      const auto key = parseKey(name);
      const auto t = parseSeconds(when), h = parseSeconds(hold);
      if (!key || !t || !h) return badValue(opt, v);
      events.push_back({toCycles(*t), order, [k = *key](Machine& m) { m.setKey(k, true); }});
      events.push_back(
          {toCycles(*t + *h), order + 1, [k = *key](Machine& m) { m.setKey(k, false); }});
    } else if (opt == "--note") {  // N:VEL@T:DUR
      std::string_view nv, td, n, vel, t, dur;
      if (!splitLast(v, '@', nv, td) || !splitLast(nv, ':', n, vel) ||
          !splitLast(td, ':', t, dur))
        return badValue(opt, v);
      const auto note = parseUint(n, 127, 10), velocity = parseUint(vel, 127, 10);
      const auto at = parseSeconds(t), len = parseSeconds(dur);
      if (!note || !velocity || !at || !len) return badValue(opt, v);
      const std::vector<uint8_t> on{0x90, uint8_t(*note), uint8_t(*velocity)};
      const std::vector<uint8_t> off{0x80, uint8_t(*note), 0x00};
      events.push_back({toCycles(*at), order, [on](Machine& m) { m.queueMidi(on); }});
      events.push_back(
          {toCycles(*at + *len), order + 1, [off](Machine& m) { m.queueMidi(off); }});
    } else if (opt == "--midi-hex") {  // "90 3C 64"@T
      std::string_view hex, t;
      if (!splitLast(v, '@', hex, t)) return badValue(opt, v);
      const std::vector<uint8_t> bytes = parseHexBytes(hex);
      const auto at = parseSeconds(t);
      if (bytes.empty() || !at) return badValue(opt, v);
      events.push_back({toCycles(*at), order, [bytes](Machine& m) { m.queueMidi(bytes); }});
    } else if (opt == "--wav" || opt == "--rate" || opt == "--snapshot-out" ||
               opt == "--snapshot-in") {
      unavailable.emplace_back(opt);
    } else {
      return usage();
    }
  }
  if (!unavailable.empty()) {
    for (const auto& o : unavailable)
      std::fprintf(stderr, "%s: not available yet (%s)\n", o.c_str(),
                   o.rfind("--snapshot", 0) == 0 ? "Task 13" : "Task 12");
    return 1;
  }

  const std::vector<uint8_t> rom = readAll(romPath);
  if (rom.size() != Machine::kBootRomSize) {
    std::fprintf(stderr, "%s: cannot read a 4096-byte boot ROM\n", romPath.c_str());
    return 1;
  }
  auto m = std::make_unique<Machine>(rom);  // large: keep it off the stack
  if (!diskPath.empty()) {
    try {
      m->insertDisk(DiskImage::loadFile(diskPath));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s: %s\n", diskPath.c_str(), e.what());
      return 1;
    }
  }
  m->powerOn();

  std::ofstream trace;
  if (!tracePath.empty()) {
    trace.open(tracePath, std::ios::binary);
    if (!trace) {
      std::fprintf(stderr, "cannot write %s\n", tracePath.c_str());
      return 1;
    }
    events.push_back({toCycles(traceFrom), -1, [&trace](Machine& mm) {
                        mm.setTrace([&trace, &mm](uint16_t pc, const Regs& r) {
                          traceLine(trace, mm, pc, r);
                        });
                      }});
  }

  std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
    return a.cycle != b.cycle ? a.cycle < b.cycle : a.order < b.order;
  });
  // Run in slices up to each event. `now` counts requested cycles: runCycles
  // credits its overshoot against the next request, so it must not be
  // re-derived from cycles().
  const uint64_t end = toCycles(seconds);
  uint64_t now = 0;
  for (const Event& e : events) {
    if (e.cycle > end) break;  // after the end of the run
    if (e.cycle > now) {
      m->runCycles(e.cycle - now);
      now = e.cycle;
    }
    e.act(*m);
  }
  if (end > now) m->runCycles(end - now);
  m->setTrace(nullptr);

  const DisplayState d = m->display();
  std::printf("display: %02X %02X\n", d.left, d.right);
  std::printf("illegal: %llu\n", static_cast<unsigned long long>(m->cpu().illegalCount()));
  std::printf("sectors: %llu\n", static_cast<unsigned long long>(m->fdc().sectorsRead()));
  std::printf("cycles: %llu\n", static_cast<unsigned long long>(m->cycles()));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "disasm") return disasm(argc - 2, argv + 2);
  if (argc >= 2 && std::string_view(argv[1]) == "run") return run(argc - 2, argv + 2);
  return usage();
}
