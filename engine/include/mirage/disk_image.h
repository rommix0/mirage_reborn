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
class DiskImage {
 public:
  static constexpr int kTracks = 80, kTrackBytes = 5632, kImageBytes = 450560;

  static DiskImage fromBytes(std::span<const uint8_t>);     // throws DiskImageError
  static DiskImage loadFile(const std::filesystem::path&);  // any extension
  static DiskImage blank();                                 // every track unformatted

  std::vector<uint8_t> toBytes() const;  // throws if any track unformatted
  void saveFile(const std::filesystem::path&) const;

  bool trackFormatted(int track) const;
  int sectorSize(int track, int sectorId) const;  // 1024, 512, or 0 if absent
  // Throw DiskImageError if the sector is absent or the span size != sectorSize.
  void readSector(int track, int sectorId, std::span<uint8_t> out) const;
  void writeSector(int track, int sectorId, std::span<const uint8_t> in);  // sets dirty

  bool dirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }

  void save(StateWriter&) const;
  void load(StateReader&);

 private:
  using Track = std::array<uint8_t, kTrackBytes>;

  // Validates track/sector/span size; returns the byte offset within the track.
  int locate(int track, int sectorId, size_t spanSize, const char* op) const;

  // Heap-allocated: the whole image is 450 KB, too big to live on the stack.
  std::vector<std::optional<Track>> tracks_ = std::vector<std::optional<Track>>(kTracks);
  bool dirty_ = false;
};

}  // namespace mirage
