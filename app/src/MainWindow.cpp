#include "MainWindow.h"

// ---- MirageContent ---------------------------------------------------------

class MirageContent::Placeholder : public juce::Component {
 public:
  void paint(juce::Graphics& g) override {
    g.fillAll(juce::Colour(0xff1c1c20));
    g.setColour(juce::Colour(0xffd8d8d8));
    g.setFont(juce::FontOptions(48.0f, juce::Font::bold));
    g.drawText("Mirage", getLocalBounds(), juce::Justification::centred);
  }
};

MirageContent::MirageContent(mirage::RealtimeHost* host, AudioEngine* engine)
    : host_(host), engine_(engine), panel_(std::make_unique<Placeholder>()) {
  addAndMakeVisible(*panel_);
  setSize(960, 208 + kStripHeight);
  text_ = buildText();
  startTimerHz(10);
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
  if (host_ != nullptr) {
    const auto s = host_->status();
    if (s.diskPresent) {
      t.left = diskName_.isNotEmpty() ? diskName_ : juce::String("Disk");
      if (s.diskDirty) t.left << " (modified)";
    }
    juce::StringArray problems;
    if (s.illegalOps != 0) problems.add("illegal ops: " + juce::String((juce::int64)s.illegalOps));
    if (s.midiDropped != 0)
      problems.add("MIDI dropped: " + juce::String((juce::int64)s.midiDropped));
    t.right = problems.joinIntoString("   ");
  }
  if (engine_ != nullptr) {
    if (auto* dev = engine_->devices().getCurrentAudioDevice())
      t.middle = dev->getName() + " @ " + juce::String(juce::roundToInt(dev->getCurrentSampleRate())) +
                 " Hz   CPU " + juce::String(juce::roundToInt(engine_->cpuLoad() * 100.0)) + "%";
  }
  return t;
}

// ---- MainWindow ------------------------------------------------------------

MainWindow::MainWindow(mirage::RealtimeHost& host, AudioEngine& engine, Settings& settings)
    : juce::DocumentWindow("Mirage", juce::Colour(0xff202024), juce::DocumentWindow::allButtons),
      host_(host),
      engine_(engine),
      settings_(settings) {
  setUsingNativeTitleBar(true);
  content_ = new MirageContent(&host, &engine);
  setContentOwned(content_, true);
  setResizable(true, true);

  commands_.registerAllCommandsForTarget(this);
  commands_.setFirstCommandTarget(this);
  addKeyListener(commands_.getKeyMappings());
  setMenuBar(this);

  centreWithSize(getWidth(), getHeight());
  setVisible(true);
}

MainWindow::~MainWindow() {
  setMenuBar(nullptr);
  clearContentComponent();
}

void MainWindow::closeButtonPressed() { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

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
