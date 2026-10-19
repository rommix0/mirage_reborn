#include "mirage/panel_layout.h"

#include <array>

namespace mirage {

namespace {

// Geometry from the plan's Panel layout section (1000 x 300 panel units), refined against
// the rendered panel (Mirage --panel-png).
constexpr float kButtonW = 40, kButtonH = 20;

// Column centre x.
constexpr float kColControl0 = 210, kColControl1 = 262;
constexpr float kColSelect2 = 330, kColSelect3 = 382, kColSelect4 = 434;
constexpr float kColSeq = 500, kColSample = 552, kColLoad = 604;
constexpr float kColSlider = 150;

// Row centre y (R1 top).
constexpr float kRow1 = 62, kRow2 = 104, kRow3 = 146, kRow4 = 188;

constexpr PanelRect btn(float cx, float cy) {
  return {cx - kButtonW / 2, cy - kButtonH / 2, kButtonW, kButtonH};
}

// In PanelKey enumerator order, so panelButtons()[uint8_t(key)] is that key's button.
constexpr std::array<PanelButton, kPanelKeyCount> kButtons = {{
    {PanelKey::LoadUpper, "Upper", CapColour::White, btn(kColLoad, kRow1)},
    {PanelKey::LoadLower, "Lower", CapColour::White, btn(kColLoad, kRow2)},
    {PanelKey::SampleUpper, "Upper", CapColour::Black, btn(kColSample, kRow1)},
    {PanelKey::PlaySeq, "Play", CapColour::Grey, btn(kColSeq, kRow2)},
    {PanelKey::LoadSeq, "Load", CapColour::Grey, btn(kColSeq, kRow3)},
    {PanelKey::SaveSeq, "Save", CapColour::Grey, btn(kColSeq, kRow4)},
    {PanelKey::RecSeq, "Rec", CapColour::Grey, btn(kColSeq, kRow1)},
    {PanelKey::SampleLower, "Lower", CapColour::Black, btn(kColSample, kRow2)},
    {PanelKey::K3, "3", CapColour::Yellow, btn(kColSelect4, kRow1)},
    {PanelKey::K6, "6", CapColour::Grey, btn(kColSelect4, kRow2)},
    {PanelKey::K9, "9", CapColour::Grey, btn(kColSelect4, kRow3)},
    {PanelKey::K5, "5", CapColour::Grey, btn(kColSelect3, kRow2)},
    {PanelKey::K8, "8", CapColour::Grey, btn(kColSelect3, kRow3)},
    {PanelKey::K0, "0/Prog", CapColour::Yellow, btn(kColSelect3, kRow4)},
    {PanelKey::K2, "2", CapColour::Yellow, btn(kColSelect3, kRow1)},
    {PanelKey::Enter, "Enter", CapColour::White, btn(kColSelect4, kRow4)},
    {PanelKey::K1, "1", CapColour::Yellow, btn(kColSelect2, kRow1)},
    {PanelKey::K4, "4", CapColour::Yellow, btn(kColSelect2, kRow2)},
    {PanelKey::K7, "7", CapColour::Grey, btn(kColSelect2, kRow3)},
    {PanelKey::Up, "On/▲", CapColour::Grey, btn(kColControl1, kRow4)},
    {PanelKey::Param, "Param", CapColour::Grey, btn(kColControl0, kRow3)},
    {PanelKey::Down, "Off/▼", CapColour::Grey, btn(kColControl0, kRow4)},
    {PanelKey::Value, "Value", CapColour::Grey, btn(kColControl1, kRow3)},
    {PanelKey::Cancel, "Cancel", CapColour::Grey, btn(kColSelect2, kRow4)},
}};

constexpr std::array<PanelHeading, 6> kHeadings = {{
    {"VOLUME", kColSlider},
    {"CONTROL", (kColControl0 + kColControl1) / 2},
    {"SELECT", kColSelect3},
    {"SEQ", kColSeq},
    {"SAMPLE", kColSample},
    {"LOAD", kColLoad},
}};

constexpr PanelGeometry kGeometry = {
    /*width*/ 1000, /*height*/ 300,
    /*display*/ {190, 34, 100, 58},
    /*volume*/ {kColSlider - 16, 40, 32, 156},  // ticks at its left, the fader slot down its centre
    /*driveSlot*/ {140, 250, 280, 20},
    /*driveLed*/ {436, 256, 8, 8},  // centred on (440, 260)
    /*ensoniqBadge*/ {20, 148, 96, 26},  // centred on the pinstripe
    /*mirageLogo*/ {680, 50, 280, 80},
    /*mirageSubtitle*/ {680, 136, 280, 12},
    /*rowRuleY*/ {(kRow1 + kRow2) / 2 - 6, (kRow2 + kRow3) / 2 - 6},  // above the next row's labels
    /*pinstripeY*/ (kRow3 + kRow4) / 2 - 6,
    /*headingRuleY*/ 204,
    /*headingY*/ 214,
    /*lipY*/ 230,
};

}  // namespace

std::span<const PanelButton> panelButtons() { return kButtons; }
std::span<const PanelHeading> panelHeadings() { return kHeadings; }
const PanelGeometry& panelGeometry() { return kGeometry; }

std::optional<PanelKey> panelKeyForChar(char32_t c) {
  static constexpr PanelKey digits[10] = {PanelKey::K0, PanelKey::K1, PanelKey::K2, PanelKey::K3,
                                          PanelKey::K4, PanelKey::K5, PanelKey::K6, PanelKey::K7,
                                          PanelKey::K8, PanelKey::K9};
  if (c >= U'0' && c <= U'9') return digits[c - U'0'];
  return std::nullopt;
}

std::optional<PanelKey> panelKeyForShortcut(ShortcutKey k) {
  switch (k) {
    case ShortcutKey::Up: return PanelKey::Up;
    case ShortcutKey::Down: return PanelKey::Down;
    case ShortcutKey::Return: return PanelKey::Enter;
    case ShortcutKey::Escape: return PanelKey::Cancel;
  }
  return std::nullopt;
}

}  // namespace mirage
