#pragma once
#include <juce_data_structures/juce_data_structures.h>

#include <mutex>
#include <optional>

// What the instances in one process share: the plugin's settings file and the
// boot ROM. Each MirageProcessor holds a juce::SharedResourcePointer<PluginShared>,
// so it lives while any instance does.
struct PluginShared {
  PluginShared();
  juce::InterProcessLock lock{"MiragePluginSettings"};  // declared first: props uses it
  juce::ApplicationProperties props;
  std::mutex romMutex;
  std::optional<juce::MemoryBlock> rom;  // the cached ROM bytes
  juce::File romFile;
};

// The plugin's own settings, %APPDATA%\MirageEmulator\MiragePlugin.settings
// (romPath, lastDiskDir, lastWavDir), guarded by an inter-process lock. It
// never touches the app's file. Message thread; valid while a MirageProcessor
// exists.
juce::PropertiesFile& pluginSettings();

// The boot ROM (design §4.7), loaded once and shared by every instance: the ROM
// path in the plugin's settings, mirage.bin beside the .vst3 bundle, the app's
// saved romPath (read only), then MIRAGE_ROM. Only files of exactly 4096 bytes
// are read. `found` is set to the file used. nullopt when none is found (the
// editor then asks for it).
std::optional<juce::MemoryBlock> findBootRom(juce::File& found);
// The editor's chooser: loads `file` (4096 bytes) as the shared ROM and saves
// its path in the settings. False, changing nothing, if it is not a ROM.
bool setBootRom(const juce::File& file);
