#include "mirage/disk_session.h"

#include <chrono>
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

double steadySeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

DiskSession::DiskSession(RealtimeHost& host, Clock clock)
    : host_(host), clock_(clock ? std::move(clock) : Clock(steadySeconds)) {}

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
  savedWrites_.reset();
  return {};
}

void DiskSession::eject() {
  if (saving_) return;
  if (host_.postEject()) {
    path_.clear();
    savedWrites_.reset();
  }
}

bool DiskSession::startSave(const std::filesystem::path& file) {
  const auto& target = file.empty() ? path_ : file;
  if (saving_ || target.empty()) return false;
  if (!host_.postCopyDisk(std::make_unique<DiskImage>(DiskImage::blank()))) return false;
  ++copiesOut_;
  saveTarget_ = target;
  saving_ = true;
  saveStarted_ = clock_();
  return true;
}

std::optional<SaveOutcome> DiskSession::poll() {
  std::optional<SaveOutcome> outcome;
  // Every disk handed back is freed here when `r` goes out of scope; only the
  // Copy that answers the current save is written out first.
  while (auto r = host_.takeReturnedDisk()) {
    if (r->why != DiskReturn::Copy && r->why != DiskReturn::NoDiskToCopy) continue;  // Ejected, Unused
    --copiesOut_;
    if (!saving_ || copiesOut_ != 0) continue;  // a timed-out save's copy
    saving_ = false;
    if (r->why == DiskReturn::NoDiskToCopy) {
      outcome = SaveOutcome{false, "There is no disk in the drive.", saveTarget_};
      continue;
    }
    try {
      r->disk->saveFile(saveTarget_);
      // Clears dirty unless the OS wrote again since the copy. If the queue
      // is full the disk merely stays marked modified.
      host_.postMarkSaved(r->writeCount);
      savedWrites_ = r->writeCount;
      path_ = saveTarget_;
      outcome = SaveOutcome{true, {}, saveTarget_};
    } catch (const std::exception& e) {
      outcome = SaveOutcome{false, e.what(), saveTarget_};
    }
  }
  if (saving_ && clock_() - saveStarted_ >= kSaveTimeoutSeconds) {
    saving_ = false;  // the copy stays counted in copiesOut_, to be freed if it ever returns
    outcome = SaveOutcome{
        false, "The audio engine is not running; the disk was not saved. Check Audio Settings.",
        saveTarget_};
  }
  return outcome;
}

bool DiskSession::modified() const {
  if (path_.empty()) return false;
  const HostStatus s = host_.status();
  // The published write count equal to the saved copy's: every write is in the
  // file, even if the mark-saved has not been applied (or published) yet.
  return s.diskDirty && !(savedWrites_ && s.diskWrites == *savedWrites_);
}

bool DiskSession::needsSaveBeforeReplacing() const { return modified(); }

std::string DiskSession::title() const {
  if (path_.empty()) return "No disk";
  std::string t = utf8(path_.filename());
  if (modified()) t += " (modified)";
  return t;
}

}  // namespace mirage
