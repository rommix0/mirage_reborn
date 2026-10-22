#pragma once
#include <juce_gui_extra/juce_gui_extra.h>

// Persistent user settings (juce::ApplicationProperties, folder "MirageEmulator").
class Settings {
 public:
  Settings();

  juce::String romPath() const;
  void setRomPath(const juce::String& path);
  juce::String lastDiskDir() const;
  void setLastDiskDir(const juce::String& dir);
  // The AudioDeviceManager state (AudioEngine::saveAudioState); null if none saved.
  std::unique_ptr<juce::XmlElement> audioState() const;
  void setAudioState(const juce::XmlElement* state);
  juce::String midiIn() const;  // MIDI device identifiers; empty = none
  void setMidiIn(const juce::String& id);
  juce::String midiOut() const;
  void setMidiOut(const juce::String& id);
  float volume() const;  // 0..1
  void setVolume(float gain01);
  float inputGain() const;  // 0..4, default 1: the gain before the emulated input jack
  void setInputGain(float gain);
  juce::String windowState() const;
  void setWindowState(const juce::String& state);

  void save();  // writes the file now

 private:
  juce::PropertiesFile& file() const;
  mutable juce::ApplicationProperties props_;
};
