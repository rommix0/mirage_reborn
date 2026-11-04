#pragma once
#include <juce_data_structures/juce_data_structures.h>

#include <optional>

// The plugin's state in a DAW project (design §5.1): the machine snapshot, what
// DiskSession knows about the inserted disk, the two parameters and the editor
// size. Stored as the ValueTree "MirageState" (version 1), GZIP-compressed.
// Shared by the plugin and plugin_host_smoke.
struct PluginStateData {
  juce::MemoryBlock machine;  // Machine::saveState(); empty when no machine ran (no ROM)
  juce::String diskPath;      // DiskSession identity (only with a disk in the drive)
  int diskFormat = 0;         // mirage::DiskFormat
  bool diskUntitled = false;
  float volume = 1, inputGain = 1;
  int editorW = 0, editorH = 0;  // 0: never sized
};

juce::MemoryBlock encodePluginState(const PluginStateData&);
// nullopt for anything that is not a whole version-1 "MirageState" (garbage, a
// truncated GZIP stream, a newer version).
std::optional<PluginStateData> decodePluginState(const void* data, size_t size);
