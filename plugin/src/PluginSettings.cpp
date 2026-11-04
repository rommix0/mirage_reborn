#include "PluginSettings.h"

namespace {

constexpr juce::int64 kRomBytes = 4096;

juce::PropertiesFile::Options settingsOptions(const juce::String& name) {
  juce::PropertiesFile::Options o;
  o.applicationName = name;
  o.folderName = "MirageEmulator";
  o.filenameSuffix = ".settings";
  o.osxLibrarySubFolder = "Application Support";
  return o;
}

juce::File fileAt(const juce::String& path) {
  return juce::File::isAbsolutePath(path) ? juce::File(path) : juce::File();
}

std::optional<juce::MemoryBlock> readRom(const juce::File& f) {
  juce::MemoryBlock bytes;
  if (f == juce::File() || !f.existsAsFile() || f.getSize() != kRomBytes || !f.loadFileAsData(bytes) ||
      bytes.getSize() != (size_t)kRomBytes)
    return std::nullopt;
  return bytes;
}

// mirage.bin in the folder that holds the Mirage.vst3 bundle (this DLL lives in
// Mirage.vst3/Contents/x86_64-win).
juce::File besideBundle() {
  const auto dll = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
  for (auto d = dll.getParentDirectory(); d != d.getParentDirectory(); d = d.getParentDirectory())
    if (d.hasFileExtension(".vst3")) return d.getSiblingFile("mirage.bin");
  return {};
}

// The app's saved ROM path (MirageEmulator.settings), read and never written.
juce::File appRom() {
  const auto o = settingsOptions("MirageEmulator");
  juce::PropertiesFile app(o.getDefaultFile(), o);
  return fileAt(app.getValue("romPath"));
}

}  // namespace

PluginShared::PluginShared() {
  auto o = settingsOptions("MiragePlugin");
  o.processLock = &lock;
  props.setStorageParameters(o);
}

juce::PropertiesFile& pluginSettings() {
  return *juce::SharedResourcePointer<PluginShared>()->props.getUserSettings();
}

std::optional<juce::MemoryBlock> findBootRom(juce::File& found) {
  juce::SharedResourcePointer<PluginShared> s;
  const std::lock_guard lock(s->romMutex);
  if (!s->rom) {
    const juce::File candidates[] = {
        fileAt(s->props.getUserSettings()->getValue("romPath")),
        besideBundle(),
        appRom(),
#ifdef MIRAGE_ROM
        fileAt(MIRAGE_ROM),
#endif
    };
    for (const auto& f : candidates)
      if (auto bytes = readRom(f)) {
        s->rom = std::move(bytes);
        s->romFile = f;
        break;
      }
  }
  if (!s->rom) return std::nullopt;
  found = s->romFile;
  return s->rom;
}

bool setBootRom(const juce::File& file) {
  auto bytes = readRom(file);
  if (!bytes) return false;
  juce::SharedResourcePointer<PluginShared> s;
  {
    const std::lock_guard lock(s->romMutex);
    s->rom = std::move(bytes);
    s->romFile = file;
  }
  s->props.getUserSettings()->setValue("romPath", file.getFullPathName());
  s->props.saveIfNeeded();
  return true;
}
