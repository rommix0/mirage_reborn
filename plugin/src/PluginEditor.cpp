#include "PluginEditor.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "ImportDialog.h"
#include "PluginProcessor.h"
#include "PluginSettings.h"

namespace {

// DiskSession speaks std::filesystem::path and UTF-8; JUCE speaks File and String.
std::filesystem::path toPath(const juce::File& file) {
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t*>(file.getFullPathName().toRawUTF8())));
}

juce::String fromUtf8(const std::string& s) { return juce::String::fromUTF8(s.data(), (int)s.size()); }

juce::String fileName(const std::filesystem::path& p) {
  const auto u = p.filename().u8string();
  return juce::String::fromUTF8(reinterpret_cast<const char*>(u.data()), (int)u.size());
}

// A folder saved in the plugin's settings, if it still exists.
juce::File savedFolder(const char* key) {
  const auto path = pluginSettings().getValue(key);
  return juce::File::isAbsolutePath(path) && juce::File(path).isDirectory() ? juce::File(path) : juce::File();
}

void saveFolder(const char* key, const juce::File& dir) {
  pluginSettings().setValue(key, dir.getFullPathName());
  pluginSettings().saveIfNeeded();
}

// The editor's natural size: the panel at its own size, the menu bar and the strip.
int naturalWidth() { return juce::roundToInt(mirage::panelGeometry().width); }
int naturalHeight() {
  return MirageEditor::kMenuHeight + juce::roundToInt(mirage::panelGeometry().height) + MirageEditor::kStripHeight;
}

}  // namespace

MirageEditor::MirageEditor(MirageProcessor& p)
    : AudioProcessorEditor(p), proc_(p), inputGain_(p.params, "inputGain", inputSlider_) {
  commands_.registerAllCommandsForTarget(this);
  commands_.setFirstCommandTarget(this);
  addKeyListener(commands_.getKeyMappings());
  menuBar_.setModel(this);
  addAndMakeVisible(menuBar_);

  message_.setColour(juce::Label::textColourId, juce::Colour(0xffc8c8c8));
  message_.setFont(juce::FontOptions(16.0f));
  message_.setJustificationType(juce::Justification::centred);
  addChildComponent(message_);
  romButton_.onClick = [this] { chooseRom(); };
  addChildComponent(romButton_);

  // Input: 0 to 4, with 1 (the default) in the middle of the slider (the parameter's range).
  inputLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffc8c8c8));
  inputLabel_.setFont(juce::FontOptions(14.0f));
  inputLabel_.setJustificationType(juce::Justification::centredRight);
  inputSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
  inputSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 20);
  inputSlider_.setNumDecimalPlacesToDisplay(2);
  inputSlider_.setDoubleClickReturnValue(true, 1.0);
  inputSlider_.setWantsKeyboardFocus(false);  // the arrow keys belong to Off/On
  addAndMakeVisible(inputLabel_);
  addAndMakeVisible(inputSlider_);
  addAndMakeVisible(inputMeter_);

  setWantsKeyboardFocus(true);
  // The saved size, read first: setResizeLimits resizes (and so saves) at once.
  const int w = proc_.editorWidth(), h = proc_.editorHeight();
  const int w0 = naturalWidth(), h0 = naturalHeight();
  setResizable(true, true);
  setResizeLimits(500, juce::roundToInt(500.0 * h0 / w0), 4000, juce::roundToInt(4000.0 * h0 / w0));
  getConstrainer()->setFixedAspectRatio((double)w0 / h0);
  setSize(w > 0 && h > 0 ? w : w0, w > 0 && h > 0 ? h : h0);

  buildMachineView();
  tick();
  ticker_.startTimerHz(10);
}

MirageEditor::~MirageEditor() {
  stopTimer();
  ticker_.stopTimer();
  delete importDialog_.getComponent();
  chooser_.reset();
  actions_.reset();
  menuBar_.setModel(nullptr);
  removeKeyListener(commands_.getKeyMappings());
  releaseHeldKeys(true);
}

// ---- Layout ----------------------------------------------------------------

void MirageEditor::resized() {
  auto r = getLocalBounds();
  menuBar_.setBounds(r.removeFromTop(kMenuHeight));
  auto strip = r.removeFromBottom(kStripHeight);
  if (panel_) panel_->setBounds(r);
  auto prompt = r.withSizeKeepingCentre(std::min(r.getWidth() - 40, 720), 110);
  message_.setBounds(prompt.removeFromTop(60));
  romButton_.setBounds(prompt.removeFromBottom(32).withSizeKeepingCentre(240, 32));
  // The Input controls sit at the right end of the strip (as the app's), clear
  // of the resize corner: label, slider with its value box, meter.
  auto controls = strip.removeFromRight(kInputControlsWidth).withTrimmedRight(18);
  inputMeter_.setBounds(controls.removeFromRight(70));
  controls.removeFromRight(4);
  inputSlider_.setBounds(controls.removeFromRight(150));
  inputLabel_.setBounds(controls);
  proc_.setEditorSize(getWidth(), getHeight());
}

void MirageEditor::paint(juce::Graphics& g) {
  g.fillAll(juce::Colour(0xff202024));
  const auto strip = getLocalBounds().removeFromBottom(kStripHeight);
  g.setColour(juce::Colour(0xff2a2a30));
  g.fillRect(strip);
  g.setColour(juce::Colour(0xffc8c8c8));
  const juce::Font font(juce::FontOptions(14.0f));
  g.setFont(font);
  // The disk title on the left; the notice and the warnings on the right, cut
  // with "..." when they do not fit.
  const auto inner = strip.withTrimmedRight(kInputControlsWidth).reduced(8, 0);
  const int leftW = std::min(inner.getWidth(), juce::GlyphArrangement::getStringWidthInt(font, text_.left));
  g.drawText(text_.left, inner, juce::Justification::centredLeft);
  g.drawText(text_.right, inner.withTrimmedLeft(leftW + 24), juce::Justification::centredRight);
}

// ---- The 10 Hz tick --------------------------------------------------------

void MirageEditor::tick() {
  if (proc_.host() != host_) buildMachineView();
  if (const auto problem = proc_.restoreProblem(); problem != problem_) {
    problem_ = problem;
    updateView();
  }
  if (actions_)
    if (auto outcome = proc_.takeSaveOutcome()) actions_->showSaveOutcome(*outcome);
  if (auto notice = proc_.takeImportNotice(); notice.isNotEmpty()) {
    notice_ = notice;
    noticeUntil_ = juce::Time::getMillisecondCounter() + kNoticeMs;
  }
  if (host_) {
    const auto s = host_->status();
    if (s.blocks != blocks_) {
      blocks_ = s.blocks;
      blocksSeenMs_ = juce::Time::getMillisecondCounter();
    }
    auto* jack = proc_.getBus(true, 0);
    inputMeter_.update(s.inputPeak, jack != nullptr && jack->isEnabled());
  } else {
    inputMeter_.update(0.0f, false);
  }
  refresh();
  auto t = buildText();
  if (t == text_) return;
  text_ = std::move(t);
  repaint(getLocalBounds().removeFromBottom(kStripHeight));
}

MirageEditor::StripText MirageEditor::buildText() const {
  StripText t;
  t.left = diskTitle_;
  juce::StringArray problems;
  if (notice_.isNotEmpty() && juce::Time::getMillisecondCounter() < noticeUntil_) problems.add(notice_);
  if (host_) {
    const auto s = host_->status();
    if (s.illegalOps != 0) problems.add("illegal ops: " + juce::String((juce::int64)s.illegalOps));
    // Bytes, plus one per MIDI-out message the plugin could not pass on (midiDropped()).
    if (const auto dropped = s.midiDropped + proc_.midiDropped(); dropped != 0)
      problems.add("MIDI dropped: " + juce::String((juce::int64)dropped));
    if (!host_->calibrationMuteArmed()) problems.add("calibration not muted (unknown ROM)");
  }
  problems.add(proc_.restoreProblem());
  problems.add(proc_.saveProblem());
  problems.removeEmptyStrings();
  t.right = problems.joinIntoString("   ");
  return t;
}

void MirageEditor::buildMachineView() {
  auto* host = proc_.host();
  mirage::DiskSession* session = nullptr;
  if (host != nullptr && !proc_.trySession([&](mirage::DiskSession& s) { session = &s; }))
    return updateView();  // a state call holds the lock: built on a later tick
  delete importDialog_.getComponent();  // it would write to the old host
  releaseHeldKeys(true);                // on the old panel
  actions_.reset();
  volume_.reset();
  panel_.reset();
  host_ = host;
  if (host_ != nullptr) {
    panel_ = std::make_unique<PanelComponent>(*host_);
    addChildComponent(*panel_);
    volume_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc_.params, "volume",
                                                                                     panel_->volumeSlider());
    actions_ = std::make_unique<DiskActions>(
        *this, *session, [this] { return diskFolder(); },
        [](const juce::File& dir) { saveFolder("lastDiskDir", dir); }, [this] { refresh(); },
        [this](std::function<void()> work) {
          // Queued work may run after this editor has gone, or after the
          // processor replaced the host (the view not yet rebuilt): dropped then.
          proc_.withSession([safe = juce::Component::SafePointer<MirageEditor>(this),
                             work = std::move(work)](mirage::DiskSession&) {
            if (safe != nullptr && safe->host_ == safe->proc_.host()) work();
          });
        });
  }
  blocks_ = 0;
  blocksSeenMs_ = 0;
  updateView();
  commands_.commandStatusChanged();
}

void MirageEditor::updateView() {
  // The prompt follows the processor: a machine whose view is not built yet
  // (the state lock was busy) shows neither the panel nor the ROM prompt.
  const bool machine = proc_.host() != nullptr;
  const bool showPanel = panel_ != nullptr && problem_.isEmpty();
  const bool showPrompt = !machine || problem_.isNotEmpty();
  if (panel_) panel_->setVisible(showPanel);
  message_.setVisible(showPrompt);
  romButton_.setVisible(showPrompt);
  if (!showPanel) releaseHeldKeys(true);
  if (showPrompt) {
    message_.setText(problem_.isNotEmpty() ? problem_
                                           : juce::String("No Mirage boot ROM was found. The plugin is silent "
                                                          "until you choose one (a 4096-byte file)."),
                     juce::dontSendNotification);
    romButton_.setButtonText(machine ? "Choose ROM" : "Choose the Mirage boot ROM");
  }
  resized();
  repaint();
}

void MirageEditor::refresh() {
  bool hasDisk = false, saving = false;
  juce::String title = "No disk", name;
  if (!proc_.trySession([&](mirage::DiskSession& s) {
        hasDisk = s.hasDisk();
        saving = s.saving();
        title = fromUtf8(s.title());
        // The panel's disk label: the file name; "Untitled" for a new blank disk; empty without a disk.
        name = s.path().empty() ? juce::String(hasDisk ? "Untitled" : "") : fileName(s.path());
      }) &&
      host_ != nullptr)
    return;  // a state call holds the lock: next tick
  if (panel_) panel_->setDiskName(name);
  diskTitle_ = title;
  const bool canImport = canImportWav();
  if (hasDisk != hasDisk_ || saving != saving_ || canImport != canImport_) {
    hasDisk_ = hasDisk;
    saving_ = saving;
    canImport_ = canImport;
    commands_.commandStatusChanged();
  }
}

// ---- Menus -----------------------------------------------------------------

juce::StringArray MirageEditor::getMenuBarNames() { return {"File", "Machine"}; }

juce::PopupMenu MirageEditor::getMenuForIndex(int index, const juce::String&) {
  juce::PopupMenu m;
  if (index == 0) {
    m.addCommandItem(&commands_, cmdOpenDisk);
    m.addCommandItem(&commands_, cmdNewBlankDisk);
    m.addCommandItem(&commands_, cmdSaveDisk);
    m.addCommandItem(&commands_, cmdSaveDiskAs);
    m.addCommandItem(&commands_, cmdEjectDisk);
    m.addSeparator();
    m.addCommandItem(&commands_, cmdImportWav);
  } else if (index == 1) {
    m.addCommandItem(&commands_, cmdReset);
    m.addCommandItem(&commands_, cmdPanic);
  }
  return m;
}

void MirageEditor::getAllCommands(juce::Array<juce::CommandID>& c) {
  c.addArray({cmdOpenDisk, cmdNewBlankDisk, cmdSaveDisk, cmdSaveDiskAs, cmdEjectDisk, cmdImportWav, cmdReset,
              cmdPanic});
}

void MirageEditor::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) {
  using juce::ModifierKeys;
  const char* file = "File";
  // As the app: no disk operation while a save is in flight; none but Open and
  // New Blank Disk without a disk. Nothing without a machine.
  const bool machine = host_ != nullptr;
  const bool idle = machine && !saving_;
  const bool disk = idle && hasDisk_;
  switch (id) {
    case cmdOpenDisk:
      info.setInfo("Open Disk Image...", "Insert a disk image", file, 0);
      info.addDefaultKeypress('o', ModifierKeys::ctrlModifier);
      info.setActive(idle);
      break;
    case cmdNewBlankDisk:
      info.setInfo("New Blank Disk", "Insert an unformatted disk, to format with the OS", file, 0);
      info.addDefaultKeypress('n', ModifierKeys::ctrlModifier);
      info.setActive(idle);
      break;
    case cmdSaveDisk:
      info.setInfo("Save Disk Image", "Save the inserted disk", file, 0);
      info.addDefaultKeypress('s', ModifierKeys::ctrlModifier);
      info.setActive(disk);
      break;
    case cmdSaveDiskAs:
      info.setInfo("Save Disk Image As...", "Save the inserted disk to a new file", file, 0);
      info.addDefaultKeypress('s', ModifierKeys::ctrlModifier | ModifierKeys::shiftModifier);
      info.setActive(disk);
      break;
    case cmdEjectDisk:
      info.setInfo("Eject Disk", "Eject the disk", file, 0);
      info.addDefaultKeypress('e', ModifierKeys::ctrlModifier);
      info.setActive(disk);
      break;
    case cmdImportWav:
      info.setInfo("Import WAV...", "Write a WAV file into wave RAM", file, 0);
      info.addDefaultKeypress('i', ModifierKeys::ctrlModifier);
      info.setActive(canImport_);
      break;
    case cmdReset:
      info.setInfo("Reset", "Power-cycle the Mirage", "Machine", 0);
      info.addDefaultKeypress('r', ModifierKeys::ctrlModifier);
      info.setActive(machine);
      break;
    case cmdPanic:
      info.setInfo("Panic", "All Notes Off on every channel", "Machine", 0);
      info.setActive(machine);
      break;
    default:
      break;
  }
}

bool MirageEditor::perform(const InvocationInfo& info) {
  if (host_ == nullptr || !actions_) return false;
  switch (info.commandID) {
    case cmdOpenDisk:
      afterDiscard([this] { actions_->open(); });
      return true;
    case cmdNewBlankDisk:
      afterDiscard([this] { actions_->newBlank(); });
      return true;
    case cmdSaveDisk:
      actions_->save();
      return true;
    case cmdSaveDiskAs:
      actions_->saveAs();
      return true;
    case cmdEjectDisk:
      actions_->eject();
      return true;
    case cmdImportWav:
      importWav();
      return true;
    case cmdReset:
      if (proc_.restoreProblem().isNotEmpty())
        afterDiscard({});  // the discard is the power-cycle
      else
        host_->postReset();
      return true;
    case cmdPanic:
      host_->postPanic();
      return true;
    default:
      return false;
  }
}

void MirageEditor::afterDiscard(std::function<void()> then) {
  if (proc_.restoreProblem().isEmpty()) {
    if (then) then();
    return;
  }
  juce::Component::SafePointer<MirageEditor> safe(this);
  juce::AlertWindow::showAsync(
      juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon, "Mirage not restored",
                                                   "Discard the Mirage saved in this project?", "Discard",
                                                   "Cancel", this),
      [safe, then = std::move(then)](int choice) {  // 1 Discard, 0 Cancel
        if (choice != 1 || safe == nullptr) return;
        safe->proc_.discardSavedState([safe, then] {
          if (safe != nullptr && then) then();
        });
      });
}

// ---- The boot ROM ----------------------------------------------------------

void MirageEditor::chooseRom() {
  chooser_ = std::make_unique<juce::FileChooser>("Choose the Mirage boot ROM", juce::File(), "*.bin;*");
  juce::Component::SafePointer<MirageEditor> safe(this);
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                        [safe](const juce::FileChooser& fc) {
                          const auto file = fc.getResult();
                          if (safe == nullptr || file == juce::File()) return;
                          if (!setBootRom(file))
                            return safe->showError("Not a Mirage boot ROM",
                                                   file.getFileName() + " is not a 4096-byte boot ROM.");
                          // Never waits for the state lock: when it is busy, the
                          // processor's timer builds the machine and tick() sees it.
                          safe->proc_.requestStartMachine();
                          safe->tick();
                        });
}

// ---- Disks and dialogs -----------------------------------------------------

juce::File MirageEditor::diskFolder() const {
  const auto last = savedFolder("lastDiskDir");
  return last != juce::File() ? last : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}

void MirageEditor::showError(const juce::String& title, const juce::String& message) {
  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, title, message, {}, this);
}

bool MirageEditor::dialogOpen() const {
  return (actions_ && actions_->dialogOpen()) ||
         juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0;
}

// ---- WAV import ------------------------------------------------------------

bool MirageEditor::canImportWav() const {
  // Without blocks the write would wait (the DAW's audio is off, or the
  // project's Mirage was not restored).
  return host_ != nullptr && blocksSeenMs_ != 0 && juce::Time::getMillisecondCounter() - blocksSeenMs_ < 500 &&
         !host_->status().driveActive;
}

void MirageEditor::importWav() {
  if (!canImportWav() || importDialog_ != nullptr) return;
  const auto last = savedFolder("lastWavDir");
  chooser_ = std::make_unique<juce::FileChooser>("Import WAV", last != juce::File() ? last : diskFolder(), "*.wav");
  juce::Component::SafePointer<MirageEditor> safe(this);
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                        [safe](const juce::FileChooser& fc) {
                          const auto file = fc.getResult();
                          if (safe == nullptr || file == juce::File()) return;
                          saveFolder("lastWavDir", file.getParentDirectory());
                          safe->showImportDialog(file);
                        });
}

void MirageEditor::showImportDialog(const juce::File& file) {
  std::shared_ptr<const mirage::WavAudio> audio;
  try {
    audio = std::make_shared<const mirage::WavAudio>(mirage::readWavFile(toPath(file)));
  } catch (const mirage::WavError& e) {
    return showError("Cannot import " + file.getFileName(), fromUtf8(e.what()));
  } catch (const std::exception&) {  // std::bad_alloc for a file too big to hold
    return showError("Cannot import " + file.getFileName(), "The WAV file could not be read.");
  }
  auto* dialog = new ImportDialog(audio, file.getFileName(), [this] { return canImportWav(); });
  juce::Component::SafePointer<MirageEditor> safe(this);
  dialog->onImport = [safe, audio](const mirage::ImportSettings& settings) {
    if (safe == nullptr) return;
    safe->postImport(*audio, settings);
    if (auto* w = safe->importDialog_.getComponent()) w->exitModalState(0);
  };
  dialog->onCancel = [safe] {
    if (safe != nullptr)
      if (auto* w = safe->importDialog_.getComponent()) w->exitModalState(0);
  };
  juce::DialogWindow::LaunchOptions o;
  o.dialogTitle = "Import WAV";
  o.content.setOwned(dialog);
  o.componentToCentreAround = this;
  o.dialogBackgroundColour = juce::Colour(0xff202024);
  o.escapeKeyTriggersCloseButton = true;
  o.useNativeTitleBar = true;
  o.resizable = false;
  importDialog_ = o.launchAsync();
}

void MirageEditor::postImport(const mirage::WavAudio& audio, const mirage::ImportSettings& settings) {
  if (!canImportWav())
    return showError("Not imported", "The drive light is on or the DAW runs no audio. Try again.");
  auto write = std::make_unique<mirage::WaveWrite>();
  write->address = mirage::planImport(audio, settings).address;
  write->bytes = mirage::convertForWaveRam(audio, settings);
  if (write->bytes.empty()) return;  // the dialog keeps Import off while the plan has a problem
  if (!host_->postWaveWrite(std::move(write))) showError("Not imported", "The emulator is busy. Try again.");
}

// ---- Keyboard --------------------------------------------------------------

bool MirageEditor::keyPressed(const juce::KeyPress& key) {
  using juce::KeyPress;
  const auto mods = key.getModifiers();
  if (mods.isCtrlDown() || mods.isAltDown() || mods.isCommandDown()) return false;  // menus
  if (dialogOpen() || panel_ == nullptr || !panel_->isVisible()) return false;
  const int code = key.getKeyCode();
  std::optional<mirage::PanelKey> panelKey;
  if (code == KeyPress::upKey)
    panelKey = mirage::panelKeyForShortcut(mirage::ShortcutKey::Up);
  else if (code == KeyPress::downKey)
    panelKey = mirage::panelKeyForShortcut(mirage::ShortcutKey::Down);
  else if (code == KeyPress::returnKey)
    panelKey = mirage::panelKeyForShortcut(mirage::ShortcutKey::Return);
  else if (code == KeyPress::escapeKey)
    panelKey = mirage::panelKeyForShortcut(mirage::ShortcutKey::Escape);
  else
    panelKey = mirage::panelKeyForChar(char32_t(key.getTextCharacter()));
  if (!panelKey) return false;
  for (const HeldKey& h : held_)
    if (h.keyCode == code) return true;  // auto-repeat
  held_.push_back({code, *panelKey});
  panel_->press(*panelKey, true);
  startTimerHz(30);
  return true;
}

bool MirageEditor::keyStateChanged(bool) {
  releaseHeldKeys(false);
  return false;
}

void MirageEditor::releaseHeldKeys(bool all) {
  for (size_t i = held_.size(); i-- > 0;) {
    if (!all && juce::KeyPress::isKeyCurrentlyDown(held_[i].keyCode)) continue;
    if (panel_) panel_->press(held_[i].key, false);
    held_.erase(held_.begin() + std::ptrdiff_t(i));
  }
  if (held_.empty()) stopTimer();
}

void MirageEditor::timerCallback() {
  // A dialog, or the DAW taking the focus, takes the key-ups: let go then.
  releaseHeldKeys(dialogOpen() || !hasKeyboardFocus(true));
}
