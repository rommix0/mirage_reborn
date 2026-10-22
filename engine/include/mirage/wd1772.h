#pragma once
#include <array>
#include <cstdint>

#include "mirage/clocks.h"
#include "mirage/disk_image.h"
#include "mirage/state_io.h"

namespace mirage {

// Western Digital WD1772 floppy controller driving one single-sided Mirage
// drive. Register select: 0 status/command, 1 track, 2 sector, 3 data.
//
// Modelled (docs/hardware/fdc.md has the timing model and the choices made):
//   Type I   Restore, Seek, Step, Step In, Step Out (6/12/2/3 ms, verify, 6-index
//            spin-up unless h=1; every command sets MO)
//   Type II  Read Sector, Write Sector, single and multi-sector (m), E settle
//   Type III Read Address, Write Track (one revolution, index to index, parsed
//            into a formatted or unformatted track); Read Track completes at
//            once with no data and no error
//   Type IV  Force Interrupt ($D0 terminate, $D8 immediate, $D4 every index pulse)
// Time is counted in E cycles. The disk turns (and index pulses occur) only while
// the drive motor is on AND a disk is present; with no rotation the chip waits,
// BUSY set, exactly as the datasheet's index-counted time-outs do.
class Wd1772 {
 public:
  uint8_t read(uint8_t reg);  // reading status clears INTRQ (except after $D8)
  void write(uint8_t reg, uint8_t v);
  void tick(uint32_t eCycles);

  bool intrq() const { return intrq_; }  // -> CPU NMI
  bool drq() const { return drq_; }      // -> CPU IRQ
  bool busy() const { return busy_; }

  // nullptr = empty drive. Replacing or removing the disk aborts a Type II/III
  // command (or a Type I verify) with Record Not Found / Seek Error, raising
  // INTRQ; it never throws. The disk is not owned.
  void setDisk(DiskImage*);
  // Re-points at the disk without flagging a change or aborting anything; for
  // state restore, where the command in flight was saved with the controller.
  void attachDisk(DiskImage* d) { disk_ = d; }
  // Drive select/motor (the Mirage's VIA PB4, active low, inverted by Machine).
  void setMotor(bool on) { motor_ = on; }
  bool motor() const { return motor_; }
  bool diskChanged() const { return diskChanged_; }  // set by any disk insert/eject
  void ackDiskChange() { diskChanged_ = false; }

  uint64_t sectorsRead() const { return sectorsRead_; }  // completed Read Sector transfers
  int headTrack() const { return head_; }                // physical head position

  // Chip reset: registers, command state, head to track 0, rotation to the
  // index. The inserted disk and the disk-change latch are left alone.
  void reset();

  void save(StateWriter&) const;
  void load(StateReader&);  // commits only on success

 private:
  enum class Phase : uint8_t {
    Idle,
    Start,       // command latency
    SpinUp,      // counting index pulses (needs rotation)
    Settle,      // head settle after E / V
    StepWait,    // one step time
    SearchWait,  // rotating to the matching ID field
    SearchFail,  // no ID can match: counting index pulses to the time-out
    VerifyId,    // Type I verify reading the ID field
    Stream,      // delivering read bytes
    StreamEnd,   // CRC bytes after the last read byte
    WriteId,     // ID field passing; DRQ for the first byte follows
    WriteGap,    // 22-byte gap in which the first byte must arrive
    WriteStream, // consuming write bytes
    WriteEnd,    // last byte, CRC, trailing $FF
    TrackWaitIndex,  // Write Track: DRQ up, waiting for the index pulse
    TrackStream,     // Write Track: one byte per byte time to the next index pulse
    Last = TrackStream,
  };
  enum class Kind : uint8_t {
    Restore, Seek, Step, StepIn, StepOut, ReadSector, WriteSector, ReadAddress, ReadTrack,
    WriteTrack
  };

  Kind kind() const;
  bool typeI() const { return cmd_ < 0x80; }
  bool spinning() const { return motor_ && disk_ != nullptr; }
  uint8_t status() const;
  void command(uint8_t v);
  void forceInterrupt(uint8_t v);
  void complete();
  void fail();  // sets status bit 4 (RNF / seek error) and completes
  void enter(Phase p, uint32_t wait, bool needSpin);
  void onWaitDone();
  void onIndex();
  void afterStart();
  void afterSpinUp();
  void startTypeI();
  void restoreStep();
  void seekStep();
  void stepPulse();
  void finishSteps();
  void startBody();
  void startSearch();
  void searchHit();
  void streamByte();
  void streamEnd();
  void writeByte();
  void writeEnd();
  void trackByte();
  void writeTrackEnd();
  uint32_t delayToSlot(int slot) const;
  void idField(int slot, uint8_t out[6]) const;

  DiskImage* disk_ = nullptr;  // not owned, not saved
  bool diskChanged_ = false;

  // Registers and status.
  uint8_t trackReg_ = 0, sectorReg_ = 0, dataReg_ = 0, cmd_ = 0;
  uint8_t errBits_ = 0;  // status bits 6, 4, 3
  bool lost_ = false, su_ = false, chipMotor_ = false;
  bool intrq_ = false, drq_ = false, busy_ = false;
  bool forcedIrq_ = false, forceOnIndex_ = false;
  bool statI_ = true;  // status register shows Type I bits (true when idle)

  // Mechanics.
  bool motor_ = false, dirOut_ = false;
  int head_ = 0;
  uint32_t rot_ = 0;  // E cycles since the last index pulse

  // Command sequencing.
  Phase phase_ = Phase::Idle;
  uint32_t wait_ = 0;
  bool needSpin_ = false;
  uint8_t spinCount_ = 0, indexCount_ = 0, idleIndex_ = 0, stepCount_ = 0;
  uint8_t slot_ = 0;
  uint16_t idx_ = 0, len_ = 0;
  std::array<uint8_t, 1024> buf_{};
  // Write Track: the bytes the CPU loaded since the index pulse (late ones as
  // $00), $F5-$F7 not yet interpreted. At most one per byte time.
  std::array<uint8_t, kDiskIndexCycles / kFdcByteCycles> trackBuf_{};
  uint16_t trackLen_ = 0;
  uint64_t sectorsRead_ = 0;
};

}  // namespace mirage
