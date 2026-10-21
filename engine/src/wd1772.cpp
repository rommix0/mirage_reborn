#include "mirage/wd1772.h"

#include <algorithm>
#include <iterator>
#include <span>

#include "mirage/clocks.h"

namespace mirage {

namespace {

constexpr uint32_t kStateTag = fourcc("WD17");

constexpr uint32_t kB = kFdcByteCycles;  // one data byte time
constexpr uint32_t kMs = kCpuClockHz / 1000;

constexpr uint32_t kStartCycles = 32;        // command latency
constexpr uint32_t kSettleCycles = 15 * kMs;  // 1772 head settle, E and V
constexpr uint32_t kStepCycles[4] = {6 * kMs, 12 * kMs, 2 * kMs, 3 * kMs};  // r1 r0
constexpr uint32_t kIndexPulseCycles = 2 * kMs;  // how long the index pulse reads as high

constexpr int kSlots = 6;      // sector ids 0-5 around the track
constexpr int kMaxHead = 83;   // mechanical stop
constexpr int kRestoreSteps = 255;
constexpr uint8_t kSpinUpPulses = 6, kMaxRevs = 5, kIdleRevs = 9;

// Bytes within a sector slot (the slot starts at the first ID address mark byte).
constexpr uint32_t kIdBytes = 10;       // A1 A1 A1 FE, track side sector size, CRC1 CRC2
constexpr uint32_t kWriteGapBytes = 22;  // gap 2: first write byte must arrive in this time
constexpr uint32_t kPreDataBytes = 16;   // 12 x $00, A1 A1 A1 FB
constexpr uint32_t kFirstReadByte = kIdBytes + kWriteGapBytes + kPreDataBytes + 1;
constexpr uint32_t kAddressFirstByte = 5;  // after A1 A1 A1 FE and the track byte

// Track layout in byte times. One revolution is 6250 bytes. Slot k's ID field
// starts at the cumulative offset of everything before it, so the gap after a
// sector is a gap 3 of 50 bytes. (The first model spaced slots 1/6 of a
// revolution apart, 1041 bytes, which is less than a 1024-byte sector's 1074
// bytes: the next sector's ID had always just passed, so every back-to-back
// read cost a whole extra revolution.) This is a model choice; the real
// Mirage format's gap lengths are not documented. See fdc.md.
constexpr uint32_t kTrackBytes = kDiskIndexCycles / kFdcByteCycles;  // 6250
constexpr uint32_t kGap1 = 16;  // index to the first ID field
constexpr uint32_t slotBytes(int id) {  // ID + gap 2 + sync/DAM + data + CRC
  return kIdBytes + kWriteGapBytes + kPreDataBytes + (id < 5 ? 1024u : 512u) + 2;
}
constexpr uint32_t kGap3 = (kTrackBytes - kGap1 - 5 * slotBytes(0) - slotBytes(5)) / 6;  // 50
constexpr uint32_t slotStart(int k) {  // in cycles from the index pulse
  uint32_t bytes = kGap1;
  for (int j = 0; j < k; ++j) bytes += slotBytes(j) + kGap3;
  return bytes * kFdcByteCycles;
}
static_assert(kGap3 == 50 && slotBytes(0) == 1074 && slotBytes(5) == 562);
static_assert(slotStart(5) + (slotBytes(5) + kGap3) * kFdcByteCycles <= kDiskIndexCycles);

// Write Track's stream holds the Mirage layout if its ID fields, F5 F5 F5 FE
// t s n z F7, are exactly ids 0-5 once each, all with t = `track`, s = 0 and
// size codes 3 (ids 0-4) and 2 (id 5), and each is followed, before the next
// ID field, by a data field F5 F5 F5 FB <data> F7. The data land in `out` in
// the raw image's sector order (id n at n x 1024). See fdc.md.
bool parseMirageTrack(std::span<const uint8_t> s, int track,
                      std::span<uint8_t, DiskImage::kTrackBytes> out) {
  const size_t n = s.size();
  const auto mark = [&](size_t i, uint8_t am) {
    return i + 4 <= n && s[i] == 0xF5 && s[i + 1] == 0xF5 && s[i + 2] == 0xF5 && s[i + 3] == am;
  };
  bool seen[kSlots] = {};
  int found = 0;
  size_t i = 0;
  while (i < n) {
    if (!mark(i, 0xFE)) {
      ++i;
      continue;
    }
    i += 4;
    if (i + 5 > n || s[i + 4] != 0xF7) return false;
    const uint8_t id = s[i + 2];
    if (s[i] != track || s[i + 1] != 0 || id >= kSlots || seen[id] || s[i + 3] != (id < 5 ? 3 : 2))
      return false;
    seen[id] = true;
    ++found;
    i += 5;
    while (i < n && !mark(i, 0xFE) && !mark(i, 0xFB)) ++i;
    if (!mark(i, 0xFB)) return false;
    i += 4;
    const size_t size = id < 5 ? 1024 : 512;
    if (i + size >= n || s[i + size] != 0xF7) return false;
    std::copy_n(s.begin() + ptrdiff_t(i), size, out.begin() + ptrdiff_t(id) * 1024);
    i += size + 1;
  }
  return found == kSlots;
}

uint16_t crcCcitt(const uint8_t* p, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; ++i) {
    crc = uint16_t(crc ^ (p[i] << 8));
    for (int b = 0; b < 8; ++b) crc = uint16_t((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
  }
  return crc;
}

}  // namespace

Wd1772::Kind Wd1772::kind() const {
  switch (cmd_ >> 4) {
    case 0x0: return Kind::Restore;
    case 0x1: return Kind::Seek;
    case 0x2: case 0x3: return Kind::Step;
    case 0x4: case 0x5: return Kind::StepIn;
    case 0x6: case 0x7: return Kind::StepOut;
    case 0x8: case 0x9: return Kind::ReadSector;
    case 0xA: case 0xB: return Kind::WriteSector;
    case 0xC: return Kind::ReadAddress;
    case 0xE: return Kind::ReadTrack;
    default: return Kind::WriteTrack;  // 0xF ($D is Force Interrupt, never stored)
  }
}

void Wd1772::reset() {
  DiskImage* d = disk_;
  const bool changed = diskChanged_;
  *this = Wd1772{};
  disk_ = d;
  diskChanged_ = changed;
}

uint8_t Wd1772::status() const {
  uint8_t s = chipMotor_ ? 0x80 : 0;
  if (statI_) {
    if (su_) s |= 0x20;
    s |= uint8_t(errBits_ & 0x18);  // seek error, CRC
    if (head_ == 0) s |= 0x04;      // track 0 (set when the head is at track 0)
    if (spinning() && rot_ < kIndexPulseCycles) s |= 0x02;
  } else {
    s |= uint8_t(errBits_ & 0x58);  // write protect, RNF, CRC
    if (lost_) s |= 0x04;
    if (drq_) s |= 0x02;
  }
  if (busy_) s |= 0x01;
  return s;
}

uint8_t Wd1772::read(uint8_t reg) {
  switch (reg & 3) {
    case 0: {
      const uint8_t s = status();
      if (!forcedIrq_) intrq_ = false;
      return s;
    }
    case 1: return trackReg_;
    case 2: return sectorReg_;
    default: drq_ = false; return dataReg_;
  }
}

void Wd1772::write(uint8_t reg, uint8_t v) {
  switch (reg & 3) {
    case 0: command(v); break;
    case 1: if (!busy_) trackReg_ = v; break;
    case 2: if (!busy_) sectorReg_ = v; break;
    default: dataReg_ = v; drq_ = false; break;
  }
}

void Wd1772::command(uint8_t v) {
  if ((v & 0xF0) == 0xD0) {
    forceInterrupt(v);
    return;
  }
  if (busy_) return;  // a busy chip ignores everything but Force Interrupt
  intrq_ = false;
  forcedIrq_ = false;
  forceOnIndex_ = false;
  cmd_ = v;
  busy_ = true;
  drq_ = false;
  errBits_ = 0;
  lost_ = false;
  su_ = false;
  statI_ = typeI();
  idleIndex_ = 0;
  stepCount_ = 0;
  enter(Phase::Start, kStartCycles, false);
}

void Wd1772::forceInterrupt(uint8_t v) {
  if (busy_) {
    // Terminates the command; the status bits (apart from busy) stay as they are.
    busy_ = false;
    phase_ = Phase::Idle;
    wait_ = 0;
    needSpin_ = false;
  } else {
    // Idle: the status register is refreshed as after a Type I command.
    statI_ = true;
    errBits_ = 0;
    lost_ = false;
  }
  drq_ = false;
  forceOnIndex_ = (v & 4) != 0;
  if (v & 8) {
    intrq_ = true;
    forcedIrq_ = true;  // stays up until $D0 / the next command
  } else {
    intrq_ = false;
    forcedIrq_ = false;
  }
}

void Wd1772::complete() {
  busy_ = false;
  drq_ = false;
  phase_ = Phase::Idle;
  wait_ = 0;
  needSpin_ = false;
  intrq_ = true;
  idleIndex_ = 0;
}

void Wd1772::fail() {
  errBits_ |= 0x10;
  complete();
}

void Wd1772::setDisk(DiskImage* d) {
  if (d == disk_) return;
  if (disk_ != nullptr && busy_) {
    // The disk left under a running command: it can no longer find its sector.
    if (!typeI()) fail();
    else if (phase_ == Phase::SearchWait || phase_ == Phase::SearchFail ||
             phase_ == Phase::VerifyId)
      fail();
  }
  disk_ = d;
  diskChanged_ = true;
  // A search that had nothing to look at tries again on the new disk.
  if (busy_ && phase_ == Phase::SearchFail) startSearch();
}

void Wd1772::enter(Phase p, uint32_t wait, bool needSpin) {
  phase_ = p;
  wait_ = wait;
  needSpin_ = needSpin;
  // SpinUp, SearchFail and TrackWaitIndex end on index pulses, not on a timer.
  if (wait == 0 && p != Phase::SpinUp && p != Phase::SearchFail && p != Phase::TrackWaitIndex &&
      p != Phase::Idle)
    onWaitDone();
}

void Wd1772::tick(uint32_t n) {
  while (n > 0) {
    const bool spin = spinning();
    uint32_t step = n;
    if (spin) step = std::min(step, kDiskIndexCycles - rot_);
    // A timer that needs the disk turning stalls while it does not.
    const bool counting = wait_ > 0 && (spin || !needSpin_);
    if (counting) step = std::min(step, wait_);
    n -= step;
    bool waitDone = false, indexHit = false;
    if (counting) {
      wait_ -= step;
      waitDone = wait_ == 0;
    }
    if (spin) {
      rot_ += step;
      if (rot_ >= kDiskIndexCycles) {
        rot_ -= kDiskIndexCycles;
        indexHit = true;
      }
    }
    if (indexHit) onIndex();
    if (waitDone) onWaitDone();
  }
}

void Wd1772::onIndex() {
  if (forceOnIndex_) {
    forceOnIndex_ = false;
    intrq_ = true;
    forcedIrq_ = true;
  }
  switch (phase_) {
    case Phase::Idle:
      // The chip turns its motor line off after 9 idle revolutions.
      if (chipMotor_ && ++idleIndex_ >= kIdleRevs) {
        chipMotor_ = false;
        idleIndex_ = 0;
      }
      break;
    case Phase::SpinUp:
      if (++spinCount_ >= kSpinUpPulses) afterSpinUp();
      break;
    case Phase::SearchFail:
      if (++indexCount_ >= kMaxRevs) fail();
      break;
    case Phase::TrackWaitIndex: trackByte(); break;  // writing starts at the index pulse
    case Phase::TrackStream: writeTrackEnd(); break;  // and stops at the next one
    default: break;
  }
}

void Wd1772::onWaitDone() {
  switch (phase_) {
    case Phase::Start: afterStart(); break;
    case Phase::Settle:
      if (typeI()) startSearch();
      else startBody();
      break;
    case Phase::StepWait:
      if (kind() == Kind::Restore) restoreStep();
      else if (kind() == Kind::Seek) seekStep();
      else finishSteps();
      break;
    case Phase::SearchWait: searchHit(); break;
    case Phase::VerifyId: complete(); break;
    case Phase::Stream: streamByte(); break;
    case Phase::StreamEnd: streamEnd(); break;
    case Phase::WriteId:
      drq_ = true;  // ask for the first byte
      enter(Phase::WriteGap, kWriteGapBytes * kB, true);
      break;
    case Phase::WriteGap:
      if (drq_) {
        lost_ = true;  // the CPU was too late: the chip gives up
        complete();
      } else {
        enter(Phase::WriteStream, kPreDataBytes * kB, true);
      }
      break;
    case Phase::WriteStream: writeByte(); break;
    case Phase::WriteEnd: writeEnd(); break;
    case Phase::TrackStream: trackByte(); break;
    default: break;
  }
}

void Wd1772::afterStart() {
  if (!(cmd_ & 8) && !chipMotor_) {
    // h=0 and the motor line is off: switch it on and wait 6 index pulses.
    chipMotor_ = true;
    spinCount_ = 0;
    enter(Phase::SpinUp, 0, true);
    return;
  }
  afterSpinUp();
}

void Wd1772::afterSpinUp() {
  su_ = true;
  if (typeI()) {
    startTypeI();
  } else if (cmd_ & 4) {  // E: head settle
    enter(Phase::Settle, kSettleCycles, false);
  } else {
    startBody();
  }
}

void Wd1772::startTypeI() {
  switch (kind()) {
    case Kind::Restore: restoreStep(); break;
    case Kind::Seek: seekStep(); break;
    default: stepPulse(); break;
  }
}

void Wd1772::stepPulse() {
  const Kind k = kind();
  if (k == Kind::StepIn) dirOut_ = false;
  else if (k == Kind::StepOut) dirOut_ = true;
  if (cmd_ & 0x10) trackReg_ = uint8_t(trackReg_ + (dirOut_ ? -1 : 1));  // u
  head_ = std::clamp(head_ + (dirOut_ ? -1 : 1), 0, kMaxHead);
  enter(Phase::StepWait, kStepCycles[cmd_ & 3], false);
}

void Wd1772::restoreStep() {
  if (head_ == 0) {
    trackReg_ = 0;
    finishSteps();
  } else if (stepCount_ >= kRestoreSteps) {
    // TR00 never came: a Seek Error with verify, otherwise the command just ends.
    if (cmd_ & 4) fail();
    else complete();
  } else {
    ++stepCount_;
    dirOut_ = true;
    head_ -= 1;
    enter(Phase::StepWait, kStepCycles[cmd_ & 3], false);
  }
}

void Wd1772::seekStep() {
  if (trackReg_ == dataReg_) {
    finishSteps();
    return;
  }
  dirOut_ = dataReg_ < trackReg_;
  if (dirOut_ && head_ == 0) {  // TR00 stops an outward seek
    trackReg_ = 0;
    finishSteps();
    return;
  }
  trackReg_ = uint8_t(trackReg_ + (dirOut_ ? -1 : 1));
  head_ = std::clamp(head_ + (dirOut_ ? -1 : 1), 0, kMaxHead);
  enter(Phase::StepWait, kStepCycles[cmd_ & 3], false);
}

void Wd1772::finishSteps() {
  if (cmd_ & 4) enter(Phase::Settle, kSettleCycles, false);  // V: settle, then verify
  else complete();
}

void Wd1772::startBody() {
  switch (kind()) {
    case Kind::ReadSector:
    case Kind::WriteSector:
    case Kind::ReadAddress: startSearch(); break;
    case Kind::WriteTrack:
      // DRQ goes up at once; the first byte is taken at the index pulse.
      trackLen_ = 0;
      drq_ = true;
      enter(Phase::TrackWaitIndex, 0, true);
      break;
    default: complete();  // Read Track: not modelled
  }
}

// Slot k's ID field starts at slotStart(k) cycles after the index pulse.
uint32_t Wd1772::delayToSlot(int slot) const {
  const uint32_t start = slotStart(slot);
  return start >= rot_ ? start - rot_ : start + kDiskIndexCycles - rot_;
}

void Wd1772::startSearch() {
  indexCount_ = 0;
  int best = -1;
  uint32_t bestDelay = 0;
  if (disk_ != nullptr && disk_->trackFormatted(head_)) {
    for (int k = 0; k < kSlots; ++k) {
      bool match;
      switch (kind()) {
        case Kind::ReadSector:
        case Kind::WriteSector: match = k == sectorReg_ && trackReg_ == head_; break;
        case Kind::ReadAddress: match = true; break;
        default: match = trackReg_ == head_; break;  // Type I verify: any ID, track must agree
      }
      if (!match) continue;
      const uint32_t d = delayToSlot(k);
      if (best < 0 || d < bestDelay) {
        best = k;
        bestDelay = d;
      }
    }
  }
  if (best < 0) {
    enter(Phase::SearchFail, 0, true);  // wait 5 index pulses (for ever without any)
  } else {
    slot_ = uint8_t(best);
    enter(Phase::SearchWait, bestDelay, true);
  }
}

void Wd1772::idField(int slot, uint8_t out[6]) const {
  const uint8_t id[8] = {0xA1, 0xA1, 0xA1, 0xFE, uint8_t(head_), 0, uint8_t(slot),
                         uint8_t(slot < 5 ? 3 : 2)};
  const uint16_t crc = crcCcitt(id, 8);
  for (int i = 0; i < 4; ++i) out[i] = id[4 + i];
  out[4] = uint8_t(crc >> 8);
  out[5] = uint8_t(crc & 0xFF);
}

// The head is at the start of the matching ID field.
void Wd1772::searchHit() {
  switch (kind()) {
    case Kind::ReadSector:
      len_ = uint16_t(disk_ != nullptr ? disk_->sectorSize(head_, slot_) : 0);
      try {
        if (disk_ == nullptr) throw DiskImageError("no disk");
        disk_->readSector(head_, slot_, std::span<uint8_t>(buf_.data(), len_));
      } catch (const DiskImageError&) {
        fail();
        return;
      }
      idx_ = 0;
      enter(Phase::Stream, kFirstReadByte * kB, true);
      break;
    case Kind::WriteSector:
      len_ = uint16_t(disk_ != nullptr ? disk_->sectorSize(head_, slot_) : 0);
      if (len_ == 0) {
        fail();
        return;
      }
      idx_ = 0;
      enter(Phase::WriteId, kIdBytes * kB, true);
      break;
    case Kind::ReadAddress:
      idField(slot_, buf_.data());
      len_ = 6;
      idx_ = 0;
      enter(Phase::Stream, kAddressFirstByte * kB, true);
      break;
    default:  // Type I verify
      enter(Phase::VerifyId, kIdBytes * kB, true);
      break;
  }
}

void Wd1772::streamByte() {
  if (drq_) lost_ = true;  // the previous byte was never taken
  dataReg_ = buf_[idx_++];
  drq_ = true;
  if (idx_ >= len_) {
    // Sector data is followed by two CRC bytes before the chip finishes.
    enter(Phase::StreamEnd, kind() == Kind::ReadSector ? 2 * kB : kB, true);
  } else {
    enter(Phase::Stream, kB, true);
  }
}

void Wd1772::streamEnd() {
  if (kind() == Kind::ReadSector) {
    ++sectorsRead_;
    if (cmd_ & 0x10) {  // m: on to the next sector, no spin-up or settle
      ++sectorReg_;
      startSearch();
      return;
    }
  } else if (kind() == Kind::ReadAddress) {
    sectorReg_ = buf_[0];  // the ID's track number lands in the sector register
  }
  complete();
}

void Wd1772::writeByte() {
  uint8_t b = 0;  // a late byte is replaced by zero
  if (drq_) lost_ = true;
  else b = dataReg_;
  buf_[idx_++] = b;
  if (idx_ < len_) {
    drq_ = true;  // ask for the next byte
    enter(Phase::WriteStream, kB, true);
  } else {
    enter(Phase::WriteEnd, 3 * kB, true);  // last byte, two CRC bytes
  }
}

void Wd1772::writeEnd() {
  try {
    if (disk_ == nullptr) throw DiskImageError("no disk");
    disk_->writeSector(head_, slot_, std::span<const uint8_t>(buf_.data(), len_));
  } catch (const DiskImageError&) {
    fail();
    return;
  }
  if (cmd_ & 0x10) {
    ++sectorReg_;
    startSearch();
    return;
  }
  complete();
}

// One byte time of Write Track: the data register goes to the disk ($00, with
// Lost Data, if the CPU has not reloaded it) and DRQ asks for the next byte.
// $F5 and $F6 write one byte ($A1, $C2), $F7 two (the CRC), so the byte after
// an $F7 is taken two byte times later. The stream keeps the CPU's bytes; the
// parse at the next index pulse reads the marks from them.
void Wd1772::trackByte() {
  uint8_t b = 0;
  if (drq_) lost_ = true;
  else b = dataReg_;
  if (trackLen_ < trackBuf_.size()) trackBuf_[trackLen_++] = b;
  drq_ = true;
  enter(Phase::TrackStream, (b == 0xF7 ? 2 : 1) * kB, true);
}

// The next index pulse ends Write Track: the track becomes formatted if the
// stream holds the Mirage layout, otherwise unformatted (a drive would find
// none of the Mirage's sectors on it).
void Wd1772::writeTrackEnd() {
  if (disk_ != nullptr && head_ < DiskImage::kTracks) {
    std::array<uint8_t, DiskImage::kTrackBytes> data{};
    if (parseMirageTrack(std::span<const uint8_t>(trackBuf_.data(), trackLen_), head_, data))
      disk_->setTrack(head_, data);
    else
      disk_->unformatTrack(head_);
  }
  complete();
}

void Wd1772::save(StateWriter& w) const {
  w.tag(kStateTag);
  w.u8(trackReg_);
  w.u8(sectorReg_);
  w.u8(dataReg_);
  w.u8(cmd_);
  w.u8(errBits_);
  const bool flags[] = {lost_,      su_,     chipMotor_,   intrq_,  drq_,   busy_,
                        forcedIrq_, forceOnIndex_, motor_, dirOut_, needSpin_, statI_,
                        diskChanged_};
  uint16_t bits = 0;
  for (size_t i = 0; i < std::size(flags); ++i)
    if (flags[i]) bits = uint16_t(bits | (1u << i));
  w.u16(bits);
  w.i32(head_);
  w.u32(rot_);
  w.u8(uint8_t(phase_));
  w.u32(wait_);
  w.u8(spinCount_);
  w.u8(indexCount_);
  w.u8(idleIndex_);
  w.u8(stepCount_);
  w.u8(slot_);
  w.u16(idx_);
  w.u16(len_);
  w.bytes(buf_);
  w.u16(trackLen_);
  w.bytes(std::span<const uint8_t>(trackBuf_.data(), trackLen_));
  w.u64(sectorsRead_);
}

void Wd1772::load(StateReader& r) {
  Wd1772 t;
  r.tag(kStateTag);
  t.trackReg_ = r.u8();
  t.sectorReg_ = r.u8();
  t.dataReg_ = r.u8();
  t.cmd_ = r.u8();
  t.errBits_ = r.u8();
  const uint16_t bits = r.u16();
  auto bit = [bits](int i) { return (bits >> i & 1) != 0; };
  t.lost_ = bit(0);
  t.su_ = bit(1);
  t.chipMotor_ = bit(2);
  t.intrq_ = bit(3);
  t.drq_ = bit(4);
  t.busy_ = bit(5);
  t.forcedIrq_ = bit(6);
  t.forceOnIndex_ = bit(7);
  t.motor_ = bit(8);
  t.dirOut_ = bit(9);
  t.needSpin_ = bit(10);
  t.statI_ = bit(11);
  t.diskChanged_ = bit(12);
  const int32_t head = r.i32();
  t.rot_ = r.u32();
  const uint8_t phase = r.u8();
  t.wait_ = r.u32();
  t.spinCount_ = r.u8();
  t.indexCount_ = r.u8();
  t.idleIndex_ = r.u8();
  t.stepCount_ = r.u8();
  t.slot_ = r.u8();
  t.idx_ = r.u16();
  t.len_ = r.u16();
  r.bytes(t.buf_);
  t.trackLen_ = r.u16();
  if (t.trackLen_ > t.trackBuf_.size()) throw StateError("wd1772: bad track stream length");
  r.bytes(std::span<uint8_t>(t.trackBuf_.data(), t.trackLen_));
  t.sectorsRead_ = r.u64();

  if (bits >> 13) throw StateError("wd1772: bad flags");
  if (head < 0 || head > kMaxHead) throw StateError("wd1772: bad head position");
  if (t.rot_ >= kDiskIndexCycles) throw StateError("wd1772: bad rotation");
  if (phase > uint8_t(Phase::Last)) throw StateError("wd1772: bad phase");
  if (t.wait_ > 2 * kDiskIndexCycles) throw StateError("wd1772: bad wait");
  if (t.slot_ >= kSlots || t.len_ > t.buf_.size() || t.idx_ > t.len_)
    throw StateError("wd1772: bad transfer state");
  t.head_ = head;
  t.phase_ = static_cast<Phase>(phase);
  if ((t.phase_ == Phase::Idle) == t.busy_) throw StateError("wd1772: phase/busy mismatch");
  if ((t.phase_ == Phase::Stream || t.phase_ == Phase::WriteStream) && t.idx_ >= t.len_)
    throw StateError("wd1772: bad transfer state");
  if ((t.phase_ == Phase::TrackWaitIndex || t.phase_ == Phase::TrackStream) &&
      t.kind() != Kind::WriteTrack)
    throw StateError("wd1772: bad transfer state");

  t.disk_ = disk_;
  *this = t;
}

}  // namespace mirage
