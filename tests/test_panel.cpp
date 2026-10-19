#include <catch2/catch_test_macros.hpp>

#include <string>

#include "mirage/clocks.h"
#include "mirage/panel.h"

using namespace mirage;

namespace {
constexpr uint64_t ms(double t) { return uint64_t(t * 1e-3 * kCpuClockHz); }
}  // namespace

TEST_CASE("panel with no keys down reads all rows high") {
  Panel p;
  for (uint8_t col = 0; col < 8; ++col) CHECK(p.rowSense(col) == 0xE0);
}

TEST_CASE("panel Enter is row 1 column 7") {
  Panel p;
  p.setKey(PanelKey::Enter, true);
  const uint8_t s = p.rowSense(7);
  CHECK((s & 0x40) == 0);  // row 1 sense low
  CHECK((s & 0x20) != 0);
  CHECK((s & 0x80) != 0);
  CHECK(p.rowSense(6) == 0xE0);  // other columns unaffected
  p.setKey(PanelKey::Enter, false);
  CHECK(p.rowSense(7) == 0xE0);
}

TEST_CASE("panel column select uses PA0-2 only") {
  Panel p;
  p.setKey(PanelKey::LoadUpper, true);  // row 0 column 0
  p.setKey(PanelKey::Cancel, true);     // row 2 column 7
  CHECK(p.rowSense(0x18) == 0xC0);      // PA3/PA4/high bits ignored: column 0
  CHECK(p.rowSense(0xFF) == 0x60);      // column 7: row 2 low
  CHECK(p.rowSense(0x03) == 0xE0);
}

TEST_CASE("panel keypad matrix covers all 24 keys") {
  for (uint8_t k = 0; k < 24; ++k) {
    Panel p;
    p.setKey(PanelKey(k), true);
    for (uint8_t col = 0; col < 8; ++col) {
      const uint8_t expect = (col == (k & 7)) ? uint8_t(0xE0 & ~(0x20 << (k >> 3))) : 0xE0;
      CHECK(p.rowSense(col) == expect);
    }
  }
}

TEST_CASE("panel key names are the enumerator names and round-trip") {
  CHECK(kPanelKeyCount == 24);
  // Pinned against the matrix (row*8 + column, global-constraints keypad table).
  CHECK(panelKeyName(PanelKey::LoadUpper) == "LoadUpper");
  CHECK(panelKeyName(PanelKey::SampleLower) == "SampleLower");
  CHECK(panelKeyName(PanelKey::K3) == "K3");
  CHECK(panelKeyName(PanelKey::K0) == "K0");
  CHECK(panelKeyName(PanelKey::Enter) == "Enter");
  CHECK(panelKeyName(PanelKey::K1) == "K1");
  CHECK(panelKeyName(PanelKey::Up) == "Up");
  CHECK(panelKeyName(PanelKey::Cancel) == "Cancel");
  CHECK(uint8_t(PanelKey::K3) == 8);
  CHECK(uint8_t(PanelKey::Enter) == 15);
  CHECK(uint8_t(PanelKey::Cancel) == 23);
  for (int k = 0; k < kPanelKeyCount; ++k) {
    const auto name = panelKeyName(PanelKey(k));
    REQUIRE_FALSE(name.empty());
    CHECK(panelKeyFromName(name) == PanelKey(k));
  }
}

TEST_CASE("panel key name lookup ignores case and rejects unknown names") {
  CHECK(panelKeyFromName("enter") == PanelKey::Enter);
  CHECK(panelKeyFromName("LOADLOWER") == PanelKey::LoadLower);
  CHECK(panelKeyFromName("k7") == PanelKey::K7);
  CHECK_FALSE(panelKeyFromName("").has_value());
  CHECK_FALSE(panelKeyFromName("7").has_value());
  CHECK_FALSE(panelKeyFromName("Enterr").has_value());
  CHECK_FALSE(panelKeyFromName("bogus").has_value());
  CHECK(panelKeyName(PanelKey(24)).empty());  // out of range
}

TEST_CASE("panel two keys in one column pull two rows low") {
  Panel p;
  p.setKey(PanelKey::K3, true);   // row 1 column 0
  p.setKey(PanelKey::K1, true);   // row 2 column 0
  CHECK(p.rowSense(0) == 0x20);
}

TEST_CASE("panel right digit lights on PA4 low, PA3 high") {
  Panel p;
  // PA3 high (left off), PA4 low (right on), segment 3 (d).
  p.portAWritten(0x0B, 100);
  auto d = p.display(100);
  CHECK(d.right == 0x08);
  CHECK(d.left == 0);
}

TEST_CASE("panel left digit lights on PA3 low, PA4 high") {
  Panel p;
  p.portAWritten(0x13, 100);
  auto d = p.display(100);
  CHECK(d.left == 0x08);
  CHECK(d.right == 0);
}

TEST_CASE("panel both digits light when both enables are low") {
  Panel p;
  p.portAWritten(0x03, 100);
  auto d = p.display(100);
  CHECK(d.left == 0x08);
  CHECK(d.right == 0x08);
}

TEST_CASE("panel digits stay dark when both enables are high") {
  Panel p;
  p.portAWritten(0x1B, 100);
  auto d = p.display(100);
  CHECK(d.left == 0);
  CHECK(d.right == 0);
}

TEST_CASE("panel integrates segments over the multiplex scan") {
  Panel p;
  uint64_t t = 1000;
  // Left digit shows segments a and g (0, 6), right digit shows dp (7).
  p.portAWritten(0x10 | 0, t += 2000);  // left on (PA3 low), PA4 high
  p.portAWritten(0x08 | 7, t += 2000);  // right on
  p.portAWritten(0x10 | 6, t += 2000);
  p.portAWritten(0x18 | 3, t += 2000);  // both off: records nothing
  auto d = p.display(t);
  CHECK(d.left == 0x41);
  CHECK(d.right == 0x80);
}

TEST_CASE("panel display is blank 40 ms after the last write, lit at 30 ms") {
  Panel p;
  p.portAWritten(0x03, 5000);
  CHECK(p.display(5000 + ms(30)).left == 0x08);
  CHECK(p.display(5000 + ms(40)).left == 0);
  CHECK(p.display(5000 + ms(40)).right == 0);
}

TEST_CASE("panel a rewrite refreshes the persistence window") {
  Panel p;
  p.portAWritten(0x03, 0);
  p.portAWritten(0x03, ms(30));
  CHECK(p.display(ms(55)).left == 0x08);
  CHECK(p.display(ms(66)).left == 0);
}

TEST_CASE("panel keyDown reflects setKey") {
  Panel p;
  CHECK_FALSE(p.keyDown(PanelKey::Enter));
  p.setKey(PanelKey::Enter, true);
  CHECK(p.keyDown(PanelKey::Enter));
  CHECK_FALSE(p.keyDown(PanelKey::Cancel));
  p.setKey(PanelKey::Enter, false);
  CHECK_FALSE(p.keyDown(PanelKey::Enter));
}

TEST_CASE("panel clearDisplay blanks lit segments but keeps held keys") {
  Panel p;
  p.setKey(PanelKey::Enter, true);
  p.portAWritten(0x03, 10);
  p.clearDisplay();
  CHECK(p.display(10).left == 0);
  CHECK((p.rowSense(7) & 0x40) == 0);
}

TEST_CASE("panel save/load round-trips keys and lit segments") {
  Panel p;
  p.setKey(PanelKey::Value, true);
  p.portAWritten(0x13, 777);
  p.portAWritten(0x0B | 5, 800);
  StateWriter w;
  p.save(w);
  const auto bytes = w.take();

  Panel q;
  StateReader r(bytes);
  q.load(r);
  CHECK(r.atEnd());
  CHECK(q.rowSense(6) == p.rowSense(6));
  CHECK(q.display(900).left == p.display(900).left);
  CHECK(q.display(900).right == p.display(900).right);
  StateWriter w2;
  q.save(w2);
  CHECK(w2.take() == bytes);
}

TEST_CASE("panel load leaves the panel unchanged on error") {
  Panel p;
  p.setKey(PanelKey::Enter, true);
  StateWriter w;
  p.save(w);
  auto bytes = w.take();
  bytes.resize(bytes.size() - 3);
  Panel q;
  q.setKey(PanelKey::Cancel, true);
  StateReader r(bytes);
  CHECK_THROWS_AS(q.load(r), StateError);
  CHECK(q.rowSense(7) == 0x60);  // still Cancel only
}
