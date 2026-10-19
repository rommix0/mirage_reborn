#include "MainWindow.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace {

// DiskSession speaks std::filesystem::path and UTF-8; JUCE speaks File and String.
std::filesystem::path toPath(const juce::File& file) {
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t*>(file.getFullPathName().toRawUTF8())));
}

juce::String fromUtf8(const std::string& s) { return juce::String::fromUTF8(s.data(), (int)s.size()); }

juce::String text(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return juce::String::fromUTF8(reinterpret_cast<const char*>(u.data()), (int)u.size());
}

juce::String fileName(const std::filesystem::path& p) { return text(p.filename()); }

}  // namespace

// ---- MirageContent ---------------------------------------------------------

MirageContent::MirageContent(mirage::RealtimeHost& host, AudioEngine& engine)
    : host_(host), engine_(engine), panel_(std::make_unique<PanelComponent>(host)) {
  auto& volume = panel_->volumeSlider();
  volume.setValue(engine_.volume(), juce::dontSendNotification);
  volume.onValueChange = [this] { engine_.setVolume(float(panel_->volumeSlider().getValue())); };
  addAndMakeVisible(*panel_);
  const auto& geo = mirage::panelGeometry();
  setSize(juce::roundToInt(geo.width), juce::roundToInt(geo.height) + kStripHeight);
  text_ = buildText();
  startTimerHz(10);
}

void MirageContent::setDiskName(const juce::String& name) {
  diskName_ = name;
  panel_->setDiskName(name);
}

void MirageContent::resized() {
  auto r = getLocalBounds();
  r.removeFromBottom(kStripHeight);
  panel_->setBounds(r);
}

void MirageContent::paint(juce::Graphics& g) {
  auto strip = getLocalBounds().removeFromBottom(kStripHeight);
  g.setColour(juce::Colour(0xff2a2a30));
  g.fillRect(strip);
  g.setColour(juce::Colour(0xffc8c8c8));
  g.setFont(juce::FontOptions(14.0f));
  auto inner = strip.reduced(8, 0);
  g.drawText(text_.left, inner, juce::Justification::centredLeft);
  g.drawText(text_.middle, inner, juce::Justification::centred);
  g.drawText(text_.right, inner, juce::Justification::centredRight);
}

void MirageContent::timerCallback() {
  auto t = buildText();
  if (t == text_) return;
  text_ = std::move(t);
  repaint(getLocalBounds().removeFromBottom(kStripHeight));
}

MirageContent::StripText MirageContent::buildText() const {
  StripText t;
  t.left = "No disk";
  t.middle = "No audio device";
  const auto s = host_.status();
  if (s.diskPresent) {
    t.left = diskName_.isNotEmpty() ? diskName_ : juce::String("Disk");
    if (s.diskDirty) t.left << " (modified)";
  }
  juce::StringArray problems;
  if (s.illegalOps != 0) problems.add("illegal ops: " + juce::String((juce::int64)s.illegalOps));
  if (s.midiDropped != 0) problems.add("MIDI dropped: " + juce::String((juce::int64)s.midiDropped));
  t.right = problems.joinIntoString("   ");
  if (auto* dev = engine_.devices().getCurrentAudioDevice())
    t.middle = dev->getName() + " @ " + juce::String(juce::roundToInt(dev->getCurrentSampleRate())) +
               " Hz   CPU " + juce::String(juce::roundToInt(engine_.cpuLoad() * 100.0)) + "%";
  return t;
}

// ---- MainWindow ------------------------------------------------------------

MainWindow::MainWindow(mirage::RealtimeHost& host, AudioEngine& engine, Settings& settings)
    : juce::DocumentWindow("Mirage", juce::Colour(0xff202024), juce::DocumentWindow::allButtons),
      host_(host),
      engine_(engine),
      settings_(settings),
      session_(host) {
  setUsingNativeTitleBar(true);
  content_ = new MirageContent(host, engine);
  setContentOwned(content_, true);
  setResizable(true, true);
  setResizeLimits(500, 200, 8000, 4000);  // the panel letterboxes to keep its aspect ratio

  commands_.registerAllCommandsForTarget(this);
  commands_.setFirstCommandTarget(this);
  addKeyListener(commands_.getKeyMappings());
  addKeyListener(this);
  setMenuBar(this);

  refreshDiskState();
  diskTimer_.startTimerHz(10);

  centreWithSize(getWidth(), getHeight());
  setVisible(true);
}

MainWindow::~MainWindow() {
  stopTimer();
  diskTimer_.stopTimer();
  // The app can quit with the dialog open (Windows ending the session), and
  // the dialog must not outlive the engine's AudioDeviceManager.
  delete audioSettings_.getComponent();
  chooser_.reset();
  removeKeyListener(this);
  setMenuBar(nullptr);
  clearContentComponent();
}

void MainWindow::closeButtonPressed() { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

void MainWindow::requestQuit() {
  whenDiskSafe("quitting", [] { juce::JUCEApplication::quit(); });
}

void MainWindow::activeWindowStatusChanged() {
  juce::DocumentWindow::activeWindowStatusChanged();
  // Key-up events go to the active window, so let go of everything held.
  if (!isActiveWindow()) releaseHeldKeys(true);
}

bool MainWindow::keyPressed(const juce::KeyPress& key, juce::Component*) {
  using juce::KeyPress;
  const auto mods = key.getModifiers();
  if (mods.isCtrlDown() || mods.isAltDown() || mods.isCommandDown()) return false;  // menus
  // Modal components include the alert and dialog windows and JUCE's
  // stand-in for a native file chooser.
  if (juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0) return false;
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
  content_->panel().press(*panelKey, true);
  startTimerHz(30);
  return true;
}

bool MainWindow::keyStateChanged(bool, juce::Component*) {
  releaseHeldKeys(false);
  return false;
}

void MainWindow::releaseHeldKeys(bool all) {
  for (size_t i = held_.size(); i-- > 0;) {
    if (!all && juce::KeyPress::isKeyCurrentlyDown(held_[i].keyCode)) continue;
    content_->panel().press(held_[i].key, false);
    held_.erase(held_.begin() + std::ptrdiff_t(i));
  }
  if (held_.empty()) stopTimer();
}

void MainWindow::timerCallback() {
  if (juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0)
    releaseHeldKeys(true);
}

juce::StringArray MainWindow::getMenuBarNames() { return {"File", "Machine", "MIDI", "Audio"}; }

juce::PopupMenu MainWindow::getMenuForIndex(int index, const juce::String&) {
  juce::PopupMenu m;
  switch (index) {
    case 0:
      m.addCommandItem(&commands_, cmdOpenDisk);
      m.addCommandItem(&commands_, cmdSaveDisk);
      m.addCommandItem(&commands_, cmdSaveDiskAs);
      m.addCommandItem(&commands_, cmdEjectDisk);
      m.addSeparator();
      m.addCommandItem(&commands_, cmdQuit);
      break;
    case 1:
      m.addCommandItem(&commands_, cmdReset);
      break;
    case 2: {
      midiInputs_ = juce::MidiInput::getAvailableDevices();
      midiOutputs_ = juce::MidiOutput::getAvailableDevices();
      // Radio items: at most one input is enabled at a time.
      m.addSectionHeader("MIDI input");
      const auto inId = engine_.midiInputId();
      m.addItem(midiInNone, "None", true, inId.isEmpty());
      for (int i = 0; i < midiInputs_.size(); ++i)
        m.addItem(midiInBase + i, midiInputs_[i].name, true, midiInputs_[i].identifier == inId);
      m.addSeparator();
      m.addSectionHeader("MIDI output");
      const auto outId = engine_.midiOutputId();
      m.addItem(midiOutNone, "None", true, outId.isEmpty());
      for (int i = 0; i < midiOutputs_.size(); ++i)
        m.addItem(midiOutBase + i, midiOutputs_[i].name, true, midiOutputs_[i].identifier == outId);
      m.addSeparator();
      m.addCommandItem(&commands_, cmdPanic);
      break;
    }
    case 3:
      m.addCommandItem(&commands_, cmdAudioSettings);
      break;
    default:
      break;
  }
  return m;
}

// The saved MIDI devices change only here, when the user picks one: a saved
// device that is absent at launch stays saved for the next launch.
void MainWindow::menuItemSelected(int id, int) {
  if (id == midiInNone || (id >= midiInBase && id < midiInBase + midiInputs_.size())) {
    const auto picked = id == midiInNone ? juce::String() : midiInputs_[id - midiInBase].identifier;
    engine_.setMidiInput(picked);
    settings_.setMidiIn(picked);
  } else if (id == midiOutNone || (id >= midiOutBase && id < midiOutBase + midiOutputs_.size())) {
    const auto picked =
        id == midiOutNone ? juce::String() : midiOutputs_[id - midiOutBase].identifier;
    engine_.setMidiOutput(picked);
    settings_.setMidiOut(picked);
  }
}

void MainWindow::getAllCommands(juce::Array<juce::CommandID>& c) {
  c.addArray({cmdOpenDisk, cmdSaveDisk, cmdSaveDiskAs, cmdEjectDisk, cmdQuit, cmdReset, cmdPanic,
              cmdAudioSettings});
}

void MainWindow::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) {
  using juce::ModifierKeys;
  const char* file = "File";
  // No disk operation while a save is in flight; none but Open without a disk.
  const bool idle = !session_.saving();
  const bool disk = idle && !session_.path().empty();
  switch (id) {
    case cmdOpenDisk:
      info.setInfo("Open Disk Image...", "Insert a disk image", file, 0);
      info.addDefaultKeypress('o', ModifierKeys::ctrlModifier);
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
    case cmdQuit:
      info.setInfo("Exit", "Quit Mirage", file, 0);
      break;
    case cmdReset:
      info.setInfo("Reset", "Power-cycle the Mirage", "Machine", 0);
      info.addDefaultKeypress('r', ModifierKeys::ctrlModifier);
      break;
    case cmdPanic:
      info.setInfo("Panic", "All Notes Off on every channel", "MIDI", 0);
      break;
    case cmdAudioSettings:
      info.setInfo("Audio Settings...", "Choose the audio output", "Audio", 0);
      break;
    default:
      break;
  }
}

bool MainWindow::perform(const InvocationInfo& info) {
  switch (info.commandID) {
    case cmdOpenDisk:
      openDisk();
      return true;
    case cmdSaveDisk:
      saveDisk();
      return true;
    case cmdSaveDiskAs:
      saveDiskAs();
      return true;
    case cmdEjectDisk:
      ejectDisk();
      return true;
    case cmdQuit:
      juce::JUCEApplication::getInstance()->systemRequestedQuit();
      return true;
    case cmdReset:
      host_.postReset();
      return true;
    case cmdPanic:
      host_.postPanic();
      return true;
    case cmdAudioSettings:
      showAudioSettings();
      return true;
    default:
      return false;
  }
}

void MainWindow::showAudioSettings() {
  if (audioSettings_ != nullptr) return audioSettings_->toFront(true);
  // Outputs only (stereo pairs), no MIDI section: MIDI has its own menu.
  auto* selector = new juce::AudioDeviceSelectorComponent(engine_.devices(), 0, 0, 0, 2, false,
                                                          false, true, false);
  selector->setSize(520, 420);
  juce::DialogWindow::LaunchOptions o;
  o.dialogTitle = "Audio Settings";
  o.content.setOwned(selector);
  o.componentToCentreAround = this;
  o.dialogBackgroundColour = getBackgroundColour();
  o.escapeKeyTriggersCloseButton = true;
  o.useNativeTitleBar = true;
  o.resizable = false;
  audioSettings_ = o.launchAsync();
}

// ---- Disk files ------------------------------------------------------------

void MainWindow::openDisk() {
  if (session_.saving() || promptOpen_) return;
  // Any file of exactly 450,560 bytes is a disk image, whatever its extension.
  chooser_ = std::make_unique<juce::FileChooser>("Open Disk Image", diskFolder(), "*.img;*");
  juce::Component::SafePointer<MainWindow> safe(this);
  chooser_->launchAsync(
      juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
      [safe](const juce::FileChooser& fc) {
        const auto file = fc.getResult();
        if (safe == nullptr || file == juce::File()) return;
        safe->settings_.setLastDiskDir(file.getParentDirectory().getFullPathName());
        safe->whenDiskSafe("opening another disk", [w = safe.getComponent(), file] {
          w->insertDisk(file);
        });
      });
}

void MainWindow::insertDisk(const juce::File& file) {
  const auto error = session_.open(toPath(file));
  if (!error.empty()) showError("Cannot open disk image", fromUtf8(error));
  refreshDiskState();
}

void MainWindow::saveDisk(std::function<void()> then) {
  if (session_.path().empty()) return saveDiskAs(std::move(then));
  startSave({}, std::move(then));
}

void MainWindow::saveDiskAs(std::function<void()> then) {
  if (session_.saving() || session_.path().empty()) return;
  // The .img extension is the dialog's default (taken from the suggested name).
  const auto suggested =
      diskFolder().getChildFile(text(session_.path().stem())).withFileExtension(".img");
  chooser_ = std::make_unique<juce::FileChooser>("Save Disk Image As", suggested, "*.img");
  juce::Component::SafePointer<MainWindow> safe(this);
  chooser_->launchAsync(juce::FileBrowserComponent::saveMode |
                            juce::FileBrowserComponent::canSelectFiles |
                            juce::FileBrowserComponent::warnAboutOverwriting,
                        [safe, then = std::move(then)](const juce::FileChooser& fc) {
                          const auto file = fc.getResult();
                          if (safe == nullptr || file == juce::File()) return;
                          safe->settings_.setLastDiskDir(
                              file.getParentDirectory().getFullPathName());
                          safe->startSave(file, then);
                        });
}

void MainWindow::startSave(const juce::File& file, std::function<void()> then) {
  const auto target = file == juce::File() ? std::filesystem::path() : toPath(file);
  if (!session_.startSave(target)) {
    showError("Cannot save disk image", "The disk could not be saved right now. Try again.");
    return;
  }
  afterSave_ = std::move(then);
  refreshDiskState();
}

void MainWindow::ejectDisk() {
  whenDiskSafe("ejecting it", [this] {
    session_.eject();
    refreshDiskState();
  });
}

void MainWindow::whenDiskSafe(const juce::String& action, std::function<void()> then) {
  if (promptOpen_) return;
  if (session_.saving()) {
    // Ask again once the save in flight succeeds (the disk may be clean by then).
    if (!afterSave_) afterSave_ = [this, action, then] { whenDiskSafe(action, then); };
    return;
  }
  if (!session_.needsSaveBeforeReplacing()) return then();
  promptOpen_ = true;
  juce::Component::SafePointer<MainWindow> safe(this);
  juce::AlertWindow::showAsync(
      juce::MessageBoxOptions::makeOptionsYesNoCancel(
          juce::MessageBoxIconType::QuestionIcon, "Unsaved changes",
          "Save changes to " + fileName(session_.path()) + " before " + action + "?", "Save",
          "Don't Save", "Cancel", this),
      [safe, then = std::move(then)](int choice) {  // 1 Save, 2 Don't Save, 0 Cancel
        if (safe == nullptr) return;
        safe->promptOpen_ = false;
        if (choice == 1)
          safe->saveDisk(then);
        else if (choice == 2)
          then();
      });
}

void MainWindow::pollDisk() {
  if (auto outcome = session_.poll()) {
    auto then = std::exchange(afterSave_, nullptr);
    if (outcome->ok) {
      if (then) then();
    } else {
      showError("Cannot save disk image", fromUtf8(outcome->message));
    }
  }
  refreshDiskState();
}

void MainWindow::refreshDiskState() {
  const auto title = juce::String(juce::CharPointer_UTF8("Mirage \xe2\x80\x94 ")) +
                     fromUtf8(session_.title());
  if (getName() != title) setName(title);
  const auto name = fileName(session_.path());  // empty without a disk
  content_->setDiskName(name);
  const bool hasDisk = !session_.path().empty(), saving = session_.saving();
  if (hasDisk != menusHaveDisk_ || saving != menusSaving_) {
    menusHaveDisk_ = hasDisk;
    menusSaving_ = saving;
    commands_.commandStatusChanged();
  }
}

juce::File MainWindow::diskFolder() const {
  const juce::File last(settings_.lastDiskDir());
  if (settings_.lastDiskDir().isNotEmpty() && last.isDirectory()) return last;
  return juce::File(MIRAGE_ASSET_ROOT);
}

void MainWindow::showError(const juce::String& title, const juce::String& message) {
  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, title, message, {},
                                         this);
}
