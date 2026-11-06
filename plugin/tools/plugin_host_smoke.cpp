// plugin_host_smoke <Mirage.vst3> <rom> <SND-A01.img> [--editor-png <out.png>]
// plugin_host_smoke <Mirage.vst3> --midi-out
// Loads the built plugin through JUCE's own VST3 hosting and checks its buses,
// its state before prepareToPlay, a restored SND-A01 playing a note, a state
// round trip into a second instance (bit-identical output) and the blobs it
// cannot restore (silence, and the same bytes back). With --editor-png it
// instead opens the editor of a restored SND-A01 in a window all but off
// screen, sizes it to 1200 px wide, checks it keeps its aspect ratio and writes
// a capture of the window to <out.png>. With --midi-out it runs a synthetic ROM
// that transmits known MIDI (midiOut()); it needs no asset. Exit 0 on success,
// 1 on a failed check (the reason on stderr), 2 for usage.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "PluginState.h"
#include "mirage/clocks.h"
#include "mirage/disk_image.h"
#include "mirage/machine.h"
#include "non_interactive.h"  // after JUCE: it includes windows.h

namespace {

struct Failure {
  std::string why;
};

void check(bool ok, const juce::String& why) {
  if (!ok) throw Failure{why.toStdString()};
}

void pass(const juce::String& what) { std::cout << "ok: " << what << std::endl; }

// Deletes a temporary file or folder however the scope ends.
struct TempFile {
  juce::File file;
  ~TempFile() { file.deleteRecursively(); }
};

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

// JUCE's VST3 host wraps the component state in the XML "VST3PluginState"
// (base64), and the JUCE VST3 wrapper appends its private data to it:
// [the plugin's blob][int64 0][private data][int64 its size]"JUCEPrivateData".
juce::MemoryBlock wrapForHost(const juce::MemoryBlock& blob) {
  juce::XmlElement x("VST3PluginState");
  x.createNewChildElement("IComponent")->addTextElement(blob.toBase64Encoding());
  juce::MemoryBlock out;
  juce::AudioProcessor::copyXmlToBinary(x, out);
  return out;
}

juce::MemoryBlock unwrapFromHost(const juce::MemoryBlock& state) {
  const auto x = juce::AudioProcessor::getXmlFromBinary(state.getData(), (int)state.getSize());
  check(x != nullptr, "the host state is not VST3PluginState XML");
  const auto* c = x->getChildByName("IComponent");
  check(c != nullptr, "the host state has no IComponent");
  juce::MemoryBlock mb;
  check(mb.fromBase64Encoding(c->getAllSubText()), "the IComponent state is not base64");
  static constexpr char kMagic[] = "JUCEPrivateData";
  const size_t m = sizeof(kMagic) - 1, size = mb.getSize();
  const auto* bytes = static_cast<const char*>(mb.getData());
  if (size >= m + 16 && std::memcmp(bytes + size - m, kMagic, m) == 0) {
    juce::int64 priv = 0;
    std::memcpy(&priv, bytes + size - m - 8, 8);
    check(priv >= 0 && (size_t)priv + m + 16 <= size, "bad JUCE private data size");
    mb.setSize(size - m - 16 - (size_t)priv);
  }
  return mb;
}

juce::MemoryBlock getState(juce::AudioPluginInstance& p) {
  juce::MemoryBlock mb;
  p.getStateInformation(mb);
  return unwrapFromHost(mb);
}

void setState(juce::AudioPluginInstance& p, const juce::MemoryBlock& blob) {
  const auto wrapped = wrapForHost(blob);
  p.setStateInformation(wrapped.getData(), (int)wrapped.getSize());
}

void prepare(juce::AudioPluginInstance& p) {
  p.setNonRealtime(true);
  p.prepareToPlay(kRate, kBlock);
}

// Renders `seconds` in 512-frame blocks with note 60 on at 0.5 s and off at
// 1.5 s; returns channel 0 after checking channel 1 is the same.
std::vector<float> render(juce::AudioPluginInstance& p, double seconds) {
  juce::AudioBuffer<float> buf(2, kBlock);
  juce::MidiBuffer midi;
  std::vector<float> out;
  const int total = (int)(seconds * kRate), on = (int)(0.5 * kRate), off = (int)(1.5 * kRate);
  for (int pos = 0; pos < total; pos += kBlock) {
    buf.clear();
    midi.clear();
    if (on >= pos && on < pos + kBlock) midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), on - pos);
    if (off >= pos && off < pos + kBlock) midi.addEvent(juce::MidiMessage::noteOff(1, 60), off - pos);
    p.processBlock(buf, midi);
    check(std::memcmp(buf.getReadPointer(0), buf.getReadPointer(1), sizeof(float) * kBlock) == 0,
          "the two output channels differ");
    out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + kBlock);
  }
  return out;
}

float peak(const std::vector<float>& v) {
  float p = 0;
  for (float s : v) p = std::max(p, std::abs(s));
  return p;
}

juce::MemoryBlock gzipTree(const juce::ValueTree& t) {
  juce::MemoryOutputStream mo;
  {
    juce::GZIPCompressorOutputStream gz(mo);
    t.writeToStream(gz);
  }
  return mo.getMemoryBlock();
}

juce::ValueTree gunzipTree(const juce::MemoryBlock& b) {
  juce::MemoryInputStream mi(b, false);
  juce::GZIPDecompressorInputStream gz(mi);
  return juce::ValueTree::readFromStream(gz);
}

juce::MemoryBlock snapshotBlob(const std::vector<uint8_t>& snapshot) {
  PluginStateData d;
  d.machine = juce::MemoryBlock(snapshot.data(), snapshot.size());
  return encodePluginState(d);
}

bool hasParameterNamed(juce::AudioPluginInstance& p, const juce::String& name) {
  for (auto* param : p.getParameters())
    if (param->getName(100) == name) return true;
  return false;
}

std::unique_ptr<juce::AudioPluginInstance> makeInstance(const juce::File& pluginFile) {
  juce::VST3PluginFormat format;
  juce::OwnedArray<juce::PluginDescription> types;
  format.findAllTypesForFile(types, pluginFile.getFullPathName());
  check(types.size() == 1, "expected one plugin in " + pluginFile.getFullPathName() + ", found " +
                               juce::String(types.size()));
  juce::String error;
  auto p = format.createInstanceFromDescription(*types[0], kRate, kBlock, error);
  check(p != nullptr, "cannot create the plugin: " + error);
  return p;
}

juce::MemoryBlock loadRom(const juce::File& romFile) {
  juce::MemoryBlock rom;
  check(romFile.loadFileAsData(rom) && rom.getSize() == mirage::Machine::kBootRomSize,
        romFile.getFullPathName() + ": cannot read a 4096-byte boot ROM");
  return rom;
}

// SND-A01 booted headless to 16 s: its first sound is playable.
std::vector<uint8_t> bootSnapshot(std::span<const uint8_t> romBytes, const juce::File& diskFile) {
  mirage::Machine m(romBytes);
  m.insertDisk(mirage::DiskImage::loadFile(diskFile.getFullPathName().toStdString()));
  m.powerOn();
  m.runCycles(16 * mirage::kCpuClockHz);
  return m.saveState();
}

void run(const juce::File& pluginFile, const juce::File& romFile, const juce::File& diskFile) {
  const auto rom = loadRom(romFile);
  const std::span<const uint8_t> romBytes(static_cast<const uint8_t*>(rom.getData()), rom.getSize());

  // decodePluginState on its own: a round trip, and what it must refuse.
  {
    PluginStateData d;
    d.machine = juce::MemoryBlock("snapshot", 8);
    d.diskPath = "C:/disks/SND-A01.img";
    d.diskFormat = 1;
    d.diskUntitled = true;
    d.volume = 0.25f;
    d.inputGain = 2.5f;
    d.editorW = 900;
    d.editorH = 450;
    const auto blob = encodePluginState(d);
    const auto back = decodePluginState(blob.getData(), blob.getSize());
    check(back.has_value() && back->machine == d.machine && back->diskPath == d.diskPath &&
              back->diskFormat == 1 && back->diskUntitled && back->volume == 0.25f &&
              back->inputGain == 2.5f && back->editorW == 900 && back->editorH == 450,
          "encode/decode does not round-trip");
    const juce::MemoryBlock garbage("this is not a Mirage state at all", 33);
    check(!decodePluginState(garbage.getData(), garbage.getSize()), "garbage decodes");
    check(!decodePluginState(blob.getData(), blob.getSize() / 2), "a truncated blob decodes");
    auto tree = gunzipTree(blob);
    check(tree.isValid(), "the blob is not a GZIP ValueTree");
    tree.setProperty("version", 2, nullptr);
    const auto v2 = gzipTree(tree);
    check(!decodePluginState(v2.getData(), v2.getSize()), "a version-2 blob decodes");
    pass("decodePluginState");
  }

  const auto make = [&] { return makeInstance(pluginFile); };

  // 1. Buses, MIDI and parameters.
  auto a = make();
  check(a->getBusCount(false) == 1 && a->getBus(false, 0)->getCurrentLayout() == juce::AudioChannelSet::stereo(),
        "expected one stereo output bus");
  check(a->getBusCount(true) == 1 && !a->getBus(true, 0)->isEnabled(),
        "expected one input bus (the jack), disabled by default");
  check(a->acceptsMidi() && a->producesMidi(), "expected MIDI in and out");
  check(hasParameterNamed(*a, "Volume") && hasParameterNamed(*a, "Input Gain"),
        "expected the parameters Volume and Input Gain");
  pass("buses, MIDI and parameters");

  // 2. The state before prepareToPlay (a project saved before audio ever ran).
  {
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto s0 = getState(*a);
    const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
    const auto d = decodePluginState(s0.getData(), s0.getSize());
    check(d.has_value() && d->machine.getSize() > 0, "the state before prepareToPlay holds no machine");
    check(ms < 1500, "the state before prepareToPlay took " + juce::String(ms) + " ms");
    pass("state before prepareToPlay (" + juce::String(juce::roundToInt(ms)) + " ms)");
  }

  // 3. SND-A01 booted headless to 16 s, restored into the plugin.
  const auto booted = bootSnapshot(romBytes, diskFile);
  setState(*a, snapshotBlob(booted));

  // 4. It plays.
  prepare(*a);
  const float p4 = peak(render(*a, 2.0));
  check(p4 > 0.05f, "the restored SND-A01 is silent (peak " + juce::String(p4) + ")");
  pass("restored SND-A01 plays (peak " + juce::String(p4, 3) + ")");

  // 5. A state round trip into a second instance renders the same samples.
  {
    const auto sa = getState(*a);
    auto b = make();
    setState(*b, sa);
    prepare(*b);
    const auto outA = render(*a, 2.0), outB = render(*b, 2.0);
    check(peak(outA) > 0.05f, "the second render of the first instance is silent");
    check(outA == outB, "the restored instance renders different samples");
    pass("state round trip is bit-identical");
  }

  // 5b. The disk identity survives a restore: the state of an instance restored
  // with a disk path and format hands them back.
  {
    const TempFile guard{juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getNonexistentChildFile("plugin_smoke_SND-A01", ".img")};
    const auto& tmp = guard.file;
    check(diskFile.copyFileTo(tmp), "cannot copy SND-A01 to " + tmp.getFullPathName());
    PluginStateData d;
    d.machine = juce::MemoryBlock(booted.data(), booted.size());
    d.diskPath = tmp.getFullPathName();
    d.diskFormat = (int)mirage::DiskFormat::Raw;
    auto c = make();
    setState(*c, encodePluginState(d));
    prepare(*c);
    render(*c, 0.5);
    const auto back = getState(*c);
    const auto got = decodePluginState(back.getData(), back.getSize());
    c->releaseResources();
    c.reset();
    check(got.has_value() && got->diskPath == d.diskPath && got->diskFormat == d.diskFormat && !got->diskUntitled,
          "the disk identity did not survive (got path '" + (got ? got->diskPath : juce::String()) + "')");
    pass("disk identity survives a restore");
  }

  // 6. Blobs that cannot be restored: silence, and the same bytes back.
  {
    const auto good = getState(*a);
    auto tree = gunzipTree(good);
    tree.setProperty("version", 2, nullptr);
    auto wrongRom = rom;
    static_cast<uint8_t*>(wrongRom.getData())[0] ^= 0xFF;
    mirage::Machine other(std::span<const uint8_t>(static_cast<const uint8_t*>(wrongRom.getData()), wrongRom.getSize()));
    other.powerOn();
    const struct {
      const char* name;
      juce::MemoryBlock blob;
    } bad[] = {
        {"garbage", juce::MemoryBlock("not a Mirage state, just some bytes", 35)},
        {"truncated", juce::MemoryBlock(good.getData(), good.getSize() / 2)},
        {"version 2", gzipTree(tree)},
        {"wrong ROM", snapshotBlob(other.saveState())},
    };
    for (const auto& [name, blob] : bad) {
      auto c = make();
      setState(*c, blob);
      prepare(*c);
      const float pc = peak(render(*c, 1.0));
      check(pc == 0.0f, juce::String(name) + ": the plugin is not silent (peak " + juce::String(pc) + ")");
      check(getState(*c) == blob, juce::String(name) + ": getStateInformation does not return the blob");
      c->releaseResources();
      pass(juce::String(name) + " blob kept, silent");
    }
  }
  a->releaseResources();
}

// The editor capture is Win32 (window messages, PrintWindow); other platforms
// skip --editor-png. ponytail: an X11 capture would bring it to Linux.
#ifdef _WIN32
// Dispatches this thread's window messages for `ms`. Both JUCE copies, the
// tool's and the plugin's, post theirs to this thread.
void pump(int ms) {
  const auto end = juce::Time::getMillisecondCounter() + (juce::uint32)ms;
  while (juce::Time::getMillisecondCounter() < end) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(5);
  }
}

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// The window's client area as the compositor draws it. The plugin's editor is
// a child window of another JUCE copy, so createComponentSnapshot cannot see it.
juce::Image captureWindow(HWND hwnd) {
  RECT r{};
  GetClientRect(hwnd, &r);
  const int w = r.right - r.left, h = r.bottom - r.top;
  check(w > 0 && h > 0, "the editor window has no area");
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // top-down rows
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  const auto old = SelectObject(mem, bmp);
  const bool printed = PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != 0;
  GdiFlush();
  juce::Image img(juce::Image::RGB, w, h, false);
  {
    const juce::Image::BitmapData d(img, juce::Image::BitmapData::writeOnly);
    const auto* src = static_cast<const juce::uint8*>(bits);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const auto* px = src + ((size_t)y * (size_t)w + (size_t)x) * 4;  // B, G, R, unused
        d.setPixelColour(x, y, juce::Colour(px[2], px[1], px[0]));
      }
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  check(printed, "PrintWindow failed");
  return img;
}

bool sameImage(const juce::Image& a, const juce::Image& b) {
  if (!a.isValid() || !b.isValid() || a.getBounds() != b.getBounds()) return false;
  for (int y = 0; y < a.getHeight(); ++y)
    for (int x = 0; x < a.getWidth(); ++x)
      if (a.getPixelAt(x, y) != b.getPixelAt(x, y)) return false;
  return true;
}

// Row y is painted: under 5% of it is the black the host and the VST3
// wrapper fill a window with before (or outside) the plugin's editor paints.
bool rowPainted(const juce::Image& img, int y) {
  int black = 0;
  for (int x = 0; x < img.getWidth(); ++x)
    if ((img.getPixelAt(x, y).getARGB() & 0xFFFFFF) == 0) ++black;
  return black * 20 < img.getWidth();
}

// The editor of an instance restored as in run() (SND-A01 at 16 s), with the
// disk's identity (a temporary copy named SND-A01.img), on the desktop with
// one pixel on screen and sized to 1200 px wide. Its capture is written to `png`.
void editorPng(const juce::File& pluginFile, const juce::File& romFile, const juce::File& diskFile,
               const juce::File& png) {
  const auto rom = loadRom(romFile);
  const auto booted =
      bootSnapshot(std::span<const uint8_t>(static_cast<const uint8_t*>(rom.getData()), rom.getSize()), diskFile);
  const TempFile guard{
      juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("plugin_editor_smoke", "")};
  const auto& dir = guard.file;
  check(dir.createDirectory(), "cannot create " + dir.getFullPathName());
  const auto disk = dir.getChildFile("SND-A01.img");
  check(diskFile.copyFileTo(disk), "cannot copy SND-A01 to " + disk.getFullPathName());
  PluginStateData d;
  d.machine = juce::MemoryBlock(booted.data(), booted.size());
  d.diskPath = disk.getFullPathName();
  d.diskFormat = (int)mirage::DiskFormat::Raw;

  auto p = makeInstance(pluginFile);
  setState(*p, encodePluginState(d));
  prepare(*p);
  render(*p, 0.5);  // publishes the display and the drive status
  {
    std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditorIfNeeded());
    check(editor != nullptr, "the plugin has no editor");
    // One pixel on screen, in the corner: a window wholly off screen is never
    // repainted by the plugin's renderer.
    const auto corner = [&] { editor->setTopLeftPosition(1 - editor->getWidth(), 1 - editor->getHeight()); };
    corner();
    editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);
    // A square: the editor's fixed aspect ratio takes it back to its own height.
    editor->setSize(1200, 1200);
    corner();
    // Captures each 100 ms until two in a row are the same and both the menu
    // bar (top) and the status strip (bottom) are painted: the editor has
    // settled at its new size. A stale frame of the old size leaves the
    // bottom rows black.
    juce::Image img, last;
    bool settled = false;
    for (const auto end = juce::Time::getMillisecondCounter() + 10000;
         !settled && juce::Time::getMillisecondCounter() < end;) {
      pump(100);
      last = std::exchange(img, captureWindow((HWND)editor->getPeer()->getNativeHandle()));
      const int top = img.getHeight() * 12 / std::max(editor->getHeight(), 1);  // the menu bar's middle
      settled = sameImage(img, last) && rowPainted(img, top) && rowPainted(img, img.getHeight() - 1 - top);
    }
    const int w = editor->getWidth(), h = editor->getHeight();
    check(w == 1200 && h > 300 && h < 800, "the editor did not keep its aspect ratio at 1200 px wide (got " +
                                               juce::String(w) + " x " + juce::String(h) + ")");
    check(settled, "the editor capture did not settle within 10 s with its menu bar and strip painted "
                   "(a stale or blank frame)");
    if (img.getWidth() != w)  // a scaled display: the capture is in physical pixels
      img = img.rescaled(w, juce::roundToInt(img.getHeight() * (double)w / img.getWidth()),
                         juce::Graphics::highResamplingQuality);
    png.deleteFile();
    juce::FileOutputStream out(png);
    check(out.openedOk() && juce::PNGImageFormat().writeImageToStream(img, out),
          "cannot write " + png.getFullPathName());
    pass("editor " + juce::String(w) + " x " + juce::String(h) + " written to " + png.getFullPathName());
  }
  p->releaseResources();
}
#endif  // _WIN32

// The synthetic ROM of --midi-out. It transmits, as fast as the ACIA takes
// them (one byte per 640 cycles), kMidiOutHead, a SysEx of 65,535 bytes (the
// most a juce::MidiBuffer event holds), one of 65,536 (the plugin drops it),
// then kMidiOutTail, and idles. A SysEx's data bytes count 00..7F, wrapping.
const std::vector<uint8_t> kMidiOutHead = {0x90, 0x3C, 0x64, 0xB0, 0x07, 0x64, 0xC0, 0x05, 0x80, 0x3C, 0x40};
const std::vector<uint8_t> kMidiOutTail = {0x90, 0x40, 0x64, 0x43, 0x50,  // the second by running status
                                           0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7, 0x80, 0x40, 0x00};
constexpr size_t kLongSysex = 65535;
constexpr double kByteCycles = 640;  // the ACIA at 31,250 baud

std::vector<uint8_t> midiOutRom() {
  std::vector<uint8_t> rom(mirage::Machine::kBootRomSize, 0x12);  // NOP
  const auto put = [&](uint16_t at, std::initializer_list<uint8_t> code) {
    std::copy(code.begin(), code.end(), rom.begin() + (at - 0xF000));
  };
  put(0xF000, {0x86, 0x03, 0xB7, 0xE1, 0x00,  // LDA #$03; STA $E100 (ACIA reset)
               0x10, 0xCE, 0x8F, 0x00,        // LDS #$8F00
               0x8E, 0xF2, 0x00,              // LDX #head
               0xBD, 0xF1, 0x00,              // JSR sendtab
               0x10, 0x8E, 0xFF, 0xFD,        // LDY #65533
               0xBD, 0xF1, 0x20,              // JSR sendlong
               0x10, 0x8E, 0xFF, 0xFE,        // LDY #65534
               0xBD, 0xF1, 0x20,              // JSR sendlong
               0x8E, 0xF3, 0x00,              // LDX #tail
               0xBD, 0xF1, 0x00,              // JSR sendtab
               0x20, 0xFE});                  // BRA *
  put(0xF100, {0xE6, 0x80,                    // sendtab: LDB ,X+
               0xC1, 0xFF,                    // CMPB #$FF (the end)
               0x27, 0x05,                    // BEQ ret
               0xBD, 0xF1, 0x40,              // JSR send
               0x20, 0xF5,                    // BRA sendtab
               0x39});                        // ret: RTS
  put(0xF120, {0xC6, 0xF0, 0xBD, 0xF1, 0x40,  // sendlong: LDB #$F0; JSR send
               0x4F,                          // CLRA
               0x1F, 0x89,                    // loop: TFR A,B
               0xC4, 0x7F,                    // ANDB #$7F
               0xBD, 0xF1, 0x40,              // JSR send
               0x4C,                          // INCA
               0x31, 0x3F,                    // LEAY -1,Y
               0x26, 0xF4,                    // BNE loop
               0xC6, 0xF7, 0xBD, 0xF1, 0x40,  // LDB #$F7; JSR send
               0x39});                        // RTS
  put(0xF140, {0x34, 0x02,                    // send: PSHS A
               0xB6, 0xE1, 0x00,              // wait: LDA $E100
               0x85, 0x02,                    // BITA #$02 (TDRE)
               0x27, 0xF9,                    // BEQ wait
               0xF7, 0xE1, 0x01,              // STB $E101
               0x35, 0x82});                  // PULS A,PC
  std::copy(kMidiOutHead.begin(), kMidiOutHead.end(), rom.begin() + 0x200);
  rom[0x200 + kMidiOutHead.size()] = 0xFF;
  std::copy(kMidiOutTail.begin(), kMidiOutTail.end(), rom.begin() + 0x300);
  rom[0x300 + kMidiOutTail.size()] = 0xFF;
  rom[0xFFE] = 0xF0;  // the reset vector: $F000
  rom[0xFFF] = 0x00;
  return rom;
}

// MIDI out lands whole and in order, each message inside its block, in frame
// order, and near the moment its last byte was sent.
void midiOut(const juce::File& pluginFile) {
  struct Expected {
    std::vector<uint8_t> bytes;
    size_t lastByte;  // the index of its last byte in all the ROM transmits
  };
  std::vector<Expected> expected;
  size_t sent = 0;
  const auto add = [&](const std::vector<uint8_t>& bytes, size_t lastByte) { expected.push_back({bytes, lastByte}); };
  add({0x90, 0x3C, 0x64}, 2);
  add({0xB0, 0x07, 0x64}, 5);
  add({0xC0, 0x05}, 7);
  add({0x80, 0x3C, 0x40}, 10);
  sent = kMidiOutHead.size();
  std::vector<uint8_t> sysex(kLongSysex);
  sysex.front() = 0xF0;
  for (size_t i = 1; i + 1 < kLongSysex; ++i) sysex[i] = uint8_t((i - 1) & 0x7F);
  sysex.back() = 0xF7;
  add(sysex, sent + kLongSysex - 1);
  sent += kLongSysex + kLongSysex + 1;  // and the one dropped
  add({0x90, 0x40, 0x64}, sent + 2);
  add({0x90, 0x43, 0x50}, sent + 4);
  add({0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7}, sent + 10);
  add({0x80, 0x40, 0x00}, sent + 13);
  sent += kMidiOutTail.size();

  auto p = makeInstance(pluginFile);
  prepare(*p);
  juce::AudioBuffer<float> buf(2, kBlock);
  juce::MidiBuffer midi;
  size_t got = 0;
  double worst = 0;
  // Every byte's time, and a tenth of a second more.
  const auto blocks = (int)(((double)sent * kByteCycles / mirage::kCpuClockHz + 0.1) * kRate / kBlock);
  for (int block = 0; block < blocks; ++block) {
    buf.clear();
    midi.clear();
    p->processBlock(buf, midi);
    int prev = 0;
    for (const auto m : midi) {
      const juce::String at = "block " + juce::String(block) + " frame " + juce::String(m.samplePosition);
      check(m.samplePosition >= 0 && m.samplePosition < kBlock, "a message outside its block (" + at + ")");
      check(m.samplePosition >= prev, "messages out of frame order (" + at + ")");
      prev = m.samplePosition;
      check(got < expected.size(), "an unexpected extra message (" + at + ")");
      const auto& e = expected[got++];
      check((size_t)m.numBytes == e.bytes.size() && std::equal(e.bytes.begin(), e.bytes.end(), m.data),
            "message " + juce::String((int)got) + " is not the one sent (" + juce::String(m.numBytes) + " bytes, " +
                at + ")");
      // When its last byte was sent, as a sample: within 2 ms (a byte takes
      // 0.32 ms; the output resampler's delay is about 1 ms).
      const double want = (double)(e.lastByte + 1) * kByteCycles / mirage::kCpuClockHz * kRate;
      const double off = std::abs((double)block * kBlock + m.samplePosition - want);
      worst = std::max(worst, off);
      check(off < 0.002 * kRate, "message " + juce::String((int)got) + " is " + juce::String(off, 1) +
                                     " samples from when it was sent (" + at + ")");
    }
  }
  check(got == expected.size(),
        "got " + juce::String((int)got) + " of " + juce::String((int)expected.size()) + " messages");
  p->releaseResources();
  pass(juce::String((int)got) + " MIDI-out messages whole, in order and at their frames (at most " +
       juce::String(worst, 1) + " samples off); a 65,535-byte SysEx passed, a 65,536-byte one was dropped");
}

}  // namespace

int main(int argc, char** argv) {
  mirage::cli::makeNonInteractive();  // a Debug assertion reports on stderr, never in a dialog
  const bool editor = argc == 6 && std::string(argv[4]) == "--editor-png";
  const bool midi = argc == 3 && std::string(argv[2]) == "--midi-out";
  if (argc != 4 && !editor && !midi) {
    std::cerr << "usage: plugin_host_smoke <Mirage.vst3> <rom> <SND-A01.img> [--editor-png <out.png>]\n"
                 "       plugin_host_smoke <Mirage.vst3> --midi-out"
              << std::endl;
    return 2;
  }
  // --midi-out: the synthetic ROM, in a temporary file (the plugin accepts an
  // unknown ROM).
  const TempFile romGuard{midi ? juce::File::getSpecialLocation(juce::File::tempDirectory)
                                     .getNonexistentChildFile("plugin_smoke_midi_out", ".bin")
                               : juce::File()};
  if (midi) {
    const auto bytes = midiOutRom();
    if (!romGuard.file.replaceWithData(bytes.data(), bytes.size())) {
      std::cerr << "FAIL: cannot write " << romGuard.file.getFullPathName() << std::endl;
      return 1;
    }
  }
  // The plugin finds its ROM through this override first (PluginSettings.h), so
  // the run does not depend on the user's settings. Set before the first
  // instance: the plugin caches the ROM, once per process (so --midi-out is a
  // run of its own).
  const auto rom =
      midi ? romGuard.file.getFullPathName() : juce::File(juce::String::fromUTF8(argv[2])).getFullPathName();
#ifdef _WIN32
  SetEnvironmentVariableW(L"MIRAGE_PLUGIN_ROM", rom.toWideCharPointer());
#else
  setenv("MIRAGE_PLUGIN_ROM", rom.toRawUTF8(), 1);
#endif
  juce::ScopedJuceInitialiser_GUI juce;
  try {
    const auto arg = [&](int i) { return juce::File(juce::String::fromUTF8(argv[i])); };
    if (midi)
      midiOut(arg(1));
    else if (editor)
#ifdef _WIN32
      editorPng(arg(1), arg(2), arg(3), arg(5));
#else
      throw Failure{"--editor-png is Windows only"};
#endif
    else
      run(arg(1), arg(2), arg(3));
  } catch (const Failure& f) {
    std::cerr << "FAIL: " << f.why << std::endl;
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "plugin_host_smoke: all checks passed" << std::endl;
  return 0;
}
