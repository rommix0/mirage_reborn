#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

#include "mirage/disk_session.h"

// The File menu's disk actions on a DiskSession: Open, New Blank Disk, Save,
// Save As and Eject, with their file choosers and prompts. Shared by the app's
// MainWindow and the plugin's editor. Message thread; every dialog is
// asynchronous and shown over `owner`.
//
// Open, New Blank Disk and Eject ask first when the disk has unsaved changes
// (whenSafeToReplace). Save with no file is Save As. A save completes in the
// owner's DiskSession::poll(), whose outcome goes to showSaveOutcome.
class DiskActions {
 public:
  // Runs a piece of disk work: at once by default. The plugin's holds its state
  // lock while it runs, and may run it later when the lock is busy.
  using Guard = std::function<void(std::function<void()>)>;

  // `diskFolder`: where the choosers start; `setDiskFolder`: the folder of a file
  // the user picked; `changed`: the disk or a save in flight may have changed.
  DiskActions(juce::Component& owner, mirage::DiskSession& session, std::function<juce::File()> diskFolder,
              std::function<void(const juce::File&)> setDiskFolder, std::function<void()> changed,
              Guard guard = {});
  ~DiskActions();

  void open();
  void newBlank();
  void save();  // Save As when the disk has no file
  void saveAs();
  void eject();
  // Runs `then` at once if the disk has no unsaved changes. Otherwise asks
  // "Save changes to X before <verb>?" (Save / Don't Save / Cancel): Save runs
  // `then` once the save succeeds, Don't Save runs it now, Cancel drops it.
  // Ignored while that prompt is already showing.
  void whenSafeToReplace(const juce::String& verb, std::function<void()> then);
  // A finished save (DiskSession::poll): runs what waited on it, or shows why it failed.
  void showSaveOutcome(const mirage::SaveOutcome&);
  // A prompt, chooser or other modal dialog is open (keyboard shortcuts are off).
  bool dialogOpen() const;
  void showError(const juce::String& title, const juce::String& message);

 private:
  void run(std::function<void()> work);  // through the guard, if this still exists then
  void insert(const juce::File& file);
  void save(std::function<void()> then);
  void saveAs(std::function<void()> then);
  void startSave(const juce::File& file, std::function<void()> then);  // File(): its own file

  juce::Component& owner_;
  mirage::DiskSession& session_;
  std::function<juce::File()> diskFolder_;
  std::function<void(const juce::File&)> setDiskFolder_;
  std::function<void()> changed_;
  Guard guard_;
  std::unique_ptr<juce::FileChooser> chooser_;
  std::function<void()> afterSave_;  // runs when the save in flight succeeds
  bool promptOpen_ = false;          // the unsaved-changes prompt is showing

  JUCE_DECLARE_WEAK_REFERENCEABLE(DiskActions)
};
