#include "run_options.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include "mirage/clocks.h"
#include "mirage/machine.h"

namespace mirage::cli {

namespace {

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
  if (t.empty() || t[0] == '-' || t[0] == '+') return std::nullopt;
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

// "90 3C 64" (spaces or commas between bytes) -> bytes; empty on error.
std::vector<uint8_t> parseHexBytes(std::string_view hex) {
  std::vector<uint8_t> bytes;
  const auto sep = [](char c) { return c == ' ' || c == ','; };
  size_t p = 0;
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

RunArgsError bad(std::string_view opt, std::string_view v) {
  return {2, "bad value for " + std::string(opt) + ": " + std::string(v)};
}

struct Hold {
  uint64_t from, to;
};

int rank(RunEvent::Kind k) {
  switch (k) {
    case RunEvent::Kind::TraceOn: return 0;
    case RunEvent::Kind::KeyDown:
    case RunEvent::Kind::KeyUp: return 1;
    case RunEvent::Kind::Midi: return 2;
  }
  return 3;
}

// Merges each key's holds (overlapping or touching intervals become one) and
// emits one KeyDown/KeyUp pair per merged interval.
void addKeyEvents(const std::map<PanelKey, std::vector<Hold>>& holds,
                  std::vector<RunEvent>& out) {
  for (auto [key, list] : holds) {
    std::sort(list.begin(), list.end(),
              [](const Hold& a, const Hold& b) { return a.from < b.from; });
    for (size_t i = 0; i < list.size();) {
      Hold merged = list[i++];
      while (i < list.size() && list[i].from <= merged.to)
        merged.to = std::max(merged.to, list[i++].to);
      out.push_back({merged.from, RunEvent::Kind::KeyDown, key, {}});
      out.push_back({merged.to, RunEvent::Kind::KeyUp, key, {}});
    }
  }
}

}  // namespace

std::variant<RunOptions, RunArgsError> parseRunArgs(const std::vector<std::string>& args,
                                                    const std::string& defaultRom) {
  RunOptions o;
  o.rom = defaultRom;
  double seconds = 10, traceFrom = 0;
  std::map<PanelKey, std::vector<Hold>> holds;
  std::vector<RunEvent> midi;
  std::string unavailable;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string_view opt = args[i];
    if (i + 1 >= args.size()) return RunArgsError{2, ""};
    const std::string_view v = args[++i];
    if (opt == "--disk") {
      o.disk = v;
    } else if (opt == "--rom") {
      o.rom = v;
    } else if (opt == "--seconds") {
      const auto s = parseSeconds(v);
      if (!s) return bad(opt, v);
      seconds = *s;
    } else if (opt == "--trace") {
      o.trace = v;
    } else if (opt == "--trace-from") {
      const auto s = parseSeconds(v);
      if (!s) return bad(opt, v);
      traceFrom = *s;
    } else if (opt == "--key") {  // NAME@T[:HOLD]
      std::string_view name, when, hold = "0.1";
      if (!splitLast(v, '@', name, when)) return bad(opt, v);
      if (const size_t colon = when.find(':'); colon != std::string_view::npos) {
        hold = when.substr(colon + 1);
        when = when.substr(0, colon);
      }
      const auto key = panelKeyFromName(name);
      const auto t = parseSeconds(when), h = parseSeconds(hold);
      if (!key || !t || !h) return bad(opt, v);
      holds[*key].push_back({toCycles(*t), toCycles(*t + *h)});
    } else if (opt == "--note") {  // N:VEL@T:DUR
      std::string_view nv, td, n, vel, t, dur;
      if (!splitLast(v, '@', nv, td) || !splitLast(nv, ':', n, vel) ||
          !splitLast(td, ':', t, dur))
        return bad(opt, v);
      const auto note = parseUint(n, 127, 10), velocity = parseUint(vel, 127, 10);
      const auto at = parseSeconds(t), len = parseSeconds(dur);
      if (!note || !velocity || !at || !len) return bad(opt, v);
      midi.push_back({toCycles(*at), RunEvent::Kind::Midi, {},
                      {0x90, uint8_t(*note), uint8_t(*velocity)}});
      midi.push_back({toCycles(*at + *len), RunEvent::Kind::Midi, {},
                      {0x80, uint8_t(*note), 0x00}});
    } else if (opt == "--midi-hex") {  // "90 3C 64"@T
      std::string_view hex, t;
      if (!splitLast(v, '@', hex, t)) return bad(opt, v);
      std::vector<uint8_t> bytes = parseHexBytes(hex);
      const auto at = parseSeconds(t);
      if (bytes.empty() || !at) return bad(opt, v);
      midi.push_back({toCycles(*at), RunEvent::Kind::Midi, {}, std::move(bytes)});
    } else if (opt == "--wav") {
      if (v.empty()) return bad(opt, v);
      o.wav = v;
    } else if (opt == "--rate") {
      const auto r = parseUint(v, 768000, 10);
      if (!r || *r < 1000) return bad(opt, v);
      o.rate = int(*r);
    } else if (opt == "--snapshot-out" || opt == "--snapshot-in") {
      unavailable += std::string(opt) + ": not available yet (Task 13)\n";
    } else {
      return RunArgsError{2, ""};
    }
  }
  if (!unavailable.empty()) return RunArgsError{1, unavailable};

  o.endCycle = toCycles(seconds);
  if (!o.trace.empty()) o.events.push_back({toCycles(traceFrom), RunEvent::Kind::TraceOn, {}, {}});
  addKeyEvents(holds, o.events);
  o.events.insert(o.events.end(), midi.begin(), midi.end());
  // Stable: MIDI events keep command-line order at equal times, and a key's
  // KeyDown stays before its KeyUp when a hold has zero length.
  std::stable_sort(o.events.begin(), o.events.end(), [](const RunEvent& a, const RunEvent& b) {
    return a.cycle != b.cycle ? a.cycle < b.cycle : rank(a.kind) < rank(b.kind);
  });
  std::erase_if(o.events, [&o](const RunEvent& e) { return e.cycle > o.endCycle; });
  return o;
}

void runSchedule(Machine& m, const RunOptions& o,
                 const std::function<void(uint16_t pc, const Regs&)>& trace,
                 std::vector<float>* audio) {
  // `now` counts requested cycles: runCycles credits its overshoot against the
  // next request, so the slice must not be re-derived from cycles().
  uint64_t now = 0;
  // Audio: the slice to a cycle count is the whole frames up to the nearest
  // frame boundary; framesDone counts the frames rendered so far.
  uint64_t framesDone = 0;
  const auto framesAt = [&o](uint64_t cycle) {
    return uint64_t(double(cycle) * o.rate / kCpuClockHz + 0.5);
  };
  const auto advanceTo = [&](uint64_t cycle) {
    if (!audio) {
      if (cycle > now) m.runCycles(cycle - now);
      now = std::max(now, cycle);
      return;
    }
    const uint64_t target = framesAt(cycle);
    if (target > framesDone) {
      const size_t at = audio->size();
      audio->resize(at + size_t(target - framesDone));
      m.render(audio->data() + at, size_t(target - framesDone), o.rate);
      framesDone = target;
    }
  };
  for (const RunEvent& e : o.events) {
    advanceTo(e.cycle);
    switch (e.kind) {
      case RunEvent::Kind::TraceOn: m.setTrace(trace); break;
      case RunEvent::Kind::KeyDown: m.setKey(e.key, true); break;
      case RunEvent::Kind::KeyUp: m.setKey(e.key, false); break;
      case RunEvent::Kind::Midi: m.queueMidi(e.midi); break;
    }
  }
  advanceTo(o.endCycle);
  m.setTrace(nullptr);
}

}  // namespace mirage::cli
