#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
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
// buffer), then writes the copy here with DiskImage::saveFile (atomic, in the
// format below) and sends postMarkSaved with the copy's write count, so a
// write the OS makes in between keeps the disk modified. poll() must run
// regularly (>= 10 Hz): it finishes saves and frees every disk the host hands
// back.
//
// The copy only comes back if the audio thread runs process(). If it has not
// come back kSaveTimeoutSeconds after startSave (no audio device, a failed
// device change), the save fails with a message saying so. The copy may still
// return later: poll() counts the copies out and frees such a stale one,
// never taking it as the answer to a later save.
//
// While a save is in flight, open() and eject() are refused: the mark-saved
// must reach the disk that was copied.
//
// Formats (docs/hardware/fdc.md): open() takes a raw image or a Giebler .edm,
// by content. Save writes the format the file was read in (or last saved as);
// Save As writes Giebler for a name ending in .edm (any case), raw otherwise.
class DiskSession {
 public:
  // A monotonic clock in seconds (tests inject their own).
  using Clock = std::function<double()>;
  static constexpr double kSaveTimeoutSeconds = 2.0;

  // An empty clock means std::chrono::steady_clock.
  explicit DiskSession(RealtimeHost& host, Clock clock = {});
  DiskSession(const DiskSession&) = delete;
  DiskSession& operator=(const DiskSession&) = delete;

  // Loads and validates now; on success posts the insert and remembers the path
  // and the format.
  // Returns an error message (empty on success). Does not prompt: the caller
  // checks needsSaveBeforeReplacing() first.
  std::string open(const std::filesystem::path& file);
  // Inserts a new, wholly unformatted disk (DiskImage::blank()) that has no
  // file: title "Untitled (blank)", path() empty until a Save As succeeds. The
  // OS formats it through the controller. False (nothing done) when a save is
  // in flight or the host's queue is full. Like open(), does not prompt.
  bool newBlank();
  void eject();
  // Starts a save to `file` (current path when empty, in the current format;
  // otherwise in the format its name picks). Completes in poll().
  // False (nothing started) when no target is known, a save is already in
  // flight, or the host's queue is full.
  // The save itself fails, leaving the disk modified and no file (not even a
  // temporary one), when the copy has unformatted tracks.
  bool startSave(const std::filesystem::path& file = {});
  // Call on the UI thread at >= 10 Hz: frees returned disks, finishes saves.
  // Returns a finished save's outcome once (empty optional while none).
  std::optional<SaveOutcome> poll();
  // A disk is in and has writes not in its file. status().diskDirty says so,
  // except that right after a successful save (before the audio thread has
  // applied the mark-saved) a disk whose published write count still equals
  // the saved copy's counts as clean.
  bool needsSaveBeforeReplacing() const;
  bool saving() const { return saving_; }
  // A disk was inserted (open or newBlank) and not ejected: from a file, or untitled.
  bool hasDisk() const { return !path_.empty() || untitled_; }
  const std::filesystem::path& path() const { return path_; }  // empty: no disk, or untitled
  // The format a plain Save writes: what open() found, or what the last
  // successful Save As chose. Raw for an untitled or absent disk.
  DiskFormat format() const { return format_; }
  // UTF-8: "SND-A01.img", "SND-A01.img (modified)", "Untitled (blank)" or "No disk".
  std::string title() const;

 private:
  bool modified() const;  // the disk in the drive has unsaved writes

  RealtimeHost& host_;
  Clock clock_;
  std::filesystem::path path_;               // the inserted disk's file; empty: none, or untitled
  DiskFormat format_ = DiskFormat::Raw;      // path_'s format: what a plain Save writes
  bool untitled_ = false;                    // a newBlank() disk is in and has no file yet
  bool saving_ = false;                      // a copy is out, awaiting its return
  double saveStarted_ = 0.0;                 // clock_() at startSave
  std::filesystem::path saveTarget_;         // where the copy goes
  DiskFormat saveFormat_ = DiskFormat::Raw;  // and in which format
  // Copies posted and not yet returned. Copies come back in order, so the one
  // answering the current save is the last: it returns when this drops to 0.
  size_t copiesOut_ = 0;
  std::optional<uint64_t> savedWrites_;  // write count of the last copy saved from this disk
};

}  // namespace mirage
