// Ensoniq 5503 DOC (Task 10). Expected values come from the datasheet
// (datasheets/v4_13_EnsoniqDOC.pdf) and buchty.net's 5503 page.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "mirage/doc5503.h"
#include "mirage/state_io.h"

using namespace mirage;

namespace {

constexpr uint8_t kFl = 0x00, kFh = 0x20, kVol = 0x40, kData = 0x60, kPtr = 0x80, kCtrl = 0xA0,
                  kSize = 0xC0, kOir = 0xE0, kOer = 0xE1, kAdc = 0xE2;
// Control register: CA in bits 7-4, IE bit 3, mode bits 2-1, H bit 0.
constexpr uint8_t kHalt = 0x01, kOneShot = 0x02, kSyncAm = 0x04, kSwap = 0x06, kIe = 0x08;

// A DOC on 128 KB of wave memory that records every address it reads.
struct Rig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(0x20000, 0x80);
  std::vector<uint32_t> reads;
  std::vector<std::array<int32_t, 8>> frames;
  Doc5503 doc{[this](uint32_t a) {
    reads.push_back(a);
    return mem[a & 0x1FFFF];
  }};
  Rig() {
    doc.onFrame = [this](const std::array<int32_t, 8>& f) { frames.push_back(f); };
  }
  Rig(const Rig&) = delete;

  // OER bits 5-1 hold (count - 1).
  void enable(int count) { doc.write(kOer, uint8_t(2 * (count - 1))); }
  // Sets up oscillator n; `control` is written last, so it starts the oscillator
  // when H = 0.
  void osc(int n, uint8_t ptr, uint16_t fc, uint8_t control, uint8_t sizeReg = 0,
           uint8_t vol = 0) {
    doc.write(uint8_t(kFl + n), uint8_t(fc));
    doc.write(uint8_t(kFh + n), uint8_t(fc >> 8));
    doc.write(uint8_t(kVol + n), vol);
    doc.write(uint8_t(kPtr + n), ptr);
    doc.write(uint8_t(kSize + n), sizeReg);
    doc.write(uint8_t(kCtrl + n), control);
  }
  void scans(int count) {
    const int slots = doc.enabledOscillators() + 2;
    for (int i = 0; i < count; ++i) doc.tickMasterClocks(uint32_t(8 * slots));
  }
  bool halted(int n) { return (doc.read(uint8_t(kCtrl + n)) & kHalt) != 0; }
  // Reads made from the 256-byte page `page` (any bank).
  std::vector<uint32_t> readsFrom(uint8_t page) const {
    std::vector<uint32_t> out;
    for (uint32_t a : reads)
      if (((a >> 8) & 0xFF) == page) out.push_back(a);
    return out;
  }
};

uint8_t oirFor(int n) { return uint8_t(0x40 | (n << 1) | 1); }

// The last address in `v`; fails the test cleanly when nothing was read.
uint32_t lastOf(const std::vector<uint32_t>& v) {
  REQUIRE_FALSE(v.empty());
  return v.back();
}

}  // namespace

// Enabled = OER bits 5-1 (E4-E0) + 1: the register summary puts E4-E0 in bits
// 5-1, and the SQ80 writes $2E for 24 oscillators at 38.46 kHz (buchty).
TEST_CASE("doc sample rate is 8 MHz / (8 * (enabled + 2)); enabled = OER bits 5-1 + 1") {
  Rig r;
  CHECK(r.doc.enabledOscillators() == 1);  // reset default: one oscillator
  CHECK(r.doc.sampleRate() == Catch::Approx(8e6 / 24).margin(0.01));
  r.doc.write(kOer, 0x3E);
  CHECK(r.doc.enabledOscillators() == 32);
  CHECK(r.doc.sampleRate() == Catch::Approx(29411.76).margin(0.01));
  r.doc.write(kOer, 0x2E);  // the SQ80's value
  CHECK(r.doc.enabledOscillators() == 24);
  CHECK(r.doc.sampleRate() == Catch::Approx(38461.54).margin(0.01));
  r.doc.write(kOer, 0xFF);  // the boot ROM's value ($F192)
  CHECK(r.doc.enabledOscillators() == 32);
  CHECK(r.doc.sampleRate() == Catch::Approx(29411.76).margin(0.01));
  r.doc.write(kOer, 0x00);  // a minimum of one is always enabled
  CHECK(r.doc.enabledOscillators() == 1);
  CHECK(r.doc.sampleRate() == Catch::Approx(333333.33).margin(0.01));
  r.doc.write(kOer, 0x40);  // bit 6 is not an E bit
  CHECK(r.doc.enabledOscillators() == 1);
}

TEST_CASE("doc emits one frame per scan of enabled + 2 slots of 8 clocks") {
  Rig r;
  r.doc.write(kOer, 0x3E);
  r.doc.tickMasterClocks(8 * 34 - 1);
  CHECK(r.frames.empty());
  r.doc.tickMasterClocks(1);
  CHECK(r.frames.size() == 1);
  for (int i = 0; i < 8 * 34 * 10; ++i) r.doc.tickMasterClocks(1);  // remainders carry
  CHECK(r.frames.size() == 11);
}

TEST_CASE("doc address stepping: add FC first, then accumulator bits 16-9") {
  Rig r;
  r.enable(1);
  r.osc(0, 0x12, 0x0200, 0x00);
  r.scans(4);
  CHECK(r.reads == std::vector<uint32_t>{0x1201, 0x1202, 0x1203, 0x1204});
  CHECK(r.doc.read(kData + 0) == 0x80);  // the last byte read
}

TEST_CASE("doc table size 2 KB ignores pointer bits P2-P0") {
  Rig r;
  r.enable(1);
  r.osc(0, 0x2F, 0x0200, 0x00, uint8_t(3 << 3 | 3));  // size 3, res 3
  r.scans(3);
  CHECK(r.reads == std::vector<uint32_t>{0x2801, 0x2802, 0x2803});
}

TEST_CASE("doc resolution 4 with size 3 takes accumulator bits from bit 10") {
  Rig r;
  r.enable(1);
  r.osc(0, 0x2F, 0x0200, 0x00, uint8_t(3 << 3 | 4));  // size 3, res 4
  r.scans(4);
  // acc = 0x200, 0x400, 0x600, 0x800 >> 10: the same FC steps at half the rate.
  CHECK(r.reads == std::vector<uint32_t>{0x2800, 0x2801, 0x2801, 0x2802});
}

TEST_CASE("doc address formation follows the size/resolution table for every setting") {
  for (int size = 0; size < 8; ++size) {
    for (int res = 0; res < 8; ++res) {
      Rig r;
      r.enable(1);
      const uint16_t fc = 0xFEDB;
      r.osc(0, 0xFF, fc, 0x00, uint8_t(size << 3 | res));
      r.scans(40);
      const uint32_t mask = (256u << size) - 1;
      const uint32_t period = 1u << (17 + res);  // one pass of the table
      uint32_t acc = 0;
      bool ok = r.reads.size() == 40;
      for (size_t i = 0; ok && i < r.reads.size(); ++i) {
        acc = (acc + fc) % period;
        const uint32_t want = ((0xFFu << 8) & ~mask) | ((acc >> (9 + res - size)) & mask);
        ok = r.reads[i] == want;
      }
      INFO("size " << size << " res " << res);
      CHECK(ok);
    }
  }
  // Datasheet example: a 512-byte table at resolution 7 uses accumulator bits
  // 15-23 and pointer bits 7-1.
  Rig r;
  r.enable(1);
  r.osc(0, 0x13, 0x8000, 0x00, uint8_t(1 << 3 | 7));
  r.scans(1);
  CHECK(lastOf(r.reads) == 0x1201);  // acc 0x8000 >> 15 = 1, P0 ignored
}

TEST_CASE("doc bank select reads from the upper 64 KB") {
  Rig r;
  r.enable(1);
  r.osc(0, 0x12, 0x0200, 0x00, 0x40);  // BS = 1
  r.scans(2);
  CHECK(r.reads == std::vector<uint32_t>{0x11201, 0x11202});
}

TEST_CASE("doc a zero byte halts the oscillator, sets H and interrupts through the OIR") {
  Rig r;
  r.enable(6);
  r.mem[0x3001] = 0x00;
  r.osc(5, 0x30, 0x0200, kIe);
  CHECK_FALSE(r.doc.irq());
  CHECK(r.doc.read(kOir) == 0xFF);  // nothing pending: bit 7 = 1
  r.scans(1);
  CHECK(r.halted(5));
  CHECK(r.doc.read(kData + 5) == 0x00);
  CHECK(r.doc.irq());
  CHECK(r.doc.read(kOir) == oirFor(5));
  CHECK_FALSE(r.doc.irq());
  CHECK((r.doc.read(kOir) & 0x80) != 0);
  r.scans(3);  // halted: no more reads from its table
  CHECK(r.readsFrom(0x30).size() == 1);
}

TEST_CASE("doc a zero byte halts in free-run mode too, without IE no interrupt") {
  Rig r;
  r.enable(1);
  r.mem[0x3003] = 0x00;
  r.osc(0, 0x30, 0x0200, 0x00);
  r.scans(5);
  CHECK(r.halted(0));
  CHECK_FALSE(r.doc.irq());
  CHECK(r.readsFrom(0x30).size() == 3);
}

TEST_CASE("doc one-shot halts after a single pass of a 256-byte table") {
  Rig r;
  r.enable(1);
  r.osc(0, 0x10, 0x0200, kOneShot | kIe);
  r.scans(255);
  CHECK_FALSE(r.halted(0));
  CHECK_FALSE(r.doc.irq());
  CHECK(lastOf(r.reads) == 0x10FF);
  r.scans(1);
  CHECK(r.halted(0));
  CHECK(r.doc.read(kOir) == oirFor(0));
  CHECK(r.reads.size() == 255);  // the halting update reads nothing
  // H = 1 with M0 = 1 holds the accumulator at 0: a restart begins at the top.
  r.doc.write(kCtrl + 0, kOneShot);
  r.scans(1);
  CHECK(lastOf(r.reads) == 0x1001);
}

TEST_CASE("doc free-run wraps and interrupts at the end of every pass") {
  Rig r;
  r.enable(1);
  r.osc(0, 0x10, 0x0300, kIe);  // 2/3 of the way through a step at the wrap
  for (int pass = 0; pass < 3; ++pass) {
    for (int i = 0; i < 1000 && !r.doc.irq(); ++i) r.scans(1);
    CHECK(r.doc.read(kOir) == oirFor(0));
  }
  CHECK_FALSE(r.halted(0));
  // acc = 0x300 * k; the wrap keeps the phase (0x20100 -> 0x100, 0x20200 -> 0x200).
  const auto& v = r.reads;
  REQUIRE(v.size() > 172);
  CHECK(v[169] == 0x10FF);  // acc 170 * 0x300 = 0x1FE00
  CHECK(v[170] == 0x1000);  // acc 0x20100 - 0x20000 = 0x100 -> offset 0
  CHECK(v[171] == 0x1002);  // acc 0x400
}

TEST_CASE("doc writing H = 1 with M0 = 1 resets the accumulator") {
  Rig r;
  r.enable(1);
  r.osc(0, 0x10, 0x0200, kOneShot);
  r.scans(10);
  CHECK(lastOf(r.reads) == 0x100A);
  r.doc.write(kCtrl + 0, kOneShot | kHalt);
  r.doc.write(kCtrl + 0, kOneShot);
  r.scans(1);
  CHECK(lastOf(r.reads) == 0x1001);
  // Free run (M0 = 0): halting keeps the accumulator.
  r.osc(0, 0x10, 0x0200, 0x00);
  r.scans(4);
  CHECK(lastOf(r.reads) == 0x1005);
  r.doc.write(kCtrl + 0, kHalt);
  r.doc.write(kCtrl + 0, 0x00);
  r.scans(1);
  CHECK(lastOf(r.reads) == 0x1006);
}

TEST_CASE("doc swap: at the end the even oscillator halts and the odd one starts from 0") {
  Rig r;
  r.enable(4);
  r.osc(2, 0x20, 0x0200, kSwap);
  r.osc(3, 0x30, 0x0200, kOneShot | kHalt);
  r.scans(255);
  CHECK_FALSE(r.halted(2));
  CHECK(r.halted(3));
  CHECK(r.readsFrom(0x30).empty());
  r.scans(1);
  CHECK(r.halted(2));
  CHECK_FALSE(r.halted(3));
  REQUIRE(r.readsFrom(0x30).size() == 1);
  CHECK(r.readsFrom(0x30)[0] == 0x3001);  // acc 0 + FC: started from the top
  r.scans(255);
  CHECK(r.halted(3));  // osc 3 then plays its one-shot table
}

TEST_CASE("doc sync: when the even oscillator wraps, its odd partner's accumulator resets") {
  for (bool sync : {true, false}) {
    Rig r;
    r.enable(2);
    r.osc(0, 0x20, 0x0200, sync ? kSyncAm : 0x00);
    r.osc(1, 0x40, 0x0100, 0x00);
    r.scans(255);
    CHECK(lastOf(r.readsFrom(0x40)) == 0x407F);  // acc 255 * 0x100
    r.scans(1);
    CHECK_FALSE(r.halted(0));  // the even oscillator keeps running
    CHECK(lastOf(r.readsFrom(0x40)) == (sync ? 0x4000u : 0x4080u));
  }
}

TEST_CASE("doc AM: the odd oscillator's volume is the even partner's wave byte") {
  Rig r;
  r.enable(2);
  r.mem.assign(r.mem.size(), 0x90);
  for (int i = 0; i < 256; ++i) r.mem[0x4000 + i] = 0xC0;
  r.osc(0, 0x20, 0x0200, 0x10, 0, 0);            // channel 1, volume 0: silent
  r.osc(1, 0x40, 0x0200, 0x20 | kSyncAm, 0, 7);  // channel 2, volume ignored
  r.scans(1);
  REQUIRE(r.frames.size() == 1);
  CHECK(r.frames[0] == std::array<int32_t, 8>{0, 0, (0xC0 - 128) * 0x90, 0, 0, 0, 0, 0});
}

TEST_CASE("doc output: (sample - 128) * volume summed per channel CA & 7") {
  Rig r;
  r.enable(3);
  r.mem.assign(r.mem.size(), 0xC0);
  r.osc(0, 0x10, 0x0200, 0x30, 0, 255);  // channel 3
  r.scans(1);
  REQUIRE(r.frames.size() == 1);
  CHECK(r.frames[0] == std::array<int32_t, 8>{0, 0, 0, (0xC0 - 128) * 255, 0, 0, 0, 0});
  r.mem.assign(r.mem.size(), 0x40);
  r.osc(1, 0x11, 0x0200, 0xB0, 0, 10);         // CA = 11 -> channel 3
  r.osc(2, 0x12, 0x0200, 0x50 | kHalt, 0, 99);  // halted: contributes 0
  r.scans(1);
  CHECK(r.frames[1] == std::array<int32_t, 8>{0, 0, 0, (0x40 - 128) * 255 + (0x40 - 128) * 10,
                                              0, 0, 0, 0});
}

TEST_CASE("doc interrupts leave the OIR in the order they happened") {
  Rig r;
  r.enable(4);
  r.osc(1, 0x10, 0x0200, kOneShot | kIe);  // ends at scan 256
  r.osc(3, 0x10, 0x0200, kOneShot | kIe);
  r.scans(1);
  r.doc.write(kCtrl + 1, kOneShot | kIe | kHalt);  // restart osc 1 one scan later
  r.doc.write(kCtrl + 1, kOneShot | kIe);
  r.scans(256);
  CHECK(r.doc.read(kOir) == oirFor(3));
  CHECK(r.doc.irq());
  CHECK(r.doc.read(kOir) == oirFor(1));
  CHECK_FALSE(r.doc.irq());
  CHECK(r.doc.read(kOir) == (0xC1 | (1 << 1)));  // empty: bit 7 = 1, last osc kept
}

TEST_CASE("doc an oscillator is queued once until the OIR passes it on") {
  Rig r;
  r.enable(1);
  r.osc(0, 0x10, 0xFFFF, kIe);  // free run, wraps every 2-3 scans
  r.scans(40);
  CHECK(r.doc.read(kOir) == oirFor(0));
  CHECK_FALSE(r.doc.irq());
}

TEST_CASE("doc completion with IE = 0 leaves no interrupt behind when IE is set later") {
  Rig r;
  r.enable(2);
  r.mem[0x2003] = 0x00;
  r.osc(0, 0x10, 0x0200, kOneShot);  // ends its table with IE = 0
  r.osc(1, 0x20, 0x0200, 0x00);      // halts on the zero at $2003 with IE = 0
  r.scans(256);
  CHECK(r.halted(0));
  CHECK(r.halted(1));
  CHECK_FALSE(r.doc.irq());
  r.doc.write(kCtrl + 0, kOneShot | kHalt | kIe);
  r.doc.write(kCtrl + 1, kHalt | kIe);
  CHECK_FALSE(r.doc.irq());
  CHECK((r.doc.read(kOir) & 0x80) != 0);  // nothing pending
}

TEST_CASE("doc register read-back") {
  Rig r;
  r.osc(7, 0x9A, 0x5634, 0x5B, 0x3D, 0x78);
  CHECK(r.doc.read(kFl + 7) == 0x34);
  CHECK(r.doc.read(kFh + 7) == 0x56);
  CHECK(r.doc.read(kVol + 7) == 0x78);
  CHECK(r.doc.read(kPtr + 7) == 0x9A);
  CHECK(r.doc.read(kCtrl + 7) == 0x5B);
  CHECK(r.doc.read(kSize + 7) == (0x3D | 0x80));  // bit 7 unused: reads 1
  r.doc.write(kData + 7, 0x11);                    // read only
  CHECK(r.doc.read(kData + 7) == 0x80);
  r.doc.write(kOer, 0x3E);
  CHECK(r.doc.read(kOer) == 0xFF);  // X bits 7, 6 and 0 read 1
  r.doc.write(kOer, 0x08);
  CHECK(r.doc.read(kOer) == 0xC9);
  CHECK(r.doc.read(0xE5) == 0xFF);
}

TEST_CASE("doc A/D: a read of $E2 returns adcRead()") {
  Rig r;
  int calls = 0;
  r.doc.adcRead = [&calls] {
    ++calls;
    return uint8_t(0x5A);
  };
  CHECK(r.doc.read(kAdc) == 0x5A);
  CHECK(calls == 1);
}

TEST_CASE("doc save/load restores every register, the FIFO and the scan position") {
  Rig a;
  a.enable(5);
  a.mem[0x3003] = 0x00;
  a.osc(0, 0x10, 0x0123, 0x10 | kIe, 0x12, 200);
  a.osc(1, 0x20, 0x0F00, 0x20 | kOneShot | kIe, 0, 100);
  a.osc(2, 0x30, 0x0200, 0x30 | kIe, 0, 50);  // halts on the zero at $3003
  a.osc(3, 0x40, 0x3000, 0x40 | kSyncAm, 0, 77);
  a.osc(4, 0x50, 0x0700, 0x50 | kSwap, 0x49, 33);
  a.scans(40);
  a.doc.tickMasterClocks(8 * 3 + 5);  // mid-scan, mid-slot
  REQUIRE(a.doc.irq());
  StateWriter w;
  a.doc.save(w);
  const auto blob = w.take();

  Rig b;
  b.mem = a.mem;
  StateReader rd(blob);
  b.doc.load(rd);
  CHECK(rd.atEnd());
  a.reads.clear();
  a.frames.clear();
  for (int i = 0; i < 2000; ++i) {
    a.doc.tickMasterClocks(13);
    b.doc.tickMasterClocks(13);
  }
  CHECK(a.reads == b.reads);
  CHECK(a.frames == b.frames);
  for (int reg = 0; reg <= 0xE1; ++reg) CHECK(a.doc.read(uint8_t(reg)) == b.doc.read(uint8_t(reg)));
}

TEST_CASE("doc load rejects bad state and leaves the chip unchanged") {
  Rig a;
  a.enable(2);
  a.osc(1, 0x10, 0x0200, 0x30, 0, 9);
  StateWriter w;
  a.doc.save(w);
  auto blob = w.take();

  Rig b;
  b.osc(0, 0x77, 0x1111, 0x05, 0x22, 0x33);
  const auto check = [&b] {
    CHECK(b.doc.read(kPtr + 0) == 0x77);
    CHECK(b.doc.enabledOscillators() == 1);
  };
  {
    auto truncated = blob;
    truncated.pop_back();
    StateReader rd(truncated);
    CHECK_THROWS_AS(b.doc.load(rd), StateError);
    check();
  }
  {
    auto bad = blob;
    bad[4] ^= 0xFF;  // first byte after the tag
    bad.back() = 0xFF;
    StateReader rd(bad);
    CHECK_THROWS_AS(b.doc.load(rd), StateError);
    check();
  }
  StateReader rd(blob);
  b.doc.load(rd);
  CHECK(b.doc.read(kPtr + 1) == 0x10);
}

TEST_CASE("doc reset halts every oscillator and empties the FIFO") {
  Rig r;
  r.enable(1);
  r.mem[0x1001] = 0;
  r.osc(0, 0x10, 0x0200, kIe);
  r.scans(1);
  REQUIRE(r.doc.irq());
  r.doc.reset();
  CHECK_FALSE(r.doc.irq());
  CHECK(r.doc.enabledOscillators() == 1);
  for (int n = 0; n < 32; ++n) CHECK(r.halted(n));
  r.reads.clear();
  r.scans(10);
  CHECK(r.reads.empty());
}
