#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "mirage/state_io.h"

namespace mirage {

struct DiskImageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// A Mirage floppy: 80 single-sided tracks, each 5 sectors of 1024 bytes plus
// one of 512 (ids 0-5), 5632 bytes per track. A track is either formatted
// (holds data) or unformatted (absent), so a blank disk can be formatted by
// the emulated controller.
//
// Move-only: the image is 450 KB, so copies are explicit (clone, copyTo), and
// the host can hand disks between threads as std::unique_ptr<DiskImage>. A
// moved-from DiskImage holds no tracks and may only be assigned to or
// destroyed.
class DiskImage {
 public:
  DiskImage() = default;  // same as blank()
  DiskImage(const DiskImage&) = delete;
  DiskImage& operator=(const DiskImage&) = delete;
  DiskImage(DiskImage&&) = default;
  DiskImage& operator=(DiskImage&&) = default;

  static constexpr int kTracks = 80, kTrackBytes = 5632, kImageBytes = 450560;

  static DiskImage fromBytes(std::span<const uint8_t>);     // throws DiskImageError
  static DiskImage loadFile(const std::filesystem::path&);  // any extension
  static DiskImage blank();                                 // every track unformatted

  std::vector<uint8_t> toBytes() const;  // throws if any track unformatted
  // Atomic: the bytes are written to "<path>.tmp" in the same folder, then
  // renamed over the target, so the target is never left partial. On any
  // failure the temp file is removed and DiskImageError names the path; an
  // existing target is untouched. Throws if any track is unformatted.
  void saveFile(const std::filesystem::path&) const;

  // Deep copy (tracks, dirty flag, write count). Allocates.
  DiskImage clone() const;
  // Copies tracks and the dirty flag into dest, and dest.writeCount() becomes
  // writeCount(). Does not allocate (or free) provided dest is not moved-from,
  // e.g. dest came from DiskImage::blank(): the audio thread relies on this.
  // If dest is moved-from (empty track vector) it is resized first, which
  // allocates.
  void copyTo(DiskImage& dest) const;

  bool trackFormatted(int track) const;
  int sectorSize(int track, int sectorId) const;  // 1024, 512, or 0 if absent
  // Throw DiskImageError if the sector is absent or the span size != sectorSize.
  void readSector(int track, int sectorId, std::span<uint8_t> out) const;
  void writeSector(int track, int sectorId, std::span<const uint8_t> in);  // sets dirty

  bool dirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }
  // Number of writeSector calls since construction (clone/copyTo carry it
  // over). Not saved in snapshots: load() restarts it at 0. A saver compares
  // it before and after to tell whether a write slipped in, before clearing
  // dirty.
  uint64_t writeCount() const { return writeCount_; }

  void save(StateWriter&) const;
  void load(StateReader&);

 private:
  using Track = std::array<uint8_t, kTrackBytes>;

  // Validates track/sector/span size; returns the byte offset within the track.
  int locate(int track, int sectorId, size_t spanSize, const char* op) const;

  // Heap-allocated: the whole image is 450 KB, too big to live on the stack.
  std::vector<std::optional<Track>> tracks_ = std::vector<std::optional<Track>>(kTracks);
  bool dirty_ = false;
  uint64_t writeCount_ = 0;
};

}  // namespace mirage
