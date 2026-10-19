#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "mirage/state_io.h"

namespace mirage {

// Value = row*8 + column of the keypad matrix.
enum class PanelKey : uint8_t {
  LoadUpper, LoadLower, SampleUpper, PlaySeq, LoadSeq, SaveSeq, RecSeq, SampleLower,
  K3, K6, K9, K5, K8, K0, K2, Enter,
  K1, K4, K7, Up, Param, Down, Value, Cancel
};
inline constexpr int kPanelKeyCount = 24;

// The enumerator's name ("LoadUpper", "K3", "Enter", ...); empty if out of range.
std::string_view panelKeyName(PanelKey key);
// Inverse of panelKeyName, ignoring ASCII case.
std::optional<PanelKey> panelKeyFromName(std::string_view name);

struct DisplayState {
  uint8_t left = 0, right = 0;  // bit n = segment n (a..g, dp)
};

// Keypad matrix and the two multiplexed 7-segment digits, wired to VIA port A:
// PA0-2 column / segment select, PA3 left-digit enable, PA4 right-digit enable
// (both active low), PA5-7 sense key rows 0-2 (active low).
class Panel {
 public:
  void setKey(PanelKey key, bool down);
  bool keyDown(PanelKey key) const { return (keys_ >> (uint8_t(key) % 24)) & 1u; }
  // Row sense levels in bits 5..7 (bit 5+row; 0 = a key is down in that row in
  // the column selected by portA & 7). Other bits are 0.
  uint8_t rowSense(uint8_t portA) const;
  // Records the segment lit on each enabled digit; `cycle` is the E-cycle count.
  void portAWritten(uint8_t portA, uint64_t cycle);
  // Segments written within the persistence window (35 ms, panel.cpp).
  DisplayState display(uint64_t nowCycle) const;
  void clearDisplay();  // forgets the lit history; held keys are kept

  void save(StateWriter&) const;
  void load(StateReader&);  // commits only on success

 private:
  static constexpr uint32_t kKeyMask = 0x00FFFFFF;
  uint32_t keys_ = 0;
  // [digit][segment]: cycle of the last write that lit it, valid if litValid_.
  std::array<std::array<uint64_t, 8>, 2> lastLit_{};
  uint16_t litValid_ = 0;  // bit digit*8+segment
};

}  // namespace mirage
