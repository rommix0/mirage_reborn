#include "mirage/disk_session.h"

#include <exception>
#include <memory>
#include <utility>

namespace mirage {

namespace {

// UTF-8 text of a path. path::string() goes through the ANSI code page on
// Windows and throws for names it cannot map.
std::string utf8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

}  // namespace

DiskSession::DiskSession(RealtimeHost& host) : host_(host) {}

std::string DiskSession::open(const std::filesystem::path& file) {
  if (saving_) return "Wait for the save to finish before opening another disk.";
  std::error_code ec;
  if (std::filesystem::is_directory(file, ec))
    return "'" + utf8(file) + "' is a folder, not a disk image.";
  std::unique_ptr<DiskImage> disk;
  try {
    disk = std::make_unique<DiskImage>(DiskImage::loadFile(file));
  } catch (const DiskImageError& e) {
    return e.what();  // already names the file and the problem
  } catch (const std::exception& e) {
    return "Cannot open disk image '" + utf8(file) + "': " + e.what();
  }
  if (!host_.postInsert(std::move(disk))) return "The emulator is busy. Try again.";
  path_ = file;
  return {};
}

void DiskSession::eject() {
  if (saving_) return;
  if (host_.postEject()) path_.clear();
}

bool DiskSession::startSave(const std::filesystem::path& file) {
  const auto& target = file.empty() ? path_ : file;
  if (saving_ || target.empty()) return false;
  if (!host_.postCopyDisk(std::make_unique<DiskImage>(DiskImage::blank()))) return false;
  saveTarget_ = target;
  saving_ = true;
  return true;
}

std::optional<SaveOutcome> DiskSession::poll() {
  std::optional<SaveOutcome> outcome;
  // Every disk handed back is freed here when `r` goes out of scope; only a
  // Copy that answers our save is written out first.
  while (auto r = host_.takeReturnedDisk()) {
    if (!saving_) continue;
    if (r->why == DiskReturn::NoDiskToCopy) {
      outcome = SaveOutcome{false, "There is no disk in the drive.", saveTarget_};
    } else if (r->why == DiskReturn::Copy) {
      try {
        r->disk->saveFile(saveTarget_);
        // Clears dirty unless the OS wrote again since the copy. If the queue
        // is full the disk merely stays marked modified.
        host_.postMarkSaved(r->writeCount);
        path_ = saveTarget_;
        outcome = SaveOutcome{true, {}, saveTarget_};
      } catch (const std::exception& e) {
        outcome = SaveOutcome{false, e.what(), saveTarget_};
      }
    } else {
      continue;  // Ejected or Unused
    }
    saving_ = false;
  }
  return outcome;
}

bool DiskSession::needsSaveBeforeReplacing() const {
  return !path_.empty() && host_.status().diskDirty;
}

std::string DiskSession::title() const {
  if (path_.empty()) return "No disk";
  std::string t = utf8(path_.filename());
  if (host_.status().diskDirty) t += " (modified)";
  return t;
}

}  // namespace mirage
