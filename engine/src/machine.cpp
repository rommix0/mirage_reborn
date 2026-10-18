#include "mirage/machine.h"

#include <algorithm>
#include <stdexcept>

#include "mirage/state_io.h"

namespace mirage {

namespace {
constexpr uint16_t kSampleWindowEnd = 0x8000;  // $0000-$7FFF
constexpr uint16_t kRamBase = 0x8000, kExpBase = 0xC000, kIoBase = 0xE000, kRomBase = 0xF000;
constexpr uint32_t kQuarter = 0x8000;
}  // namespace

Machine::Machine(std::span<const uint8_t> bootRom) : cpu_(*this) {
  if (bootRom.size() != kBootRomSize)
    throw std::invalid_argument("boot ROM must be exactly 4096 bytes");
  std::copy(bootRom.begin(), bootRom.end(), rom_.begin());
}

void Machine::powerOn() {
  ram_.fill(0);
  exp_.fill(0);
  sample_.fill(0);
  bank_ = 0;
  cycles_ = 0;
  credit_ = 0;
  cpu_.regs() = Regs{};
  cpu_.reset();
}

void Machine::runCycles(uint64_t eCycles) {
  if (credit_ >= eCycles) {
    credit_ -= uint32_t(eCycles);
    return;
  }
  eCycles -= credit_;
  credit_ = 0;
  uint64_t done = 0;
  while (done < eCycles) {
    const int c = cpu_.step();
    cycles_ += uint64_t(c);
    done += uint64_t(c);
    tickDevices(uint32_t(c));
  }
  credit_ = uint32_t(done - eCycles);
}

uint8_t Machine::peek(uint16_t addr) {
  if (addr >= kIoBase && addr < kRomBase) return 0xFF;
  return read(addr);
}

uint8_t Machine::read(uint16_t addr) {
  if (addr < kSampleWindowEnd) return sample_[bank_ * kQuarter + addr];
  if (addr < kExpBase) return ram_[addr - kRamBase];
  if (addr < kIoBase) return exp_[addr - kExpBase];
  if (addr < kRomBase) return ioRead(addr);
  return rom_[addr - kRomBase];
}

void Machine::write(uint16_t addr, uint8_t value) {
  if (addr < kSampleWindowEnd) sample_[bank_ * kQuarter + addr] = value;
  else if (addr < kExpBase) ram_[addr - kRamBase] = value;
  else if (addr < kIoBase) exp_[addr - kExpBase] = value;
  else if (addr < kRomBase) ioWrite(addr, value);
  // $F000-$FFFF is ROM: writes are ignored.
}

// I/O decode for $E000-$EFFF. Each branch is where a later task plugs in its
// device; unmapped addresses read $FF.
uint8_t Machine::ioRead(uint16_t addr) {
  if (addr >= 0xE100 && addr <= 0xE101) return 0xFF;  // 6850 ACIA
  if (addr >= 0xE200 && addr <= 0xE2FF) return 0xFF;  // 6522 VIA, register = addr & 15
  if (addr >= 0xE400 && addr <= 0xE41F) return 0xFF;  // filter CV latches (write-only)
  if (addr >= 0xE800 && addr <= 0xE803) return 0xFF;  // WD1772
  if (addr >= 0xEC00 && addr <= 0xECEF) return 0xFF;  // DOC 5503
  return 0xFF;
}

void Machine::ioWrite(uint16_t addr, [[maybe_unused]] uint8_t value) {
  if (addr >= 0xE100 && addr <= 0xE101) return;  // 6850 ACIA
  if (addr >= 0xE200 && addr <= 0xE2FF) return;  // 6522 VIA
  if (addr >= 0xE400 && addr <= 0xE41F) return;  // filter CV latches
  if (addr >= 0xE800 && addr <= 0xE803) return;  // WD1772
  if (addr >= 0xEC00 && addr <= 0xECEF) return;  // DOC 5503
}

void Machine::tickDevices([[maybe_unused]] uint32_t cycles) {}

std::vector<uint8_t> Machine::saveState() const {
  StateWriter w;
  w.tag(fourcc("MCHN"));
  w.u8(bank_);
  w.u64(cycles_);
  w.u32(credit_);
  w.bytes(ram_);
  w.bytes(exp_);
  w.bytes(sample_);
  cpu_.save(w);
  return w.take();
}

void Machine::loadState(std::span<const uint8_t> data) {
  StateReader r(data);
  r.tag(fourcc("MCHN"));
  const uint8_t bank = r.u8();
  if (bank > 3) throw StateError("machine: bad sample bank");
  const uint64_t cycles = r.u64();
  const uint32_t credit = r.u32();
  auto ram = ram_;
  auto exp = exp_;
  auto sample = sample_;
  r.bytes(ram);
  r.bytes(exp);
  r.bytes(sample);
  // Cpu6809::load commits only on success, so it goes last. If trailing data
  // is then found, the CPU is put back from a snapshot taken beforehand.
  StateWriter before;
  cpu_.save(before);
  cpu_.load(r);
  if (!r.atEnd()) {
    const auto old = before.take();
    StateReader back(old);
    cpu_.load(back);
    throw StateError("machine: trailing data");
  }
  bank_ = bank;
  cycles_ = cycles;
  credit_ = credit;
  ram_ = ram;
  exp_ = exp;
  sample_ = sample;
}

}  // namespace mirage
