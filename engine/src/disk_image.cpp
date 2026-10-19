#include "mirage/disk_image.h"

#include <algorithm>
#include <fstream>
#include <string>

#include "path_text.h"

namespace mirage {

namespace {
constexpr int kSectors = 6;        // ids 0-5
constexpr int kBigSector = 1024;   // ids 0-4
constexpr int kSmallSector = 512;  // id 5
constexpr int kSmallSectorOffset = 5 * kBigSector;
constexpr uint32_t kStateTag = fourcc("DSKI");

std::string pathStr(const std::filesystem::path& p) { return pathText(p); }
}  // namespace

DiskImage DiskImage::fromBytes(std::span<const uint8_t> bytes) {
  if (bytes.size() != size_t(kImageBytes))
    throw DiskImageError("Not a Mirage disk image: expected exactly 450,560 bytes, got " +
                         std::to_string(bytes.size()) + ".");
  DiskImage d;
  for (int t = 0; t < kTracks; ++t) {
    Track& tr = d.tracks_[t].emplace();
    std::copy_n(bytes.begin() + size_t(t) * kTrackBytes, kTrackBytes, tr.begin());
  }
  return d;
}

DiskImage DiskImage::loadFile(const std::filesystem::path& path) {
  std::error_code ec;
  auto size = std::filesystem::file_size(path, ec);
  if (ec)
    throw DiskImageError("Cannot read disk image '" + pathStr(path) + "': " + ec.message());
  std::ifstream in(path, std::ios::binary);
  if (!in) throw DiskImageError("Cannot open disk image '" + pathStr(path) + "'.");
  // A wrong-size file is reported by fromBytes with its real size; don't
  // allocate absurd amounts for a huge unrelated file.
  if (size != uintmax_t(kImageBytes))
    throw DiskImageError(
        "Not a Mirage disk image: expected exactly 450,560 bytes, got " + std::to_string(size) +
        ". (" + pathStr(path) + ")");
  std::vector<uint8_t> buf(kImageBytes);
  in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
  if (!in || in.gcount() != std::streamsize(buf.size()))
    throw DiskImageError("Failed reading disk image '" + pathStr(path) + "'.");
  return fromBytes(buf);
}

DiskImage DiskImage::blank() { return DiskImage{}; }

std::vector<uint8_t> DiskImage::toBytes() const {
  std::vector<uint8_t> out;
  out.reserve(kImageBytes);
  for (int t = 0; t < kTracks; ++t) {
    if (!tracks_[t])
      throw DiskImageError("Cannot export disk image: track " + std::to_string(t) +
                           " is unformatted.");
    out.insert(out.end(), tracks_[t]->begin(), tracks_[t]->end());
  }
  return out;
}

void DiskImage::saveFile(const std::filesystem::path& path) const {
  const auto bytes = toBytes();  // before touching the disk: an unformatted track throws here
  std::filesystem::path tmp = path;
  tmp += ".tmp";  // same folder, so the rename stays on one volume
  std::error_code ec;
  const auto fail = [&](const std::string& why) {
    std::filesystem::remove(tmp, ec);
    throw DiskImageError(why + " '" + pathStr(path) + "'.");
  };
  bool opened = false, written = false;
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    opened = bool(out);
    if (opened) {
      out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
      out.close();  // a failed final flush shows up here, not in write()
      written = !out.fail();
    }
  }  // Closed here, before any remove or rename: on Windows an open file cannot be deleted.
  if (!opened) fail("Cannot open temporary file for writing disk image");
  if (!written) fail("Failed writing disk image");
  // Never replace the user's file with a short write.
  const auto onDisk = std::filesystem::file_size(tmp, ec);
  if (ec || onDisk != uintmax_t(kImageBytes)) fail("Disk image was not written completely to");
  std::filesystem::rename(tmp, path, ec);  // replaces an existing file
  if (ec) fail("Cannot replace disk image (" + ec.message() + ")");
}

DiskImage DiskImage::clone() const {
  DiskImage d;
  d.tracks_ = tracks_;
  d.dirty_ = dirty_;
  d.writeCount_ = writeCount_;
  return d;
}

void DiskImage::copyTo(DiskImage& dest) const {
  if (dest.tracks_.size() != tracks_.size()) dest.tracks_.resize(tracks_.size());  // moved-from only
  for (size_t i = 0; i < tracks_.size(); ++i) dest.tracks_[i] = tracks_[i];  // optional<array>: no heap
  dest.dirty_ = dirty_;
  dest.writeCount_ = writeCount_;
}

bool DiskImage::trackFormatted(int track) const {
  return track >= 0 && track < kTracks && tracks_[track].has_value();
}

int DiskImage::sectorSize(int track, int sectorId) const {
  if (!trackFormatted(track) || sectorId < 0 || sectorId >= kSectors) return 0;
  return sectorId < 5 ? kBigSector : kSmallSector;
}

int DiskImage::locate(int track, int sectorId, size_t spanSize, const char* op) const {
  int size = sectorSize(track, sectorId);
  if (size == 0)
    throw DiskImageError(std::string(op) + ": no sector " + std::to_string(sectorId) +
                         " on track " + std::to_string(track) +
                         " (unformatted or out of range).");
  if (spanSize != size_t(size))
    throw DiskImageError(std::string(op) + ": buffer is " + std::to_string(spanSize) +
                         " bytes but sector " + std::to_string(sectorId) + " is " +
                         std::to_string(size) + ".");
  return sectorId < 5 ? sectorId * kBigSector : kSmallSectorOffset;
}

void DiskImage::readSector(int track, int sectorId, std::span<uint8_t> out) const {
  int off = locate(track, sectorId, out.size(), "readSector");
  std::copy_n(tracks_[track]->begin() + off, out.size(), out.begin());
}

void DiskImage::writeSector(int track, int sectorId, std::span<const uint8_t> in) {
  int off = locate(track, sectorId, in.size(), "writeSector");
  std::copy(in.begin(), in.end(), tracks_[track]->begin() + off);
  dirty_ = true;
  ++writeCount_;
}

void DiskImage::save(StateWriter& w) const {
  w.tag(kStateTag);
  w.u8(dirty_ ? 1 : 0);
  for (const auto& tr : tracks_) {
    w.u8(tr ? 1 : 0);
    if (tr) w.bytes(*tr);
  }
}

void DiskImage::load(StateReader& r) {
  r.tag(kStateTag);
  // Decode into a scratch copy so a truncated/corrupt state leaves *this intact.
  DiskImage tmp;
  tmp.dirty_ = r.u8() != 0;
  for (auto& tr : tmp.tracks_) {
    if (r.u8() != 0) r.bytes(tr.emplace());
  }
  *this = std::move(tmp);
}

}  // namespace mirage
