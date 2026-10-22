#include <juce_gui_extra/juce_gui_extra.h>

#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "AudioEngine.h"
#include "ImportDialog.h"
#include "MainWindow.h"
#include "PanelComponent.h"
#include "Settings.h"
#include "mirage/boot_rom.h"
#include "mirage/realtime_host.h"
#include "mirage/wav_import.h"

namespace {

constexpr juce::int64 kBootRomBytes = 4096;

juce::String sha1Hex(const std::array<uint8_t, 20>& sha) {
  juce::String s;
  for (auto b : sha) s << juce::String::toHexString((int)b).paddedLeft('0', 2);
  return s;
}

bool writePng(const juce::Image& image, const juce::String& path) {
  const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(path);
  file.deleteFile();
  juce::FileOutputStream out(file);
  return out.openedOk() && juce::PNGImageFormat().writeImageToStream(image, out);
}

// Renders the front panel at 1200 px wide to a PNG (--panel-png), showing a
// fixed sample state: "2." "1" on the display (the left dot is the Param
// indicator), a disk in and the drive LED on. Needs neither an audio device nor
// the ROM.
int renderPanelPng(const juce::String& path) {
  mirage::HostStatus status;
  status.display = {0xDB, 0x06};
  status.driveActive = true;
  status.diskPresent = true;
  PanelComponent panel(status, "Sound Disk 1.img");
  const int w = 1200;
  panel.setSize(w, juce::roundToInt(w * (double)panel.getHeight() / panel.getWidth()));
  return writePng(panel.createComponentSnapshot(panel.getLocalBounds(), true, 1.0f), path) ? 0 : 1;
}

// Renders the Import WAV dialog for `wavPath` with its default settings to a
// PNG (--import-dialog-png), for reviewing its layout. Needs neither an audio
// device nor the ROM. Exit code 1 if the WAV cannot be read or the PNG written.
int renderImportDialogPng(const juce::String& wavPath, const juce::String& pngPath) {
  const auto wav = juce::File::getCurrentWorkingDirectory().getChildFile(wavPath);
  std::shared_ptr<const mirage::WavAudio> audio;
  try {
    audio = std::make_shared<const mirage::WavAudio>(mirage::readWavFile(std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(wav.getFullPathName().toRawUTF8())))));
  } catch (const std::exception&) {
    return 1;
  }
  ImportDialog dialog(audio, wav.getFileName(), {});
  return writePng(dialog.createComponentSnapshot(dialog.getLocalBounds(), true, 1.0f), pngPath) ? 0
                                                                                                 : 1;
}

}  // namespace

class MirageApplication : public juce::JUCEApplication {
 public:
  const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
  const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
  bool moreThanOneInstanceAllowed() override { return true; }

  void initialise(const juce::String&) override {
    const auto args = getCommandLineParameterArray();
    const int flag = args.indexOf("--panel-png");
    if (flag >= 0) {
      setApplicationReturnValue(flag + 1 < args.size() ? renderPanelPng(args[flag + 1]) : 2);
      quit();
      return;
    }
    if (const int f = args.indexOf("--import-dialog-png"); f >= 0) {
      setApplicationReturnValue(
          f + 2 < args.size() ? renderImportDialogPng(args[f + 1], args[f + 2]) : 2);
      quit();
      return;
    }
    // Boot ROM: the saved path, then the default location, then ask.
    const auto defaultRom = juce::File(MIRAGE_ASSET_ROOT)
                                .getChildFile("Ensoniq Mirage EPROM Firmware")
                                .getChildFile("mirage.bin");
    if (settings_.romPath().isNotEmpty() && tryRom(juce::File(settings_.romPath()))) return;
    if (tryRom(defaultRom)) return;
    chooseRom();
  }

  void shutdown() override {
    if (engine_ != nullptr) {
      // The MIDI devices are saved when the user picks them (MainWindow), so
      // one absent at this launch stays saved.
      settings_.setAudioState(engine_->saveAudioState().get());
      settings_.setVolume(engine_->volume());
      settings_.setInputGain(engine_->inputGain());
    }
    if (window_ != nullptr) settings_.setWindowState(window_->getWindowStateAsString());
    settings_.save();
    // Window first (it uses the engine), the engine before the host it feeds.
    window_.reset();
    engine_.reset();
    host_.reset();
  }

  // The close button, File > Exit and Windows ending the session all come here.
  // With a window, it asks about unsaved disk changes first and quits only once
  // they are saved or discarded. At logoff (WM_QUERYENDSESSION) JUCE returns
  // at once: if the prompt is showing, Windows sees the app hold the session
  // and offers to end it anyway; it never waits on us.
  void systemRequestedQuit() override {
    if (window_ != nullptr)
      window_->requestQuit();
    else
      quit();
  }

 private:
  // Starts the machine from `file` if it is a 4096-byte ROM.
  bool tryRom(const juce::File& file) {
    if (!file.existsAsFile() || file.getSize() != kBootRomBytes) return false;
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes) || bytes.getSize() != (size_t)kBootRomBytes) return false;
    host_ = std::make_unique<mirage::RealtimeHost>(
        std::span<const uint8_t>(static_cast<const uint8_t*>(bytes.getData()), bytes.getSize()));
    settings_.setRomPath(file.getFullPathName());
    startMachine();
    return true;
  }

  void chooseRom() {
    chooser_ = std::make_unique<juce::FileChooser>("Locate the Mirage boot ROM (mirage.bin)",
                                                   juce::File(MIRAGE_ASSET_ROOT), "*.bin;*.rom;*");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                            const auto file = fc.getResult();
                            if (file == juce::File()) return quit();
                            if (file.getSize() == kBootRomBytes && tryRom(file)) return;
                            refuseRom(file);
                          });
  }

  void refuseRom(const juce::File& file) {
    const auto text = file.getFileName() + " is " + juce::String(file.getSize()) +
                      " bytes; the Mirage boot ROM (mirage.bin) is exactly 4096 bytes.";
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::WarningIcon,
                                                     "Not a boot ROM", text, "Choose again", "Quit"),
        [this](int choice) {
          if (choice == 1)
            chooseRom();
          else
            quit();
        });
  }

  void startMachine() {
    engine_ = std::make_unique<AudioEngine>(*host_);
    engine_->setVolume(settings_.volume());
    engine_->setInputGain(settings_.inputGain());
    const auto saved = settings_.audioState();
    const auto error = engine_->start(saved.get());
    if (error.isNotEmpty())
      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Audio device",
                                             error);
    engine_->setMidiInput(settings_.midiIn());
    engine_->setMidiOutput(settings_.midiOut());

    window_ = std::make_unique<MainWindow>(*host_, *engine_, settings_);
    if (settings_.windowState().isNotEmpty())
      window_->restoreWindowStateFromString(settings_.windowState());

    if (!mirage::isKnownBootRom(host_->romSha1()))
      juce::AlertWindow::showMessageBoxAsync(
          juce::MessageBoxIconType::WarningIcon, "Unknown boot ROM",
          "This ROM's SHA-1 is " + sha1Hex(host_->romSha1()) + ".\nThe known Mirage boot ROM is " +
              sha1Hex(mirage::kKnownBootRomSha1) + ".\nIt may not behave like the real machine.");
  }

  Settings settings_;
  std::unique_ptr<juce::FileChooser> chooser_;
  // Declared in the order they are created; shutdown() destroys them in reverse.
  std::unique_ptr<mirage::RealtimeHost> host_;
  std::unique_ptr<AudioEngine> engine_;
  std::unique_ptr<MainWindow> window_;
};

START_JUCE_APPLICATION(MirageApplication)
