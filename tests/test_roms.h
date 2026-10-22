#pragma once
// Synthetic 4 KB boot ROMs for tests that need a running CPU but not the
// user's ROM.
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace mirage_test {

// 4 KB ROM with `code` at $F000 and the reset vector pointing at it.
inline std::vector<uint8_t> romWith(std::initializer_list<uint8_t> code) {
  std::vector<uint8_t> rom(4096, 0x12);  // NOP fill
  size_t i = 0;
  for (uint8_t b : code) rom[i++] = b;
  rom[0xFFE] = 0xF0;
  rom[0xFFF] = 0x00;
  return rom;
}

// ROM image: `parts` concatenated at $F000, reset vector at $F000.
inline std::vector<uint8_t> romFrom(std::initializer_list<std::vector<uint8_t>> parts) {
  std::vector<uint8_t> rom(4096, 0x12);
  size_t i = 0;
  for (const auto& p : parts)
    for (uint8_t b : p) rom[i++] = b;
  rom[0xFFE] = 0xF0;
  rom[0xFFF] = 0x00;
  return rom;
}

// The audio-jack rig: 32 oscillators enabled (all silent), VIA DDRB $1F, the A/D
// mux (PB3:PB2) set to `mux`, filter 7 (the channel the jack feeds) given
// cutoff CV `cutoffCv` and resonance 0, then forever: start an A/D conversion,
// wait, read it, and store the code at $8000.
inline std::vector<uint8_t> adcLoopRom(uint8_t mux, uint8_t cutoffCv = 0xFF) {
  return romFrom({
      {0x86, 0xFF, 0xB7, 0xEC, 0xE1},                    // OER = $FF
      {0x86, 0x1F, 0xB7, 0xE2, 0x02},                    // DDRB = $1F
      {0x86, uint8_t(mux << 2), 0xB7, 0xE2, 0x00},       // ORB: the mux
      {0x86, cutoffCv, 0xB7, 0xE4, 0x1F, 0xB7, 0xE4, 0x17},  // DAC preset, cutoff 7
      {0xB6, 0xEC, 0xE2, 0x3D, 0x3D, 0x3D, 0xB6, 0xEC, 0xE2,  // loop: LDA $ECE2; MUL x3; LDA $ECE2
       0xB7, 0x80, 0x00, 0x20, 0xF2},                    // STA $8000; BRA loop
  });
}

// MIDI receiver: polls the ACIA status; records any OVRN sighting at $9001 and
// stores each received byte at $8000,X+. ROM-style setup first ($03 reset, $95).
inline const std::vector<uint8_t> kAciaReceiver = {
    0x86, 0x03, 0xB7, 0xE1, 0x00,  // LDA #$03; STA $E100
    0x86, 0x95, 0xB7, 0xE1, 0x00,  // LDA #$95; STA $E100
    0x8E, 0x80, 0x00,              // LDX #$8000
    0xB6, 0xE1, 0x00,              // loop: LDA $E100
    0x85, 0x20,                    // BITA #$20
    0x27, 0x03,                    // BEQ noov
    0xB7, 0x90, 0x01,              // STA $9001
    0x85, 0x01,                    // noov: BITA #$01
    0x27, 0xF2,                    // BEQ loop
    0xB6, 0xE1, 0x01,              // LDA $E101
    0xA7, 0x80,                    // STA ,X+
    0x20, 0xEB,                    // BRA loop
};

}  // namespace mirage_test
