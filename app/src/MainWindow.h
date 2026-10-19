#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>
#include <vector>

#include "AudioEngine.h"
#include "PanelComponent.h"
#include "Settings.h"
#include "mirage/realtime_host.h"

// The window's content: the replica panel above a status strip. The panel's
// volume slider drives the engine's output gain.
class MirageContent : public juce::Component, private juce::Timer {
 public:
  static constexpr int kStripHeight = 26;

  MirageContent(mirage::RealtimeHost& host, AudioEngine& engine);
  ~MirageContent() override = default;

  void setDiskName(const juce::String& name);  // file name; empty = unnamed
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
  juce::String diskName_;
  std::unique_ptr<PanelComponent> panel_;
  StripText text_;
};

// Keyboard: the digits press 0..9, the up and down arrows On/Off, Return Enter
// and Escape Cancel, one press per physical press, released when the key comes
// up. Ignored while a modal dialog or file chooser is open; Ctrl and Alt
// combinations go to the menus.
class MainWindow : public juce::DocumentWindow,
                   private juce::MenuBarModel,
                   private juce::ApplicationCommandTarget,
                   private juce::KeyListener {
 public:
  MainWindow(mirage::RealtimeHost& host, AudioEngine& engine, Settings& settings);
  ~MainWindow() override;

  void closeButtonPressed() override;
  void activeWindowStatusChanged() override;
  MirageContent& content() { return *content_; }

 private:
  enum CommandIds {
    cmdOpenDisk = 1,
    cmdSaveDisk,
    cmdSaveDiskAs,
    cmdEjectDisk,
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

  void showAudioSettings();

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
};
