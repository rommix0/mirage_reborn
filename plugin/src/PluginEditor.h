#pragma once
#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <memory>
#include <vector>

#include "DiskActions.h"
#include "InputMeter.h"
#include "PanelComponent.h"
#include "mirage/wav_import.h"

class MirageProcessor;

// The plugin's editor (design §4.5): a menu bar (File, Machine) above the app's
// replica panel and a status strip below it. The strip shows the disk title,
// an import's outcome, the warnings and the Input slider with its meter. The
// panel's volume and the Input slider are the `volume` and `inputGain`
// parameters. Resizable with a fixed aspect ratio; the size is saved with the
// project.
//
// Everything that must outlive the editor (the DiskSession and its poll, the
// returned wave writes) is the processor's, and closing the editor never
// prompts (§4.6). The disk work runs through MirageProcessor::withSession.
//
// With no boot ROM, a "Choose the Mirage boot ROM" button takes the panel's
// place; with a project's Mirage that could not be restored, its reason and a
// "Choose ROM" button do, and Reset, Open and New Blank ask "Discard the Mirage
// saved in this project?" first.
//
// Keyboard (as the app): the digits press 0..9, the up and down arrows On/Off,
// Return Enter and Escape Cancel, one press per physical press, released when
// the key comes up or the editor loses focus. Ignored while a dialog is open;
// Ctrl combinations go to the menus (when the DAW lets them through).
class MirageEditor : public juce::AudioProcessorEditor,
                     private juce::MenuBarModel,
                     private juce::ApplicationCommandTarget,
                     private juce::Timer {
 public:
  static constexpr int kMenuHeight = 24, kStripHeight = 26;
  static constexpr int kInputControlsWidth = 290;  // label, slider with its box, meter, the resize corner
  static constexpr juce::uint32 kNoticeMs = 15000;

  explicit MirageEditor(MirageProcessor&);
  ~MirageEditor() override;

  void paint(juce::Graphics&) override;
  void resized() override;
  bool keyPressed(const juce::KeyPress&) override;
  bool keyStateChanged(bool isKeyDown) override;
  // The host the panel and the disk actions were built for (the processor frees
  // a replaced host only once the editor has moved on).
  const mirage::RealtimeHost* shownHost() const { return host_; }

 private:
  enum CommandIds {
    cmdOpenDisk = 1,
    cmdNewBlankDisk,
    cmdSaveDisk,
    cmdSaveDiskAs,
    cmdEjectDisk,
    cmdImportWav,
    cmdReset,
    cmdPanic
  };

  // MenuBarModel
  juce::StringArray getMenuBarNames() override;
  juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
  void menuItemSelected(int, int) override {}
  // ApplicationCommandTarget
  ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
  void getAllCommands(juce::Array<juce::CommandID>& commands) override;
  void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override;
  bool perform(const InvocationInfo& info) override;

  void timerCallback() override;  // 30 Hz while keys are held
  void releaseHeldKeys(bool all);  // all, or only those no longer physically down
  bool dialogOpen() const;

  void tick();              // 10 Hz: the machine, save outcomes, notices, the strip
  void buildMachineView();  // the panel and the disk actions for the processor's host
  void updateView();        // the panel, or the ROM prompt in its place
  void refresh();           // the disk's name and title, menu enablement
  void chooseRom();
  // Runs `then` now, or once the user agrees to discard a project's Mirage that
  // could not be restored (which power-cycles the machine).
  void afterDiscard(std::function<void()> then);
  juce::File diskFolder() const;
  void showError(const juce::String& title, const juce::String& message);

  // Import WAV, as the app's (spec 6.7); the processor reports the outcome.
  bool canImportWav() const;  // blocks run and the drive LED is off
  void importWav();
  void showImportDialog(const juce::File& file);
  void postImport(const mirage::WavAudio& audio, const mirage::ImportSettings& settings);

  struct StripText {
    juce::String left, right;
    bool operator==(const StripText&) const = default;
  };
  StripText buildText() const;

  MirageProcessor& proc_;
  juce::ApplicationCommandManager commands_;
  juce::MenuBarComponent menuBar_;
  mirage::RealtimeHost* host_ = nullptr;  // the host the view was built for
  std::unique_ptr<PanelComponent> panel_;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volume_;  // goes before panel_
  std::unique_ptr<DiskActions> actions_;
  juce::Label message_;  // no ROM, or why the project's Mirage was not restored
  juce::TextButton romButton_;
  juce::Label inputLabel_{{}, "Input"};
  juce::Slider inputSlider_;
  juce::AudioProcessorValueTreeState::SliderAttachment inputGain_;
  InputMeter inputMeter_;
  juce::TimedCallback ticker_{[this] { tick(); }};

  struct HeldKey {
    int keyCode;  // juce::KeyPress key code, polled with isKeyCurrentlyDown
    mirage::PanelKey key;
  };
  std::vector<HeldKey> held_;  // keyboard keys holding panel keys
  std::unique_ptr<juce::FileChooser> chooser_;  // the ROM's and Import WAV's
  juce::Component::SafePointer<juce::DialogWindow> importDialog_;  // its callbacks use this editor

  // As tick() last saw them.
  juce::String problem_;  // MirageProcessor::restoreProblem()
  juce::String diskTitle_{"No disk"};
  juce::String notice_;
  juce::uint32 noticeUntil_ = 0;
  bool hasDisk_ = false, saving_ = false, canImport_ = false;
  uint64_t blocks_ = 0;           // HostStatus::blocks
  juce::uint32 blocksSeenMs_ = 0;  // when it last moved
  StripText text_;
};
