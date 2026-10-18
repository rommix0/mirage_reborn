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
//   01 one shot and 11 swap: halt (H = 1, A = 0). In swap mode the partner
//      (n ^ 1) is started: H = 0, A = 0.
// A wave byte of 0 halts the oscillator in every mode (H = 1; A = 0 when
// M0 = 1; a swap oscillator starts its partner). A completed table and a zero
// halt each raise an interrupt: queued in the OIR FIFO when IE = 1, otherwise
// remembered and queued when IE is next changed to 1 (datasheet, IE bit). An
// oscillator is in the FIFO at most once.
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
  double sampleRate() const;  // 8e6 / (8 * (enabled + 2))

  std::function<uint8_t()> adcRead;  // a read of $E2 calls this
  // Emitted once per full scan: per-channel sums of (sample-128)*volume,
  // channels 0..7 = CA & 7.
  std::function<void(const std::array<int32_t, kChannels>&)> onFrame;

  void save(StateWriter&) const;
  void load(StateReader&);  // commits only on success

 private:
  struct Osc {
    uint16_t fc = 0;
    uint8_t volume = 0, data = 0x80, pointer = 0, control = 0, sizeReg = 0;
    uint32_t acc = 0;      // 24 bits
    bool latent = false;   // completed with IE = 0, not yet passed to the OIR
  };

  void slot();
  void updateOscillator(int n);
  void halt(int n);
  void interrupt(int n);
  void queue(int n);

  std::function<uint8_t(uint32_t)> waveRead_;
  std::array<Osc, kOscillators> osc_{};
  uint8_t oer_ = 0;
  std::array<uint8_t, kOscillators> fifo_{};  // oscillator numbers, oldest first
  int fifoCount_ = 0;
  uint8_t lastIrqOsc_ = 0x1F;  // OIR bits 5-1 once the FIFO is empty
  uint32_t clockRem_ = 0;      // input clocks into the current slot (0-7)
  int slot_ = 0;               // position in the scan
  std::array<int32_t, kChannels> frame_{};
};

}  // namespace mirage
