#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "mirage/panel.h"

namespace mirage {

// The replica front panel in panel units (the later grey DSK-8), y down, origin top-left.
// The numbers live in one table in panel_layout.cpp so they can be refined in one place.
struct PanelRect {
  float x, y, w, h;
};

enum class CapColour : uint8_t { Yellow, Grey, White, Black };

struct PanelButton {
  PanelKey key;
  std::string_view label;  // UTF-8, printed above the button
  CapColour cap;
  PanelRect r;
};
// 24 entries, one per PanelKey, indexed by the key's enumerator value.
std::span<const PanelButton> panelButtons();

struct PanelHeading {
  std::string_view text;
  float centreX;
};
// VOLUME, CONTROL, SELECT, SEQ, SAMPLE, LOAD, drawn at PanelGeometry::headingY.
std::span<const PanelHeading> panelHeadings();

struct PanelGeometry {
  float width, height;
  PanelRect display, volume, driveSlot, driveLed, ensoniqBadge, mirageLogo;
  float headingY;  // baseline row of the section headings
  float lipY;      // top of the front-lip strip (drive slot and LED)
};
const PanelGeometry& panelGeometry();

// Keyboard shortcuts for the window.
enum class ShortcutKey : uint8_t { Up, Down, Return, Escape };
std::optional<PanelKey> panelKeyForChar(char32_t c);         // '0'..'9' -> K0..K9
std::optional<PanelKey> panelKeyForShortcut(ShortcutKey k);  // Up, Down, Enter, Cancel

}  // namespace mirage
