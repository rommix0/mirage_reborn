// Replica panel layout and keyboard shortcuts (host/include/mirage/panel_layout.h),
// and the known boot ROM hash (host/include/mirage/boot_rom.h).
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <initializer_list>
#include <set>

#include "asset_paths.h"
#include "booted_machine.h"
#include "mirage/boot_rom.h"
#include "mirage/machine.h"
#include "mirage/panel_layout.h"

using namespace mirage;

namespace {
bool overlaps(const PanelRect& a, const PanelRect& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
bool inside(const PanelRect& r, float w, float h) {
  return r.x >= 0 && r.y >= 0 && r.x + r.w <= w && r.y + r.h <= h;
}
float centreX(const PanelRect& r) { return r.x + r.w / 2; }
float centreY(const PanelRect& r) { return r.y + r.h / 2; }
float right(const PanelRect& r) { return r.x + r.w; }
float bottom(const PanelRect& r) { return r.y + r.h; }
const PanelButton& findButton(PanelKey k) {
  for (const PanelButton& b : panelButtons())
    if (b.key == k) return b;
  FAIL("missing key");
  return panelButtons()[0];
}
const PanelRect& rectOf(PanelKey k) { return findButton(k).r; }
// The keys share one row and run left to right without overlapping.
void requireRow(std::initializer_list<PanelKey> keys) {
  const PanelRect* prev = nullptr;
  for (PanelKey k : keys) {
    const PanelRect& r = rectOf(k);
    if (prev != nullptr) {
      REQUIRE(centreY(r) == centreY(*prev));
      REQUIRE(r.x >= right(*prev));
    }
    prev = &r;
  }
}
// The keys share one column and run top to bottom without overlapping.
void requireColumn(std::initializer_list<PanelKey> keys) {
  const PanelRect* prev = nullptr;
  for (PanelKey k : keys) {
    const PanelRect& r = rectOf(k);
    if (prev != nullptr) {
      REQUIRE(centreX(r) == centreX(*prev));
      REQUIRE(r.y >= bottom(*prev));
    }
    prev = &r;
  }
}
}  // namespace

TEST_CASE("panel layout has each of the 24 keys exactly once, labelled and coloured as printed",
          "[panel_layout]") {
  const auto buttons = panelButtons();
  REQUIRE(buttons.size() == 24);
  std::set<PanelKey> seen;
  for (const PanelButton& b : buttons) seen.insert(b.key);
  REQUIRE(seen.size() == 24);
  for (size_t i = 0; i < buttons.size(); ++i) REQUIRE(size_t(buttons[i].key) == i);  // enum order

  // The plan's Panel layout table (the later grey DSK-8). The arrows are UTF-8.
  struct Printed {
    PanelKey key;
    std::string_view label;
    CapColour cap;
  };
  using C = CapColour;
  const Printed table[] = {
      {PanelKey::LoadUpper, "Upper", C::White},
      {PanelKey::LoadLower, "Lower", C::White},
      {PanelKey::SampleUpper, "Upper", C::Black},
      {PanelKey::SampleLower, "Lower", C::Black},
      {PanelKey::RecSeq, "Rec", C::Grey},
      {PanelKey::PlaySeq, "Play", C::Grey},
      {PanelKey::LoadSeq, "Load", C::Grey},
      {PanelKey::SaveSeq, "Save", C::Grey},
      {PanelKey::K1, "1", C::Yellow},
      {PanelKey::K2, "2", C::Yellow},
      {PanelKey::K3, "3", C::Yellow},
      {PanelKey::K4, "4", C::Yellow},
      {PanelKey::K5, "5", C::Grey},
      {PanelKey::K6, "6", C::Grey},
      {PanelKey::K7, "7", C::Grey},
      {PanelKey::K8, "8", C::Grey},
      {PanelKey::K9, "9", C::Grey},
      {PanelKey::Cancel, "Cancel", C::Grey},
      {PanelKey::K0, "0/Prog", C::Yellow},
      {PanelKey::Enter, "Enter", C::White},
      {PanelKey::Param, "Param", C::Grey},
      {PanelKey::Value, "Value", C::Grey},
      {PanelKey::Down, "Off/\xE2\x96\xBC", C::Grey},  // Off/▼
      {PanelKey::Up, "On/\xE2\x96\xB2", C::Grey},     // On/▲
  };
  for (const Printed& p : table) {
    INFO(panelKeyName(p.key));
    REQUIRE(findButton(p.key).label == p.label);
    REQUIRE(findButton(p.key).cap == p.cap);
  }

  const auto headings = panelHeadings();
  REQUIRE(headings.size() == 6);
  const char* names[] = {"VOLUME", "CONTROL", "SELECT", "SEQ", "SAMPLE", "LOAD"};
  for (size_t i = 0; i < 6; ++i) REQUIRE(headings[i].text == names[i]);
}

TEST_CASE("panel buttons lie inside the panel and don't overlap each other or the display",
          "[panel_layout]") {
  const PanelGeometry& g = panelGeometry();
  REQUIRE(g.width == 1000);
  REQUIRE(g.height == 300);
  const auto buttons = panelButtons();
  for (size_t i = 0; i < buttons.size(); ++i) {
    REQUIRE(inside(buttons[i].r, g.width, g.lipY));  // the top strip
    REQUIRE_FALSE(overlaps(buttons[i].r, g.display));
    REQUIRE_FALSE(overlaps(buttons[i].r, g.volume));
    for (size_t j = i + 1; j < buttons.size(); ++j)
      REQUIRE_FALSE(overlaps(buttons[i].r, buttons[j].r));
  }
  for (const PanelRect& r : {g.display, g.volume, g.driveSlot, g.driveLed, g.ensoniqBadge,
                             g.mirageLogo, g.mirageSubtitle, g.redStripe})
    REQUIRE(inside(r, g.width, g.height));
  for (const PanelHeading& h : panelHeadings()) {
    REQUIRE(h.centreX > 0);
    REQUIRE(h.centreX < g.width);
  }
  REQUIRE(g.headingY < g.lipY);
  REQUIRE(g.lipY < g.height);
}

TEST_CASE("panel layout follows the DSK-8 front panel", "[panel_layout]") {
  const PanelGeometry& g = panelGeometry();
  using K = PanelKey;

  // Rows R1..R4 left to right, and the columns top to bottom. SELECT is the
  // 3 x 4 keypad; SEQ, SAMPLE and LOAD follow it in that order.
  requireRow({K::K1, K::K2, K::K3, K::RecSeq, K::SampleUpper, K::LoadUpper});
  requireRow({K::K4, K::K5, K::K6, K::PlaySeq, K::SampleLower, K::LoadLower});
  requireRow({K::Param, K::Value, K::K7, K::K8, K::K9, K::LoadSeq});
  requireRow({K::Down, K::Up, K::Cancel, K::K0, K::Enter, K::SaveSeq});
  requireColumn({K::K1, K::K4, K::K7, K::Cancel});
  requireColumn({K::K2, K::K5, K::K8, K::K0});
  requireColumn({K::K3, K::K6, K::K9, K::Enter});
  requireColumn({K::RecSeq, K::PlaySeq, K::LoadSeq, K::SaveSeq});
  requireColumn({K::SampleUpper, K::SampleLower});
  requireColumn({K::LoadUpper, K::LoadLower});
  requireColumn({K::Param, K::Down});
  requireColumn({K::Value, K::Up});

  // The volume slider is left of CONTROL. The display sits above Param and
  // Value and spans both.
  REQUIRE(right(g.volume) <= rectOf(K::Param).x);
  REQUIRE(bottom(g.display) <= rectOf(K::Param).y);
  REQUIRE(g.display.x <= rectOf(K::Param).x);
  REQUIRE(right(g.display) >= right(rectOf(K::Value)));
  REQUIRE(right(g.display) <= rectOf(K::K1).x);

  // Headings: one row under the bottom rule, below every button, centred on
  // their columns.
  for (const PanelButton& b : panelButtons()) REQUIRE(bottom(b.r) < g.headingRuleY);
  REQUIRE(bottom(g.volume) < g.headingRuleY);
  REQUIRE(g.headingRuleY < g.headingY);
  const auto h = panelHeadings();
  REQUIRE(h[0].centreX == centreX(g.volume));
  REQUIRE(h[1].centreX == (centreX(rectOf(K::Param)) + centreX(rectOf(K::Value))) / 2);
  REQUIRE(h[2].centreX == centreX(rectOf(K::K2)));
  REQUIRE(h[3].centreX == centreX(rectOf(K::RecSeq)));
  REQUIRE(h[4].centreX == centreX(rectOf(K::SampleUpper)));
  REQUIRE(h[5].centreX == centreX(rectOf(K::LoadUpper)));

  // Rules under rows 1 and 2; the pinstripe above row 4.
  REQUIRE(g.rowRuleY[0] > bottom(rectOf(K::K1)));
  REQUIRE(g.rowRuleY[0] < rectOf(K::K4).y);
  REQUIRE(g.rowRuleY[1] > bottom(rectOf(K::K4)));
  REQUIRE(g.rowRuleY[1] < rectOf(K::K7).y);
  REQUIRE(g.pinstripeY > bottom(rectOf(K::K7)));
  REQUIRE(g.pinstripeY < rectOf(K::Cancel).y);

  // Branding: the ensoniq badge at the far left, on the pinstripe. "Mirage"
  // right of the control group, its "g" descending through the pinstripe; the
  // red stripe under the pinstripe from right of the SEQ column; the subtitle
  // under the red stripe, as wide as the wordmark.
  REQUIRE(right(g.ensoniqBadge) < g.volume.x);
  REQUIRE(g.ensoniqBadge.y < g.pinstripeY);
  REQUIRE(bottom(g.ensoniqBadge) > g.pinstripeY);
  REQUIRE(g.mirageLogo.x > right(rectOf(K::LoadUpper)));
  REQUIRE(g.mirageLogo.y < g.pinstripeY);
  REQUIRE(bottom(g.mirageLogo) > g.pinstripeY);
  REQUIRE(g.redStripe.y > g.pinstripeY);
  REQUIRE(g.redStripe.x > right(rectOf(K::SaveSeq)));
  REQUIRE(g.mirageSubtitle.y >= bottom(g.redStripe));
  REQUIRE(bottom(g.mirageSubtitle) < g.lipY);
  REQUIRE(g.mirageSubtitle.x == g.mirageLogo.x);
  REQUIRE(g.mirageSubtitle.w == g.mirageLogo.w);

  // The lip strip holds the drive slot and, right of it, the drive LED.
  REQUIRE(g.driveSlot.y >= g.lipY);
  REQUIRE(g.driveLed.y >= g.lipY);
  REQUIRE(g.driveLed.x >= right(g.driveSlot));
}

TEST_CASE("digits and arrows map to keys; other chars don't", "[panel_layout]") {
  const PanelKey digits[] = {PanelKey::K0, PanelKey::K1, PanelKey::K2, PanelKey::K3, PanelKey::K4,
                             PanelKey::K5, PanelKey::K6, PanelKey::K7, PanelKey::K8, PanelKey::K9};
  for (int d = 0; d < 10; ++d) {
    const auto k = panelKeyForChar(U'0' + d);
    REQUIRE(k.has_value());
    REQUIRE(*k == digits[d]);
  }
  // U+FF10 is the fullwidth digit zero.
  for (char32_t c : {U'a', U'/', U':', U' ', U'\n', char32_t(0), char32_t(0xFF10)})
    REQUIRE_FALSE(panelKeyForChar(c).has_value());
  REQUIRE(panelKeyForShortcut(ShortcutKey::Up) == PanelKey::Up);
  REQUIRE(panelKeyForShortcut(ShortcutKey::Down) == PanelKey::Down);
  REQUIRE(panelKeyForShortcut(ShortcutKey::Return) == PanelKey::Enter);
  REQUIRE(panelKeyForShortcut(ShortcutKey::Escape) == PanelKey::Cancel);
}

TEST_CASE("known boot ROM hash matches", "[boot_rom]") {
  REQUIRE(kKnownBootRomSha1[0] == 0xec);
  REQUIRE(kKnownBootRomSha1[19] == 0x6d);
  std::array<uint8_t, 20> other = kKnownBootRomSha1;
  other[7] ^= 1;
  REQUIRE(isKnownBootRom(kKnownBootRomSha1));
  REQUIRE_FALSE(isKnownBootRom(other));
  REQUIRE_FALSE(isKnownBootRom(std::array<uint8_t, 20>{}));

  REQUIRE_ASSET(assetPath("Ensoniq Mirage EPROM Firmware/mirage.bin"));
  const Machine m(mirage_test::bootRomBytes());
  REQUIRE(isKnownBootRom(m.romSha1()));
}
