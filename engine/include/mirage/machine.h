#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "mirage/bus.h"
#include "mirage/cpu6809.h"

namespace mirage {

// The whole Mirage: 6809 + RAM + banked sample RAM + boot ROM + I/O decode.
// I/O devices are stubs for now (reads $FF, writes ignored); later tasks plug
// the ACIA, VIA, FDC, DOC and filter chips into ioRead/ioWrite/tickDevices.
class Machine : private Bus {
 public:
  static constexpr uint32_t kBootRomSize = 0x1000;
  static constexpr uint32_t kSampleRamSize = 0x20000;

  // Throws std::invalid_argument unless bootRom is exactly 4096 bytes.
  explicit Machine(std::span<const uint8_t> bootRom);
  Machine(const Machine&) = delete;
  Machine& operator=(const Machine&) = delete;

  // Clears RAM and sample RAM to 0, resets the chips and the CPU. Never
  // touches an inserted disk.
  void powerOn();
  // Steps the CPU until the requested E-cycles have elapsed. The last
  // instruction may overshoot; the excess is credited against the next call,
  // so cycles() stays exact and long-run totals track the requests.
  void runCycles(uint64_t eCycles);
  uint64_t cycles() const { return cycles_; }
  uint8_t peek(uint16_t addr);  // CPU view, no I/O side effects (I/O reads $FF)
  uint8_t sampleRam(uint32_t addr) const { return sample_[addr & (kSampleRamSize - 1)]; }
  Cpu6809& cpu() { return cpu_; }

  std::vector<uint8_t> saveState() const;
  void loadState(std::span<const uint8_t> data);  // throws StateError; unchanged on failure

 private:
  friend struct MachineTestAccess;

  uint8_t read(uint16_t addr) override;
  void write(uint16_t addr, uint8_t value) override;
  uint8_t ioRead(uint16_t addr);
  void ioWrite(uint16_t addr, uint8_t value);
  void tickDevices(uint32_t cycles);
  void setSampleBank(int quarter) { bank_ = uint8_t(quarter & 3); }  // VIA PB1:PB0 from Task 6

  std::array<uint8_t, kBootRomSize> rom_{};
  std::array<uint8_t, 0x4000> ram_{};   // $8000-$BFFF
  std::array<uint8_t, 0x2000> exp_{};   // $C000-$DFFF
  std::array<uint8_t, kSampleRamSize> sample_{};
  uint8_t bank_ = 0;
  uint64_t cycles_ = 0;
  uint32_t credit_ = 0;  // cycles executed beyond what was requested
  Cpu6809 cpu_;
};

}  // namespace mirage
