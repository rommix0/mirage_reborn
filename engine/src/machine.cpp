#include "mirage/machine.h"

#include <algorithm>
#include <stdexcept>

#include "mirage/clocks.h"
#include "mirage/state_io.h"

namespace mirage {

namespace {
constexpr uint16_t kSampleWindowEnd = 0x8000;  // $0000-$7FFF
constexpr uint16_t kRamBase = 0x8000, kExpBase = 0xC000, kIoBase = 0xE000, kRomBase = 0xF000;
constexpr uint32_t kQuarter = 0x8000;
// E cycles per MIDI byte on the wire (640 at 2 MHz).
constexpr uint64_t kMidiByteCycles = uint64_t(kMidiByteSeconds * kCpuClockHz + 0.5);
// A save file with more queued MIDI bytes than this is treated as corrupt.
constexpr uint32_t kMaxMidiQueue = 1u << 24;
}  // namespace

Machine::Machine(std::span<const uint8_t> bootRom) : cpu_(*this) {
  if (bootRom.size() != kBootRomSize)
    throw std::invalid_argument("boot ROM must be exactly 4096 bytes");
  std::copy(bootRom.begin(), bootRom.end(), rom_.begin());

  // Port A: PA0-2 column/segment, PA3/PA4 digit enables (outputs), PA5-7 key
  // rows (inputs).
  via_.portAPins = [this] { return uint8_t(panel_.rowSense(via_.outA()) | (via_.outA() & 0x1F)); };
  via_.portAOut = [this](uint8_t a) { panel_.portAWritten(a, cycles_); };
  // Port B: PB0/PB1 select the sample RAM quarter. PB5 (DOC sync) and PB6
  // (disk present, 1 = disk in) read 0 until the DOC and drive exist. PB2-4
  // and PB7 are wired by later tasks.
  via_.portBPins = [] { return uint8_t(0); };
  via_.portBOut = [this](uint8_t b) { bank_ = uint8_t(b & 3); };
  acia_.onTransmit = [this](uint8_t b) {
    if (onMidiOut) onMidiOut(b);
  };
}

void Machine::powerOn() {
  ram_.fill(0);
  exp_.fill(0);
  sample_.fill(0);
  bank_ = 0;
  cycles_ = 0;
  credit_ = 0;
  via_.reset();
  acia_.reset();
  midiIn_.clear();
  nextMidiCycle_ = 0;
  updateFirq();
  panel_.clearDisplay();
  cpu_.powerOn();
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
  if (addr >= 0xE100 && addr <= 0xE101) {  // 6850 ACIA
    const uint8_t v = acia_.read(uint8_t(addr & 1));
    updateFirq();  // a data read clears RDRF/OVRN
    return v;
  }
  if (addr >= 0xE200 && addr <= 0xE2FF) {  // 6522 VIA, register = addr & 15
    const uint8_t v = via_.read(uint8_t(addr & 15));
    updateIrq();  // reading T1C-L, T2C-L, ORx or SR clears flags
    return v;
  }
  if (addr >= 0xE400 && addr <= 0xE41F) return 0xFF;  // filter CV latches (write-only)
  if (addr >= 0xE800 && addr <= 0xE803) return 0xFF;  // WD1772
  if (addr >= 0xEC00 && addr <= 0xECEF) return 0xFF;  // DOC 5503
  return 0xFF;
}

void Machine::ioWrite(uint16_t addr, [[maybe_unused]] uint8_t value) {
  if (addr >= 0xE100 && addr <= 0xE101) {        // 6850 ACIA
    acia_.write(uint8_t(addr & 1), value);
    updateFirq();
    return;
  }
  if (addr >= 0xE200 && addr <= 0xE2FF) {        // 6522 VIA
    via_.write(uint8_t(addr & 15), value);
    updateIrq();
    return;
  }
  if (addr >= 0xE400 && addr <= 0xE41F) return;  // filter CV latches
  if (addr >= 0xE800 && addr <= 0xE803) return;  // WD1772
  if (addr >= 0xEC00 && addr <= 0xECEF) return;  // DOC 5503
}

void Machine::tickDevices(uint32_t cycles) {
  via_.tick(cycles);  // Phi2 = the CPU E clock
  updateIrq();
  // cycles_ already includes this step. One tick is a single instruction, far
  // shorter than a MIDI byte, so at most one byte falls due.
  if (!midiIn_.empty() && cycles_ >= nextMidiCycle_) {
    acia_.receiveByte(midiIn_.front());
    midiIn_.pop_front();
    nextMidiCycle_ += kMidiByteCycles;
    updateFirq();
  }
}

void Machine::queueMidi(std::span<const uint8_t> bytes) {
  if (bytes.empty()) return;
  // Into an idle line: the first byte finishes one byte time from now.
  if (midiIn_.empty()) nextMidiCycle_ = cycles_ + kMidiByteCycles;
  midiIn_.insert(midiIn_.end(), bytes.begin(), bytes.end());
}

void Machine::updateIrq() {
  cpu_.setIrq(via_.irq());  // | WD1772 DRQ | DOC IRQ (Tasks 8 and 10)
}

void Machine::updateFirq() { cpu_.setFirq(acia_.irq()); }

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
  via_.save(w);
  acia_.save(w);
  w.u64(nextMidiCycle_);
  w.u32(uint32_t(midiIn_.size()));
  for (uint8_t b : midiIn_) w.u8(b);
  panel_.save(w);
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
  // Each device load commits only on success, but a later one can still fail
  // (or trailing data be found), so the devices are put back from a snapshot.
  StateWriter before;
  cpu_.save(before);
  via_.save(before);
  panel_.save(before);
  acia_.save(before);
  uint64_t nextMidi = 0;
  std::deque<uint8_t> midi;
  try {
    cpu_.load(r);
    via_.load(r);
    acia_.load(r);
    nextMidi = r.u64();
    const uint32_t n = r.u32();
    if (n > kMaxMidiQueue) throw StateError("machine: bad MIDI queue");
    for (uint32_t i = 0; i < n; ++i) midi.push_back(r.u8());  // a short file throws here
    panel_.load(r);
    if (!r.atEnd()) throw StateError("machine: trailing data");
  } catch (...) {
    const auto old = before.take();
    StateReader back(old);
    cpu_.load(back);
    via_.load(back);
    panel_.load(back);
    acia_.load(back);
    throw;
  }
  bank_ = bank;
  cycles_ = cycles;
  credit_ = credit;
  ram_ = ram;
  exp_ = exp;
  sample_ = sample;
  nextMidiCycle_ = nextMidi;
  midiIn_ = std::move(midi);
  updateIrq();
  updateFirq();
}

}  // namespace mirage
