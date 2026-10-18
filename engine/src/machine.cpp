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
// A DOC channel sum (byte - 128) * volume as a voltage: one full-volume
// oscillator swings about +-1.0.
constexpr float kVoiceScale = 1.0f / 32768.0f;
// The line input has nothing connected (Plan 1): the A/D reads mid-scale.
constexpr uint8_t kLineInputIdle = 0x80;
}  // namespace

Machine::Machine(std::span<const uint8_t> bootRom)
    : cpu_(*this), doc_([this](uint32_t addr17) { return sample_[addr17 & (kSampleRamSize - 1)]; }) {
  if (bootRom.size() != kBootRomSize)
    throw std::invalid_argument("boot ROM must be exactly 4096 bytes");
  std::copy(bootRom.begin(), bootRom.end(), rom_.begin());

  // Port A: PA0-2 column/segment, PA3/PA4 digit enables (outputs), PA5-7 key
  // rows (inputs).
  via_.portAPins = [this] { return uint8_t(panel_.rowSense(via_.outA()) | (via_.outA() & 0x1F)); };
  via_.portAOut = [this](uint8_t a) { panel_.portAWritten(a, cycles_); };
  // Port B: PB0/PB1 select the sample RAM quarter. PB4 is the drive select/motor
  // line, active low (boot-rom.md 2.1). PB6 reads 1 while a disk is in the drive
  // (a level, not a change latch); PB5 (DOC sync) reads 0 until the DOC exists.
  // PB2-3 and PB7 are wired by later tasks.
  via_.portBPins = [this] { return uint8_t(disk_ ? 0x40 : 0x00); };
  via_.portBOut = [this](uint8_t b) {
    bank_ = uint8_t(b & 3);
    wd_.setMotor((b & 0x10) == 0);
  };
  acia_.onTransmit = [this](uint8_t b) {
    if (onMidiOut) onMidiOut(b);
  };
  // Each DOC scan runs the voice filters in emulated time, so an A/D
  // conversion sees the analog mix of that instant (filters.md 1).
  doc_.onFrame = [this](const std::array<int32_t, Doc5503::kChannels>& frame) {
    std::array<float, FilterBank::kVoices> in;
    const int32_t* sums = frame.data();
    float* v = in.data();
    for (int ch = 0; ch < FilterBank::kVoices; ++ch) v[ch] = float(sums[ch]) * kVoiceScale;
    float mix = 0.0f;
    filters_.process(in, doc_.sampleRate(), mix);
    if (onMix) onMix(mix);
  };
  // DOC A/D input: the analog mux on VIA PB3:PB2 (boot-rom.md 2.1, filters.md 3).
  doc_.adcRead = [this] {
    switch ((via_.outB() >> 2) & 3) {
      case 0: return compressedAdcCode(filters_.mix());  // voices + audio in (silent)
      case 1: return kLineInputIdle;
      case 2: return kPitchWheelRest;
      default: return kModWheelRest;
    }
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
  wd_.reset();  // the disk stays in the drive
  doc_.reset();
  filters_.reset();
  masterRem_ = 0;
  midiIn_.clear();
  nextMidiCycle_ = 0;
  updateFirq();
  panel_.clearDisplay();
  cpu_.powerOn();
  updateIrq();
  updateNmi();
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
    if (trace_) [[unlikely]] {
      if (!cpu_.waiting()) trace_(cpu_.regs().pc, cpu_.regs());
    }
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
  if (addr >= 0xE400 && addr <= 0xE41F) return 0xFF;  // filter CV latches: write only
  if (addr >= 0xE800 && addr <= 0xE803) {  // WD1772
    const uint8_t v = wd_.read(uint8_t(addr & 3));
    updateIrq();  // a data read clears DRQ
    updateNmi();  // a status read clears INTRQ
    return v;
  }
  if (addr >= 0xEC00 && addr <= 0xECEF) {  // DOC 5503, register = addr & $FF
    const uint8_t v = doc_.read(uint8_t(addr));
    updateIrq();  // an OIR read pops the interrupt FIFO
    return v;
  }
  return 0xFF;
}

void Machine::ioWrite(uint16_t addr, uint8_t value) {
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
  if (addr >= 0xE400 && addr <= 0xE41F) {        // filter CV DAC and sample-and-holds
    filters_.cvWrite(uint8_t(addr & 0x1F), value);
    return;
  }
  if (addr >= 0xE800 && addr <= 0xE803) {        // WD1772
    wd_.write(uint8_t(addr & 3), value);
    updateIrq();
    updateNmi();
    return;
  }
  if (addr >= 0xEC00 && addr <= 0xECEF) {        // DOC 5503
    doc_.write(uint8_t(addr), value);
    updateIrq();
  }
}

void Machine::tickDevices(uint32_t cycles) {
  via_.tick(cycles);  // Phi2 = the CPU E clock
  wd_.tick(cycles);
  // The DOC runs on the master clock: E cycles x kMasterClockHz / kCpuClockHz,
  // remainder carried.
  const uint64_t master = masterRem_ + uint64_t(cycles) * kMasterClockHz;
  doc_.tickMasterClocks(uint32_t(master / kCpuClockHz));
  masterRem_ = uint32_t(master % kCpuClockHz);
  updateIrq();
  updateNmi();
  // cycles_ already includes this step. One tick is a single instruction, far
  // shorter than a MIDI byte, so at most one byte falls due.
  if (!midiIn_.empty() && cycles_ >= nextMidiCycle_) {
    acia_.receiveByte(midiIn_.front());
    midiIn_.pop_front();
    nextMidiCycle_ += kMidiByteCycles;
    updateFirq();
  }
}

void Machine::insertDisk(DiskImage disk) {
  wd_.setDisk(nullptr);  // a disk already in the drive is taken out first
  disk_.emplace(std::move(disk));
  wd_.setDisk(&*disk_);
  updateIrq();
  updateNmi();
}

std::optional<DiskImage> Machine::ejectDisk() {
  if (!disk_) return std::nullopt;
  wd_.setDisk(nullptr);  // aborts a command in flight
  std::optional<DiskImage> out = std::move(disk_);
  disk_.reset();
  updateIrq();
  updateNmi();
  return out;
}

void Machine::queueMidi(std::span<const uint8_t> bytes) {
  if (bytes.empty()) return;
  // Into an idle line: the first byte finishes one byte time from now.
  if (midiIn_.empty()) nextMidiCycle_ = cycles_ + kMidiByteCycles;
  midiIn_.insert(midiIn_.end(), bytes.begin(), bytes.end());
}

void Machine::updateIrq() {
  cpu_.setIrq(via_.irq() || wd_.drq() || doc_.irq());
}

void Machine::updateNmi() { cpu_.setNmi(wd_.intrq()); }

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
  doc_.save(w);
  w.u32(masterRem_);
  filters_.save(w);
  wd_.save(w);
  w.u8(disk_ ? 1 : 0);
  if (disk_) disk_->save(w);
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
  wd_.save(before);
  doc_.save(before);
  filters_.save(before);
  uint64_t nextMidi = 0;
  uint32_t masterRem = 0;
  std::deque<uint8_t> midi;
  std::optional<DiskImage> disk;
  try {
    cpu_.load(r);
    via_.load(r);
    doc_.load(r);
    masterRem = r.u32();
    if (masterRem >= kCpuClockHz) throw StateError("machine: bad DOC clock remainder");
    filters_.load(r);
    wd_.load(r);
    const uint8_t hasDisk = r.u8();
    if (hasDisk > 1) throw StateError("machine: bad disk flag");
    if (hasDisk) disk.emplace().load(r);
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
    wd_.load(back);
    doc_.load(back);
    filters_.load(back);
    throw;
  }
  bank_ = bank;
  cycles_ = cycles;
  credit_ = credit;
  ram_ = ram;
  exp_ = exp;
  sample_ = sample;
  nextMidiCycle_ = nextMidi;
  masterRem_ = masterRem;
  midiIn_ = std::move(midi);
  disk_ = std::move(disk);
  wd_.attachDisk(disk_ ? &*disk_ : nullptr);
  updateIrq();
  updateNmi();
  updateFirq();
}

}  // namespace mirage
