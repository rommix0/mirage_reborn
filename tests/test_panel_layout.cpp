// Replica panel layout and keyboard shortcuts (host/include/mirage/panel_layout.h),
// and the known boot ROM hash (host/include/mirage/boot_rom.h).
#include <catch2/catch_test_macros.hpp>

#include <array>
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
const PanelButton& findButton(PanelKey k) {
  for (const PanelButton& b : panelButtons())
    if (b.key == k) return b;
  FAIL("missing key");
  return panelButtons()[0];
}
}  // namespace

TEST_CASE("panel layout has each of the 24 keys exactly once", "[panel_layout]") {
  const auto buttons = panelButtons();
  REQUIRE(buttons.size() == 24);
  std::set<PanelKey> seen;
  for (const PanelButton& b : buttons) {
    seen.insert(b.key);
    REQUIRE_FALSE(b.label.empty());
  }
  REQUIRE(seen.size() == 24);
  for (size_t i = 0; i < buttons.size(); ++i) REQUIRE(size_t(buttons[i].key) == i);  // enum order
  // Spot-check the table against the manual: labels and cap colours.
  REQUIRE(findButton(PanelKey::K0).label == "0/Prog");
  REQUIRE(findButton(PanelKey::K0).cap == CapColour::Yellow);
  REQUIRE(findButton(PanelKey::Down).label == "Off/▼");
  REQUIRE(findButton(PanelKey::Up).label == "On/▲");
  REQUIRE(findButton(PanelKey::Enter).cap == CapColour::White);
  REQUIRE(findButton(PanelKey::SampleUpper).cap == CapColour::Black);
  REQUIRE(findButton(PanelKey::LoadLower).cap == CapColour::White);
  REQUIRE(findButton(PanelKey::LoadLower).label == "Lower");
  REQUIRE(findButton(PanelKey::RecSeq).label == "Rec");
  REQUIRE(findButton(PanelKey::K5).cap == CapColour::Grey);
  REQUIRE(findButton(PanelKey::K4).cap == CapColour::Yellow);

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
  REQUIRE(g.display.x == 186);
  REQUIRE(g.display.y == 34);
  REQUIRE(g.display.w == 100);
  REQUIRE(g.display.h == 58);
  const auto buttons = panelButtons();
  for (size_t i = 0; i < buttons.size(); ++i) {
    REQUIRE(inside(buttons[i].r, g.width, g.height));
    REQUIRE_FALSE(overlaps(buttons[i].r, g.display));
    for (size_t j = i + 1; j < buttons.size(); ++j)
      REQUIRE_FALSE(overlaps(buttons[i].r, buttons[j].r));
  }
  for (const PanelRect& r :
       {g.display, g.volume, g.driveSlot, g.driveLed, g.ensoniqBadge, g.mirageLogo})
    REQUIRE(inside(r, g.width, g.height));
  for (const PanelHeading& h : panelHeadings()) {
    REQUIRE(h.centreX > 0);
    REQUIRE(h.centreX < g.width);
  }
  REQUIRE(g.headingY < g.lipY);
  REQUIRE(g.lipY < g.height);
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
