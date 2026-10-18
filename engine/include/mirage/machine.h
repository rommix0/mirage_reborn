#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "mirage/acia6850.h"
#include "mirage/bus.h"
#include "mirage/cpu6809.h"
#include "mirage/disk_image.h"
#include "mirage/panel.h"
#include "mirage/via6522.h"
#include "mirage/wd1772.h"

namespace mirage {

// The whole Mirage: 6809 + RAM + banked sample RAM + boot ROM + I/O decode.
// The VIA, ACIA (MIDI), WD1772 floppy controller and front panel are wired in; the
// other I/O devices are stubs for now (reads $FF, writes ignored); later tasks plug
// the DOC and filter chips into ioRead/ioWrite/tickDevices.
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

  // Front panel. Held keys survive powerOn() like a finger on a button.
  void setKey(PanelKey key, bool down) { panel_.setKey(key, down); }
  DisplayState display() const { return panel_.display(cycles_); }

  // MIDI. Queued bytes reach the ACIA one per MIDI byte time (320 us) of
  // emulated time, whatever the ACIA's divide setting. The queue is unbounded
  // (host side); an OS that reads too slowly gets OVRN as on hardware. powerOn()
  // discards queued bytes. Bytes the OS transmits are passed to onMidiOut.
  void queueMidi(std::span<const uint8_t> bytes);
  std::function<void(uint8_t)> onMidiOut;

  // Floppy drive. insertDisk replaces any disk already in the drive (aborting a
  // command in flight, as an eject does); ejectDisk hands the disk back. VIA PB6
  // reads 1 while a disk is in. powerOn() never touches the disk. driveActive()
  // is true while the motor is on (VIA PB4 low).
  void insertDisk(DiskImage disk);
  std::optional<DiskImage> ejectDisk();
  DiskImage* disk() { return disk_ ? &*disk_ : nullptr; }
  bool driveActive() const { return wd_.motor(); }
  Wd1772& fdc() { return wd_; }  // diagnostics: sectorsRead(), diskChanged()

  // The saved state includes the FDC and the inserted disk (or "no disk").
  std::vector<uint8_t> saveState() const;
  void loadState(std::span<const uint8_t> data);  // throws StateError; unchanged on failure

 private:
  uint8_t read(uint16_t addr) override;
  void write(uint16_t addr, uint8_t value) override;
  uint8_t ioRead(uint16_t addr);
  void ioWrite(uint16_t addr, uint8_t value);
  void tickDevices(uint32_t cycles);
  // The CPU IRQ line is the OR of the VIA, WD1772 DRQ and DOC IRQ outputs.
  // Call after anything that can change one of them.
  void updateIrq();
  // The CPU NMI line is the WD1772 INTRQ output (the CPU latches rising edges).
  void updateNmi();
  // The CPU FIRQ line is the ACIA IRQ output.
  void updateFirq();

  std::array<uint8_t, kBootRomSize> rom_{};
  std::array<uint8_t, 0x4000> ram_{};   // $8000-$BFFF
  std::array<uint8_t, 0x2000> exp_{};   // $C000-$DFFF
  std::array<uint8_t, kSampleRamSize> sample_{};
  uint8_t bank_ = 0;  // VIA PB1:PB0
  uint64_t cycles_ = 0;
  uint32_t credit_ = 0;  // cycles executed beyond what was requested
  Cpu6809 cpu_;
  Via6522 via_;
  Panel panel_;
  Acia6850 acia_;
  Wd1772 wd_;
  std::optional<DiskImage> disk_;  // wd_ points at it; the Machine is not movable
  std::deque<uint8_t> midiIn_;
  uint64_t nextMidiCycle_ = 0;  // cycles() at which the next queued byte arrives
};

}  // namespace mirage
