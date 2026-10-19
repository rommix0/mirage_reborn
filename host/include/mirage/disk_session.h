#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "mirage/realtime_host.h"

namespace mirage {

// How a save ended. message is empty on success; path is where it was written
// (or was meant to be).
struct SaveOutcome {
  bool ok = false;
  std::string message;
  std::filesystem::path path;
};

// The disk in the drive as the user sees it: which file it came from, whether
// it has unsaved changes, and the open / save / save as / eject operations.
// UI thread only, JUCE-free; the caller does the prompting and the dialogs.
//
// A save copies the disk out on the audio thread (postCopyDisk into a blank
// buffer), then writes the copy here with DiskImage::saveFile (atomic) and
// sends postMarkSaved with the copy's write count, so a write the OS makes in
// between keeps the disk modified. poll() must run regularly (>= 10 Hz): it
// finishes saves and frees every disk the host hands back.
//
// While a save is in flight, open() and eject() are refused: the mark-saved
// must reach the disk that was copied.
class DiskSession {
 public:
  explicit DiskSession(RealtimeHost& host);
  DiskSession(const DiskSession&) = delete;
  DiskSession& operator=(const DiskSession&) = delete;

  // Loads and validates now; on success posts the insert and remembers the path.
  // Returns an error message (empty on success). Does not prompt: the caller
  // checks needsSaveBeforeReplacing() first.
  std::string open(const std::filesystem::path& file);
  void eject();
  // Starts a save to `file` (current path when empty). Completes in poll().
  // False (nothing started) when no target is known, a save is already in
  // flight, or the host's queue is full.
  bool startSave(const std::filesystem::path& file = {});
  // Call on the UI thread at >= 10 Hz: frees returned disks, finishes saves.
  // Returns a finished save's outcome once (empty optional while none).
  std::optional<SaveOutcome> poll();
  bool needsSaveBeforeReplacing() const;  // a disk is in and status().diskDirty
  bool saving() const { return saving_; }
  const std::filesystem::path& path() const { return path_; }  // empty: no disk
  // UTF-8: "SND-A01.img", "SND-A01.img (modified)" or "No disk".
  std::string title() const;

 private:
  RealtimeHost& host_;
  std::filesystem::path path_;        // the inserted disk's file; empty: none
  bool saving_ = false;               // a copy is out, awaiting its return
  std::filesystem::path saveTarget_;  // where the copy goes
};

}  // namespace mirage
