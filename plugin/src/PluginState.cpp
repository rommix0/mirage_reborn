#include "PluginState.h"

namespace {
constexpr int kVersion = 1;
const juce::Identifier kRoot("MirageState"), kDisk("disk"), kEditor("editor");
}  // namespace

juce::MemoryBlock encodePluginState(const PluginStateData& d) {
  juce::ValueTree t(kRoot);
  t.setProperty("version", kVersion, nullptr);
  t.setProperty("machine", d.machine, nullptr);
  t.setProperty("volume", d.volume, nullptr);
  t.setProperty("inputGain", d.inputGain, nullptr);
  juce::ValueTree disk(kDisk);
  disk.setProperty("path", d.diskPath, nullptr);
  disk.setProperty("format", d.diskFormat, nullptr);
  disk.setProperty("untitled", d.diskUntitled, nullptr);
  t.appendChild(disk, nullptr);
  juce::ValueTree editor(kEditor);
  editor.setProperty("width", d.editorW, nullptr);
  editor.setProperty("height", d.editorH, nullptr);
  t.appendChild(editor, nullptr);

  juce::MemoryOutputStream out;
  {
    juce::GZIPCompressorOutputStream gz(out);
    t.writeToStream(gz);
  }
  return out.getMemoryBlock();
}

std::optional<PluginStateData> decodePluginState(const void* data, size_t size) {
  if (data == nullptr || size == 0) return std::nullopt;
  juce::MemoryBlock raw;
  {
    juce::MemoryInputStream in(data, size, false);
    juce::GZIPDecompressorInputStream gz(in);
    gz.readIntoMemoryBlock(raw);
  }
  const auto t = juce::ValueTree::readFromData(raw.getData(), raw.getSize());
  if (!t.hasType(kRoot) || !t["version"].isInt() || (int)t["version"] != kVersion) return std::nullopt;
  // A cut-off stream still parses (missing values read as 0), so a whole one
  // is one that writes back to exactly the bytes it was read from.
  juce::MemoryOutputStream again;
  t.writeToStream(again);
  if (again.getDataSize() != raw.getSize() || std::memcmp(again.getData(), raw.getData(), raw.getSize()) != 0)
    return std::nullopt;
  const auto* machine = t["machine"].getBinaryData();
  const auto disk = t.getChildWithName(kDisk), editor = t.getChildWithName(kEditor);
  if (machine == nullptr || !disk.isValid() || !editor.isValid()) return std::nullopt;

  PluginStateData d;
  d.machine = *machine;
  d.diskPath = disk["path"].toString();
  d.diskFormat = disk["format"];
  d.diskUntitled = disk["untitled"];
  d.volume = juce::jlimit(0.0f, 1.0f, (float)t["volume"]);
  d.inputGain = juce::jlimit(0.0f, 4.0f, (float)t["inputGain"]);
  d.editorW = editor["width"];
  d.editorH = editor["height"];
  return d;
}
