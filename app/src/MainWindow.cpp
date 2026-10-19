#include "MainWindow.h"

#include <optional>

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
      settings_(settings) {
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

  centreWithSize(getWidth(), getHeight());
  setVisible(true);
}

MainWindow::~MainWindow() {
  stopTimer();
  removeKeyListener(this);
  setMenuBar(nullptr);
  clearContentComponent();
}

void MainWindow::closeButtonPressed() { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

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

void MainWindow::menuItemSelected(int id, int) {
  if (id == midiInNone) {
    engine_.setMidiInput({});
  } else if (id == midiOutNone) {
    engine_.setMidiOutput({});
  } else if (id >= midiInBase && id < midiInBase + midiInputs_.size()) {
    engine_.setMidiInput(midiInputs_[id - midiInBase].identifier);
  } else if (id >= midiOutBase && id < midiOutBase + midiOutputs_.size()) {
    engine_.setMidiOutput(midiOutputs_[id - midiOutBase].identifier);
  }
}

void MainWindow::getAllCommands(juce::Array<juce::CommandID>& c) {
  c.addArray({cmdOpenDisk, cmdSaveDisk, cmdSaveDiskAs, cmdEjectDisk, cmdQuit, cmdReset, cmdPanic,
              cmdAudioSettings});
}

void MainWindow::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) {
  using juce::ModifierKeys;
  const char* file = "File";
  switch (id) {
    case cmdOpenDisk:
      info.setInfo("Open Disk Image...", "Insert a disk image", file, 0);
      info.addDefaultKeypress('o', ModifierKeys::ctrlModifier);
      info.setActive(false);  // wired in Task 11
      break;
    case cmdSaveDisk:
      info.setInfo("Save Disk Image", "Save the inserted disk", file, 0);
      info.addDefaultKeypress('s', ModifierKeys::ctrlModifier);
      info.setActive(false);  // wired in Task 11
      break;
    case cmdSaveDiskAs:
      info.setInfo("Save Disk Image As...", "Save the inserted disk to a new file", file, 0);
      info.addDefaultKeypress('s', ModifierKeys::ctrlModifier | ModifierKeys::shiftModifier);
      info.setActive(false);  // wired in Task 11
      break;
    case cmdEjectDisk:
      info.setInfo("Eject Disk", "Eject the disk", file, 0);
      info.addDefaultKeypress('e', ModifierKeys::ctrlModifier);
      info.setActive(false);  // wired in Task 11
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
  o.launchAsync();
}
