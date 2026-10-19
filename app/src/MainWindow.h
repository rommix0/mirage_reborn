#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

#include "AudioEngine.h"
#include "Settings.h"
#include "mirage/realtime_host.h"

// The window's content: the panel area above a status strip. Task 10 replaces
// the placeholder panel with the real PanelComponent. host and engine may be
// null (the --panel-png render has neither); the strip then shows no machine.
class MirageContent : public juce::Component, private juce::Timer {
 public:
  static constexpr int kStripHeight = 26;

  MirageContent(mirage::RealtimeHost* host, AudioEngine* engine);
  ~MirageContent() override = default;

  void setDiskName(const juce::String& name) { diskName_ = name; }  // file name; empty = unnamed
  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  class Placeholder;       // stands in for the PanelComponent until Task 10
  struct StripText {
    juce::String left, middle, right;
    bool operator==(const StripText&) const = default;
  };

  void timerCallback() override;
  StripText buildText() const;

  mirage::RealtimeHost* host_;
  AudioEngine* engine_;
  juce::String diskName_;
  std::unique_ptr<juce::Component> panel_;
  StripText text_;
};

class MainWindow : public juce::DocumentWindow,
                   private juce::MenuBarModel,
                   private juce::ApplicationCommandTarget {
 public:
  MainWindow(mirage::RealtimeHost& host, AudioEngine& engine, Settings& settings);
  ~MainWindow() override;

  void closeButtonPressed() override;
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

  void showAudioSettings();

  mirage::RealtimeHost& host_;
  AudioEngine& engine_;
  Settings& settings_;
  juce::ApplicationCommandManager commands_;
  MirageContent* content_ = nullptr;  // owned by the window
  juce::Array<juce::MidiDeviceInfo> midiInputs_, midiOutputs_;  // as of the last menu build
};
