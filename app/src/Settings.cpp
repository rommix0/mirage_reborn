#include "Settings.h"

Settings::Settings() {
  juce::PropertiesFile::Options o;
  o.applicationName = "MirageEmulator";
  o.folderName = "MirageEmulator";
  o.filenameSuffix = ".settings";
  o.osxLibrarySubFolder = "Application Support";
  props_.setStorageParameters(o);
}

juce::PropertiesFile& Settings::file() const { return *props_.getUserSettings(); }

juce::String Settings::romPath() const { return file().getValue("romPath"); }
void Settings::setRomPath(const juce::String& p) { file().setValue("romPath", p); }
juce::String Settings::lastDiskDir() const { return file().getValue("lastDiskDir"); }
void Settings::setLastDiskDir(const juce::String& d) { file().setValue("lastDiskDir", d); }

std::unique_ptr<juce::XmlElement> Settings::audioState() const {
  return file().getXmlValue("audioState");
}
void Settings::setAudioState(const juce::XmlElement* state) {
  if (state != nullptr)
    file().setValue("audioState", state);
  else
    file().removeValue("audioState");
}

juce::String Settings::midiIn() const { return file().getValue("midiIn"); }
void Settings::setMidiIn(const juce::String& id) { file().setValue("midiIn", id); }
juce::String Settings::midiOut() const { return file().getValue("midiOut"); }
void Settings::setMidiOut(const juce::String& id) { file().setValue("midiOut", id); }

float Settings::volume() const {
  return juce::jlimit(0.0f, 1.0f, (float)file().getDoubleValue("volume", 1.0));
}
void Settings::setVolume(float g) { file().setValue("volume", (double)g); }

juce::String Settings::windowState() const { return file().getValue("windowState"); }
void Settings::setWindowState(const juce::String& s) { file().setValue("windowState", s); }

void Settings::save() { props_.saveIfNeeded(); }
