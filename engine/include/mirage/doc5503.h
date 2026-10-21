#pragma once
#include <array>
#include <cstdint>
#include <functional>

#include "mirage/state_io.h"

namespace mirage {

// Ensoniq 5503 DOC: 32 wavetable oscillators (datasheet v4_13_EnsoniqDOC.pdf,
// buchty.net's 5503 page for the AM/sync pairing and the resolution table).
//
// Registers (n = oscillator 0-31): $00+n frequency low, $20+n frequency high,
// $40+n volume, $60+n last wave byte (read only), $80+n wavetable pointer,
// $A0+n control (CA3-0 IE M1 M0 H), $C0+n bank select / table size / resolution
// (X BS T2-0 R2-0), $E0 OIR, $E1 oscillator enable, $E2 A/D.
//
// Timing: one slot per 8 input clocks. A scan is one slot for each enabled
// oscillator in order, then two refresh slots; onFrame is called at the end
// of every scan. Enabled oscillators = OER bits 5-1 (E4-E0) + 1, so 1-32
// ($3E or the ROM's $FF = 32, the SQ80's $2E = 24; reset default one).
//
// Oscillator update (one slot): A = A + FC (24-bit accumulator), then the
// 16-bit address is ((ptr << 8) & ~mask) | ((A >> (9 + res - size)) & mask),
// mask = (256 << size) - 1, with BS selecting the upper 64 KB. The table is
// complete when A reaches 2^(17 + res) (the carry out of the top address
// bit). Then by mode (M1 M0):
//   00 free run and 10 sync/AM: wrap (A keeps its phase). An even oscillator
//      in mode 10 resets its odd partner's accumulator (sync).
//   01 one shot and 11 swap: halt (H = 1, A = 0).
// A wave byte of 0 halts the oscillator in every mode (H = 1; A = 0 when
// M0 = 1). When the DOC halts a swap oscillator n (end of table or zero byte),
// its partner n ^ 1 is started (H = 0, A = 0). A halt written by the CPU starts
// nothing.
// Retrigger (MAME es5503.cpp halt_osc, current master; its comment: "Verified on
// IIgs hardware, and hinted by the Ensoniq data sheet"): an even oscillator that
// is not in swap mode, whose odd partner is in swap mode, is not halted at the
// end of its table or on a zero byte. It clears H again and runs on, its phase
// kept as in a wrap (a one-shot then loops like free run); the partner is left
// alone; the interrupt is raised as for a halt.
// A completed table and a zero halt each raise an interrupt: queued in the OIR
// FIFO when IE = 1; with IE = 0 nothing is recorded (setting IE later raises
// nothing). An oscillator is in the FIFO at most once.
//
// Sync/AM at the end of the table (checked, Plan 3 Task 7): the datasheet's sync
// text has the even oscillator wrap to the start of its table and reset its odd
// partner, so mode 10 wraps here. MAME agrees (halt_osc treats sync/AM as free
// run and loops them). buchty's H bit note lists sync among the modes that halt
// "after completion"; the datasheet's description, which only makes sense for a
// wrapping oscillator, is followed. AM (odd oscillator in mode 10) wraps the
// same way.
//
// CA3 (ca3()): the DOC drives CA3-CA0 with the channel assignment of the
// oscillator in the current slot. The Mirage wires CA3 to VIA PB5 (MAME
// enmirage.cpp, "Q Chip sync"), and the Soundprocess OS waits for PB5 = 1 before
// each DOC register write, then delays by the oscillator group, so PB5 marks a
// fixed point of every scan. buchty's page notes that the refresh slots are the
// ones without a channel strobe (CSTRB missing while E = 1), so the chip does
// mark them on its pins, which fits a sync locked to the scan. Every oscillator
// the OS uses has CA3 = 0, so the line must go high in the two refresh slots:
// modelled as CA3 = 1 there (what the CA lines carry in refresh is not
// documented; boot-rom.md 13.7).
//
// A/D ($E2): a read returns the result of the previous conversion and starts a
// new one, sampling adcRead() (the analog input) at that moment. A conversion
// takes 26 input clocks; a read before it completes loses it (the result keeps
// the older value) and starts another. Power-on result $80.
//
// Output: running oscillators add (byte - 128) * volume to their channel
// (CA & 7, the Mirage has 8). An odd oscillator in mode 10 is amplitude
// modulated: its volume is the even partner's last wave byte.
class Doc5503 {
 public:
  static constexpr int kOscillators = 32;
  static constexpr int kChannels = 8;

  explicit Doc5503(std::function<uint8_t(uint32_t addr17)> waveRead);

  uint8_t read(uint8_t reg);  // $E0 pops the interrupt FIFO, $E2 calls adcRead
  void write(uint8_t reg, uint8_t v);
  void reset();                       // power-on state; callbacks are kept
  void tickMasterClocks(uint32_t n);  // advances oscillator slots
  bool irq() const { return fifoCount_ != 0; }
  int enabledOscillators() const;
  bool ca3() const;  // the CA3 output pin, see above
  double sampleRate() const;  // 8e6 / (8 * (enabled + 2))

  std::function<uint8_t()> adcRead;  // the A/D input, sampled when a conversion starts
  // Emitted once per full scan: per-channel sums of (sample-128)*volume,
  // channels 0..7 = CA & 7.
  std::function<void(const std::array<int32_t, kChannels>&)> onFrame;

  void save(StateWriter&) const;
  void load(StateReader&);  // commits only on success

 private:
  struct Osc {
    uint16_t fc = 0;
    uint8_t volume = 0, data = 0x80, pointer = 0, control = 0, sizeReg = 0;
    uint32_t acc = 0;  // 24 bits
  };

  void slot();
  void updateOscillator(int n);
  bool retriggers(int n) const;
  void halt(int n);
  void interrupt(int n);

  std::function<uint8_t(uint32_t)> waveRead_;
  std::array<Osc, kOscillators> osc_{};
  uint8_t oer_ = 0;
  std::array<uint8_t, kOscillators> fifo_{};  // oscillator numbers, oldest first
  int fifoCount_ = 0;
  uint8_t lastIrqOsc_ = 0x1F;  // OIR bits 5-1 once the FIFO is empty
  uint32_t clockRem_ = 0;      // input clocks into the current slot (0-7)
  int slot_ = 0;               // position in the scan
  std::array<int32_t, kChannels> frame_{};
  uint64_t clock_ = 0;  // input clocks since reset
  uint64_t adcStart_ = 0;  // clock_ when the conversion in progress started
  uint8_t adcResult_ = 0x80, adcSample_ = 0x80;
  bool adcBusy_ = false;
};

}  // namespace mirage
