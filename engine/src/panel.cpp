#include "mirage/panel.h"

#include <algorithm>
#include <cctype>

#include "mirage/clocks.h"

namespace mirage {

namespace {
// The multiplexed display looks lit for roughly this long after a segment was
// last driven; the ROM scans every segment well inside this (about 8.5 ms).
constexpr uint64_t kPersistCycles = uint64_t(kCpuClockHz) / 50;  // 20 ms

// Indexed by PanelKey (row*8 + column).
constexpr std::string_view kKeyNames[kPanelKeyCount] = {
    "LoadUpper", "LoadLower", "SampleUpper", "PlaySeq", "LoadSeq", "SaveSeq", "RecSeq",
    "SampleLower", "K3", "K6", "K9", "K5", "K8", "K0", "K2", "Enter",
    "K1", "K4", "K7", "Up", "Param", "Down", "Value", "Cancel"};
static_assert(uint8_t(PanelKey::Cancel) + 1 == kPanelKeyCount);
}  // namespace

std::string_view panelKeyName(PanelKey key) {
  const auto i = uint8_t(key);
  return i < kPanelKeyCount ? kKeyNames[i] : std::string_view{};
}

std::optional<PanelKey> panelKeyFromName(std::string_view name) {
  const auto same = [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  };
  for (int i = 0; i < kPanelKeyCount; ++i)
    if (std::ranges::equal(kKeyNames[i], name, same)) return PanelKey(i);
  return std::nullopt;
}

void Panel::setKey(PanelKey key, bool down) {
  const uint32_t bit = 1u << (uint8_t(key) % 24);
  if (down) keys_ |= bit;
  else keys_ &= ~bit;
}

uint8_t Panel::rowSense(uint8_t portA) const {
  const unsigned col = portA & 7u;
  uint8_t v = 0xE0;
  for (unsigned row = 0; row < 3; ++row)
    if (keys_ & (1u << (row * 8 + col))) v &= uint8_t(~(0x20u << row));
  return v;
}

void Panel::portAWritten(uint8_t portA, uint64_t cycle) {
  const unsigned seg = portA & 7u;
  if (!(portA & 0x08)) {  // PA3 low: left digit
    lastLit_[0][seg] = cycle;
    litValid_ |= uint16_t(1u << seg);
  }
  if (!(portA & 0x10)) {  // PA4 low: right digit
    lastLit_[1][seg] = cycle;
    litValid_ |= uint16_t(1u << (8 + seg));
  }
}

DisplayState Panel::display(uint64_t nowCycle) const {
  uint8_t out[2] = {0, 0};
  for (unsigned d = 0; d < 2; ++d)
    for (unsigned s = 0; s < 8; ++s) {
      if (!(litValid_ & (1u << (d * 8 + s)))) continue;
      const uint64_t last = lastLit_[d][s];
      if (nowCycle >= last && nowCycle - last < kPersistCycles) out[d] |= uint8_t(1u << s);
    }
  return {out[0], out[1]};
}

void Panel::clearDisplay() {
  lastLit_ = {};
  litValid_ = 0;
}

void Panel::save(StateWriter& w) const {
  w.tag(fourcc("PNEL"));
  w.u32(keys_);
  w.u16(litValid_);
  for (const auto& digit : lastLit_)
    for (uint64_t c : digit) w.u64(c);
}

void Panel::load(StateReader& r) {
  r.tag(fourcc("PNEL"));
  const uint32_t keys = r.u32();
  const uint16_t valid = r.u16();
  std::array<std::array<uint64_t, 8>, 2> lit{};
  for (auto& digit : lit)
    for (uint64_t& c : digit) c = r.u64();
  if (keys & ~kKeyMask) throw StateError("panel: bad key state");
  keys_ = keys;
  litValid_ = valid;
  lastLit_ = lit;
}

}  // namespace mirage
