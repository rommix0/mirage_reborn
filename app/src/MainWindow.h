#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <vector>

#include "AudioEngine.h"
#include "PanelComponent.h"
#include "Settings.h"
#include "mirage/disk_session.h"
#include "mirage/realtime_host.h"
#include "mirage/wav_import.h"

// The window's content: the replica panel above a status strip. The panel's
// volume slider drives the engine's output gain.
class MirageContent : public juce::Component, private juce::Timer {
 public:
  static constexpr int kStripHeight = 26;
  static constexpr juce::uint32 kNoticeMs = 15000;

  MirageContent(mirage::RealtimeHost& host, AudioEngine& engine);
  ~MirageContent() override = default;

  // File name (empty = unnamed) for the panel; title ("name (modified)" or
  // "No disk") for the status strip.
  void setDiskName(const juce::String& name, const juce::String& title);
  // A one-line message (an import's outcome) on the right of the status strip,
  // before any warnings, for kNoticeMs.
  void showNotice(const juce::String& text);
  PanelComponent& panel() { return *panel_; }
  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  struct StripText {
    juce::String left, middle, right;
    bool operator==(const StripText&) const = default;
  };

  void timerCallback() override;
  StripText buildText() const;

  mirage::RealtimeHost& host_;
  AudioEngine& engine_;
  juce::String diskTitle_;  // empty: show "No disk"
  juce::String notice_;
  juce::uint32 noticeUntil_ = 0;  // juce::Time::getMillisecondCounter() when notice_ goes
  std::unique_ptr<PanelComponent> panel_;
  StripText text_;
};

// File menu: Open, New Blank Disk, Save, Save As and Eject work through a
// DiskSession; Import WAV writes wave RAM through the host (ImportDialog). Open, New Blank Disk, Eject and Exit (and the close button) ask
// first when the disk has unsaved changes. The title names the disk: "Mirage
// <em dash> SND-A01.img (modified)".
//
// Keyboard: the digits press 0..9, the up and down arrows On/Off, Return Enter
// and Escape Cancel, one press per physical press, released when the key comes
// up. Ignored while a modal dialog or file chooser is open; Ctrl and Alt
// combinations go to the menus.
class MainWindow : public juce::DocumentWindow,
                   private juce::MenuBarModel,
                   private juce::ApplicationCommandTarget,
                   private juce::KeyListener,
                   private juce::Timer {
 public:
  MainWindow(mirage::RealtimeHost& host, AudioEngine& engine, Settings& settings);
  ~MainWindow() override;

  void closeButtonPressed() override;
  void activeWindowStatusChanged() override;
  MirageContent& content() { return *content_; }
  // Quits the app, after the unsaved-disk prompt when needed (a Save must
  // succeed first). Ignored while that prompt is already showing.
  void requestQuit();

 private:
  enum CommandIds {
    cmdOpenDisk = 1,
    cmdNewBlankDisk,
    cmdSaveDisk,
    cmdSaveDiskAs,
    cmdEjectDisk,
    cmdImportWav,
    cmdQuit,
    cmdReset,
    cmdPanic,
    cmdAudioSettings
  };
  enum MenuIds { midiInNone = 999, midiInBase = 1000, midiOutNone = 1999, midiOutBase = 2000 };

  // MenuBarModel
  juce::StringArray getMenuBarNames() override;
  juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
  void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;
  // ApplicationCommandTarget
  ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
  void getAllCommands(juce::Array<juce::CommandID>& commands) override;
  void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override;
  bool perform(const InvocationInfo& info) override;

  // KeyListener
  bool keyPressed(const juce::KeyPress& key, juce::Component*) override;
  bool keyStateChanged(bool isKeyDown, juce::Component*) override;
  void releaseHeldKeys(bool all);  // all, or only those no longer physically down
  // Runs while keys are held: a modal dialog takes the key-ups, so let go when one opens.
  void timerCallback() override;

  void showAudioSettings();

  // Disk files.
  void openDisk();
  void newBlankDisk();
  void insertDisk(const juce::File& file);
  void saveDisk(std::function<void()> then = {});  // Save As when the disk has no file
  void saveDiskAs(std::function<void()> then = {});
  void startSave(const juce::File& file, std::function<void()> then);  // File(): its own file
  void ejectDisk();
  // Runs `then` at once if the disk has no unsaved changes. Otherwise asks
  // "Save changes to X before <action>?" (Save / Don't Save / Cancel): Save
  // runs `then` once the save succeeds, Don't Save runs it now, Cancel drops it.
  void whenDiskSafe(const juce::String& action, std::function<void()> then);
  void pollDisk();          // 10 Hz: finishes saves, frees returned disks
  void refreshDiskState();  // title, drive label, menu enablement
  juce::File diskFolder() const;  // where the file dialogs start
  void showError(const juce::String& title, const juce::String& message);

  // WAV import (spec 6.7): a file chooser, then the modal ImportDialog. Import
  // converts on this thread and posts the write; pollWaves reports the outcome
  // in the status strip when the write comes back from the audio thread.
  bool canImportWav() const;  // an audio device runs and the drive LED is off
  void importWav();
  void showImportDialog(const juce::File& file);
  void postImport(const mirage::WavAudio& audio, const mirage::ImportSettings& settings);
  void pollWaves();  // 10 Hz, with pollDisk: frees returned writes

  mirage::RealtimeHost& host_;
  AudioEngine& engine_;
  Settings& settings_;
  juce::ApplicationCommandManager commands_;
  MirageContent* content_ = nullptr;  // owned by the window
  juce::Array<juce::MidiDeviceInfo> midiInputs_, midiOutputs_;  // as of the last menu build
  struct HeldKey {
    int keyCode;  // juce::KeyPress key code, polled with isKeyCurrentlyDown
    mirage::PanelKey key;
  };
  std::vector<HeldKey> held_;  // keyboard keys holding panel keys

  mirage::DiskSession session_;
  juce::TimedCallback diskTimer_{[this] { pollDisk(); }};
  std::unique_ptr<juce::FileChooser> chooser_;
  std::function<void()> afterSave_;  // runs when the save in flight succeeds
  bool promptOpen_ = false;          // the unsaved-changes prompt is showing
  bool menusHaveDisk_ = false, menusSaving_ = false;  // the state the menus last saw
  bool menusCanImport_ = false;
  juce::File lastWavDir_;  // this session's last WAV folder; the disk folder before that
  // Deleted with the window: it refers to the engine's AudioDeviceManager.
  juce::Component::SafePointer<juce::DialogWindow> audioSettings_;
  juce::Component::SafePointer<juce::DialogWindow> importDialog_;  // its callbacks use this window
};
