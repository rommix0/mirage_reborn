// Giebler .edm disk files (docs/hardware/fdc.md, "Disk file formats").
//
// A 512-byte header: 0D 0A "Mirage Disk", spaces, 0D 0A at $4E, spaces, 0D 0A 1A
// at $9D (the text a DOS "type" shows), then at $A0 a bitmap of the 480
// sectors, MSB first, per track in the order 5, 0, 1, 2, 3, 4; a 1 bit marks a
// sector that is left out because it is all zero. $DC-$1FE are zero (Giebler's
// .EDE header keeps a compression flag at $1FD) and $1FF is $01, the disk type.
// The stored sectors follow in bitmap order, each in 1024 bytes (sector 5's
// 512 bytes padded with zeros), and the file ends with one $1A.
#include <algorithm>
#include <string>

#include "mirage/disk_image.h"

namespace mirage {

namespace {

constexpr char kSignature[] = "\r\nMirage Disk";  // 13 bytes, no terminator used
constexpr size_t kSignatureBytes = sizeof(kSignature) - 1;
constexpr size_t kHeaderBytes = 512;
constexpr size_t kBitmapAt = 0xA0;
constexpr size_t kSlotBytes = 1024;  // file space per stored sector
constexpr int kSectorsPerTrack = 6;
constexpr int kSectorCount = DiskImage::kTracks * kSectorsPerTrack;  // 480 bitmap bits
constexpr int kOrder[kSectorsPerTrack] = {5, 0, 1, 2, 3, 4};
constexpr uint8_t kEndOfFile = 0x1A;
constexpr uint8_t kMirageDiskType = 0x01;

// Where sector `id` of a track lives in the raw image, and its size.
size_t rawOffset(int track, int id) {
  return size_t(track) * DiskImage::kTrackBytes + size_t(id < 5 ? id * 1024 : 5 * 1024);
}
size_t sectorBytes(int id) { return id < 5 ? 1024 : 512; }

bool leftOut(std::span<const uint8_t> file, int bit) {
  return (file[kBitmapAt + size_t(bit) / 8] & (0x80 >> (bit % 8))) != 0;
}

bool hasSignature(std::span<const uint8_t> file) {
  return file.size() >= kSignatureBytes &&
         std::equal(kSignature, kSignature + kSignatureBytes, file.begin());
}

std::string withCommas(size_t n) {
  std::string s = std::to_string(n);
  for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
  return s;
}

}  // namespace

DiskFormat detectDiskFormat(std::span<const uint8_t> file) {
  // A Giebler file is never 450,560 bytes (512 + 1024k + 1), so size decides first.
  if (file.size() == size_t(DiskImage::kImageBytes)) return DiskFormat::Raw;
  if (hasSignature(file)) return DiskFormat::GieblerEdm;
  throw DiskImageError("not a Mirage disk image (expected 450,560 bytes or a Giebler .edm file)");
}

DiskImage DiskImage::fromGieblerEdm(std::span<const uint8_t> file) {
  if (file.size() < kHeaderBytes)
    throw DiskImageError("damaged Giebler .edm file: the 512-byte header is cut short (" +
                         std::to_string(file.size()) + " bytes)");
  int stored = 0;
  for (int bit = 0; bit < kSectorCount; ++bit) stored += leftOut(file, bit) ? 0 : 1;
  const size_t needed = kHeaderBytes + size_t(stored) * kSlotBytes;
  if (file.size() < needed)
    throw DiskImageError("damaged Giebler .edm file: the header lists " + std::to_string(stored) +
                         " stored sectors (" + withCommas(needed) + " bytes) but the file has " +
                         withCommas(file.size()) + " bytes");
  // Bytes after the stored sectors (normally one $1A) are ignored.
  std::vector<uint8_t> raw(kImageBytes);  // left-out sectors read as zero
  size_t at = kHeaderBytes;
  for (int t = 0; t < kTracks; ++t) {
    for (int k = 0; k < kSectorsPerTrack; ++k) {
      if (leftOut(file, t * kSectorsPerTrack + k)) continue;
      const int id = kOrder[k];
      std::copy_n(file.begin() + std::ptrdiff_t(at), sectorBytes(id),
                  raw.begin() + std::ptrdiff_t(rawOffset(t, id)));
      at += kSlotBytes;
    }
  }
  return fromBytes(raw);
}

std::vector<uint8_t> DiskImage::toEdmBytes() const {
  const std::vector<uint8_t> raw = toBytes();  // throws for an unformatted track
  std::vector<uint8_t> out(kHeaderBytes, 0);
  std::fill(out.begin(), out.begin() + 0x9D, uint8_t(' '));
  std::copy_n(kSignature, kSignatureBytes, out.begin());
  out[0x4E] = 0x0D;
  out[0x4F] = 0x0A;
  out[0x9D] = 0x0D;
  out[0x9E] = 0x0A;
  out[0x9F] = kEndOfFile;
  out[0x1FF] = kMirageDiskType;
  for (int t = 0; t < kTracks; ++t) {
    for (int k = 0; k < kSectorsPerTrack; ++k) {
      const int id = kOrder[k], bit = t * kSectorsPerTrack + k;
      const auto sector = raw.begin() + std::ptrdiff_t(rawOffset(t, id));
      const auto end = sector + std::ptrdiff_t(sectorBytes(id));
      if (std::all_of(sector, end, [](uint8_t b) { return b == 0; })) {
        out[kBitmapAt + size_t(bit) / 8] |= uint8_t(0x80 >> (bit % 8));
        continue;
      }
      out.insert(out.end(), sector, end);
      out.resize(out.size() + kSlotBytes - sectorBytes(id), 0);  // sector 5's padding
    }
  }
  out.push_back(kEndOfFile);
  return out;
}

}  // namespace mirage
