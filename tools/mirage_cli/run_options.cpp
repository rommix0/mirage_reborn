#include "run_options.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <optional>
#include <ostream>
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

// "FILE@T[,rate=R][,start=S][,end=E][,half=lower|upper][,page=P][,normalize=0|1]".
// The file name cannot contain a comma.
std::optional<RunEvent> parseImport(std::string_view v) {
  RunEvent e;
  e.kind = RunEvent::Kind::Import;
  const size_t comma = v.find(',');
  std::string_view head = v.substr(0, comma), rest;
  if (comma != std::string_view::npos) rest = v.substr(comma + 1);
  std::string_view file, when;
  if (!splitLast(head, '@', file, when) || file.empty()) return std::nullopt;
  const auto t = parseSeconds(when);
  if (!t) return std::nullopt;
  e.file = file;
  e.cycle = toCycles(*t);
  while (comma != std::string_view::npos) {
    const size_t next = rest.find(',');
    const std::string_view field = rest.substr(0, next);
    const size_t eq = field.find('=');
    if (eq == std::string_view::npos) return std::nullopt;
    const std::string_view key = field.substr(0, eq), val = field.substr(eq + 1);
    if (key == "rate") {
      const auto r = parseSeconds(val);
      if (!r) return std::nullopt;
      e.import.rate = *r;
    } else if (key == "start") {
      const auto s = parseSeconds(val);
      if (!s) return std::nullopt;
      e.import.startSeconds = *s;
    } else if (key == "end") {
      const auto s = parseSeconds(val);
      if (!s) return std::nullopt;
      e.import.endSeconds = *s;
    } else if (key == "half") {
      if (val == "lower") e.import.half = WaveHalf::Lower;
      else if (val == "upper") e.import.half = WaveHalf::Upper;
      else return std::nullopt;
    } else if (key == "page") {
      const auto p = parseUint(val, 255, 10);
      if (!p) return std::nullopt;
      e.import.startPage = int(*p);
    } else if (key == "normalize") {
      if (val != "0" && val != "1") return std::nullopt;
      e.import.normalize = val == "1";
    } else {
      return std::nullopt;
    }
    if (next == std::string_view::npos) break;
    rest = rest.substr(next + 1);
  }
  return e;
}

// "FILE@T[,gain=G][,loop=0|1]". The file name cannot contain a comma.
std::optional<RunEvent> parseAudioIn(std::string_view v) {
  RunEvent e;
  e.kind = RunEvent::Kind::AudioIn;
  const size_t comma = v.find(',');
  std::string_view head = v.substr(0, comma), rest;
  if (comma != std::string_view::npos) rest = v.substr(comma + 1);
  std::string_view file, when;
  if (!splitLast(head, '@', file, when) || file.empty()) return std::nullopt;
  const auto t = parseSeconds(when);
  if (!t) return std::nullopt;
  e.file = file;
  e.cycle = toCycles(*t);
  while (comma != std::string_view::npos) {
    const size_t next = rest.find(',');
    const std::string_view field = rest.substr(0, next);
    const size_t eq = field.find('=');
    if (eq == std::string_view::npos) return std::nullopt;
    const std::string_view key = field.substr(0, eq), val = field.substr(eq + 1);
    if (key == "gain") {
      const auto g = parseSeconds(val);  // a finite number >= 0
      if (!g) return std::nullopt;
      e.gain = *g;
    } else if (key == "loop") {
      if (val != "0" && val != "1") return std::nullopt;
      e.loop = val == "1";
    } else {
      return std::nullopt;
    }
    if (next == std::string_view::npos) break;
    rest = rest.substr(next + 1);
  }
  return e;
}

struct Hold {
  uint64_t from, to;
};

int rank(RunEvent::Kind k) {
  switch (k) {
    case RunEvent::Kind::TraceOn: return 0;
    case RunEvent::Kind::KeyDown:
    case RunEvent::Kind::KeyUp:
    case RunEvent::Kind::Import:
    case RunEvent::Kind::AudioIn: return 1;
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
  std::vector<RunEvent> midi, imports, audioIns;

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
    } else if (opt == "--import") {
      auto e = parseImport(v);
      if (!e) return bad(opt, v);
      imports.push_back(std::move(*e));
    } else if (opt == "--audio-in") {
      auto e = parseAudioIn(v);
      if (!e) return bad(opt, v);
      audioIns.push_back(std::move(*e));
    } else if (opt == "--wav") {
      if (v.empty()) return bad(opt, v);
      o.wav = v;
    } else if (opt == "--rate") {
      const auto r = parseUint(v, 768000, 10);
      if (!r || *r < 1000) return bad(opt, v);
      o.rate = int(*r);
    } else if (opt == "--snapshot-out") {
      if (v.empty()) return bad(opt, v);
      o.snapshotOut = v;
    } else if (opt == "--save-disk") {
      if (v.empty()) return bad(opt, v);
      o.saveDisk = v;
    } else if (opt == "--snapshot-in") {
      if (v.empty()) return bad(opt, v);
      o.snapshotIn = v;
    } else {
      return RunArgsError{2, ""};
    }
  }
  if (!o.snapshotIn.empty() && !o.disk.empty())
    return RunArgsError{
        2, "--disk cannot be combined with --snapshot-in: the snapshot holds its own disk"};

  o.endCycle = toCycles(seconds);
  if (!o.trace.empty()) o.events.push_back({toCycles(traceFrom), RunEvent::Kind::TraceOn, {}, {}});
  addKeyEvents(holds, o.events);
  o.events.insert(o.events.end(), std::make_move_iterator(imports.begin()),
                  std::make_move_iterator(imports.end()));
  o.events.insert(o.events.end(), std::make_move_iterator(audioIns.begin()),
                  std::make_move_iterator(audioIns.end()));
  o.events.insert(o.events.end(), midi.begin(), midi.end());
  // Stable: MIDI events keep command-line order at equal times, and a key's
  // KeyDown stays before its KeyUp when a hold has zero length.
  std::stable_sort(o.events.begin(), o.events.end(), [](const RunEvent& a, const RunEvent& b) {
    return a.cycle != b.cycle ? a.cycle < b.cycle : rank(a.kind) < rank(b.kind);
  });
  std::erase_if(o.events, [&o](const RunEvent& e) { return e.cycle > o.endCycle; });
  return o;
}

std::string prepareImports(RunOptions& o) {
  for (RunEvent& e : o.events) {
    if (e.kind != RunEvent::Kind::Import) continue;
    try {
      const WavAudio audio = readWavFile(e.file);
      e.plan = planImport(audio, e.import);
      if (!e.plan.problem.empty()) return e.file + ": " + e.plan.problem;
      e.wave = convertForWaveRam(audio, e.import);
    } catch (const WavError& err) {
      return e.file + ": " + err.what();
    }
  }
  return {};
}

std::string prepareAudioIn(RunOptions& o) {
  for (RunEvent& e : o.events) {
    if (e.kind != RunEvent::Kind::AudioIn) continue;
    try {
      const WavAudio audio = readWavFile(e.file);
      const std::vector<float>& src = audio.mono;
      const double step = audio.rate / double(o.rate);  // source frames per output frame
      const size_t n = size_t(double(src.size()) / step + 0.5);
      e.samples.resize(n);
      for (size_t j = 0; j < n; ++j) {
        const double at = double(j) * step;
        const size_t i = std::min(size_t(at), src.size() - 1);
        const double f = at - double(i);
        const float a = src[i], b = src[std::min(i + 1, src.size() - 1)];
        e.samples[j] = float(double(e.gain) * (double(a) + f * (double(b) - double(a))));
      }
    } catch (const WavError& err) {
      return e.file + ": " + err.what();
    }
  }
  return {};
}

void runSchedule(Machine& m, const RunOptions& o,
                 const std::function<void(uint16_t pc, const Regs&)>& trace,
                 std::vector<float>* audio, std::ostream* log) {
  // `now` counts requested cycles: runCycles credits its overshoot against the
  // next request, so the slice must not be re-derived from cycles().
  uint64_t now = 0;
  // Audio: the slice to a cycle count is the whole frames up to the nearest
  // frame boundary; framesDone counts the frames rendered so far.
  uint64_t framesDone = 0;
  const auto framesAt = [&o](uint64_t cycle) {
    return uint64_t(double(cycle) * o.rate / kCpuClockHz + 0.5);
  };
  // The input jack: the AudioIn event now playing (index into o.events; -1 =
  // none yet, so nothing is plugged in), the next sample of its file, and the
  // frames each entry has fed to the jack.
  const bool hasInput = std::any_of(o.events.begin(), o.events.end(), [](const RunEvent& e) {
    return e.kind == RunEvent::Kind::AudioIn;
  });
  std::vector<uint64_t> played(o.events.size(), 0);
  ptrdiff_t active = -1;
  size_t pos = 0;
  std::vector<float> discard;  // the output when only the input matters
  const auto advanceTo = [&](uint64_t cycle) {
    if (!audio && !hasInput) {
      if (cycle > now) m.runCycles(cycle - now);
      now = std::max(now, cycle);
      return;
    }
    const uint64_t target = framesAt(cycle);
    // Without --wav the input is still rendered; the output is thrown away in
    // chunks so a long run does not grow a buffer.
    constexpr size_t kChunk = 1 << 16;
    while (target > framesDone) {
      size_t n = size_t(target - framesDone);
      float* out;
      if (audio) {
        const size_t at = audio->size();
        audio->resize(at + n);
        out = audio->data() + at;
      } else {
        n = std::min(n, kChunk);
        if (discard.size() < n) discard.resize(n);
        out = discard.data();
      }
      if (active < 0) {
        m.render(out, n, o.rate);
      } else {
        // The input goes in the output buffer (render reads it before it writes).
        const RunEvent& e = o.events[size_t(active)];
        for (size_t i = 0; i < n; ++i) {
          if (pos >= e.samples.size() && e.loop) pos = 0;
          if (pos < e.samples.size()) {
            out[i] = e.samples[pos++];
            ++played[size_t(active)];
          } else {
            out[i] = 0.0f;  // the file has ended: the jack stays plugged in, silent
          }
        }
        m.render(out, out, n, o.rate);
      }
      framesDone += n;
    }
  };
  for (size_t idx = 0; idx < o.events.size(); ++idx) {
    const RunEvent& e = o.events[idx];
    advanceTo(e.cycle);
    switch (e.kind) {
      case RunEvent::Kind::TraceOn: m.setTrace(trace); break;
      case RunEvent::Kind::KeyDown: m.setKey(e.key, true); break;
      case RunEvent::Kind::KeyUp: m.setKey(e.key, false); break;
      case RunEvent::Kind::Midi: m.queueMidi(e.midi); break;
      case RunEvent::Kind::AudioIn:
        active = ptrdiff_t(idx);
        pos = 0;
        break;
      case RunEvent::Kind::Import:
        if (m.driveActive()) {
          if (log) *log << "import: " << e.file << " refused, the drive is busy\n";
          break;
        }
        m.writeWaveRam(e.plan.address, e.wave);
        if (log) {
          char at[16];
          std::snprintf(at, sizeof at, "$%05X", unsigned(e.plan.address));
          *log << "import: " << e.file << ' ' << e.plan.bytes << " bytes at " << at
               << ", last page " << e.plan.lastPage << (e.plan.truncated ? ", truncated" : "")
               << '\n';
        }
        break;
    }
  }
  advanceTo(o.endCycle);
  m.setTrace(nullptr);
  if (log)
    for (size_t idx = 0; idx < o.events.size(); ++idx)
      if (o.events[idx].kind == RunEvent::Kind::AudioIn)
        *log << "audio-in: " << o.events[idx].file << ' ' << played[idx] << " frames played\n";
}

}  // namespace mirage::cli
