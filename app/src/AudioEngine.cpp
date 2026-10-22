#include "AudioEngine.h"

#include <algorithm>

AudioEngine::AudioEngine(mirage::RealtimeHost& host) : juce::Thread("MIDI out"), host_(host), feeder_(host) {
  startThread();
}

AudioEngine::~AudioEngine() {
  setMidiInput({});
  deviceManager_.removeAudioCallback(this);
  deviceManager_.closeAudioDevice();
  stopThread(2000);
}

juce::String AudioEngine::start(const juce::XmlElement* savedState) {
  // No input channel is active unless the saved state has some: with no saved state
  // nothing is plugged into the jack until the user picks an input in Audio Settings
  // (spec 6.5). A saved input that cannot be opened (unplugged, permission refused)
  // falls back to the default output with no input, inside JUCE.
  auto error = deviceManager_.initialise(0, 2, savedState, true);
  keptState_.reset();
  if (savedState != nullptr && !openedAsSaved(*savedState, error))
    keptState_ = std::make_unique<juce::XmlElement>(*savedState);  // the fallback is not the user's choice
  deviceManager_.addAudioCallback(this);
  return error;
}

// Did the manager open what the saved state names (output device, and the input device
// with channels when it had some)?
bool AudioEngine::openedAsSaved(const juce::XmlElement& saved, const juce::String& error) const {
  if (error.isNotEmpty() || deviceManager_.getCurrentAudioDevice() == nullptr) return false;
  const auto all = saved.getStringAttribute("audioDeviceName");
  const auto outName = all.isNotEmpty() ? all : saved.getStringAttribute("audioOutputDeviceName");
  const auto inName = all.isNotEmpty() ? all : saved.getStringAttribute("audioInputDeviceName");
  juce::BigInteger inChans;
  inChans.parseString(saved.getStringAttribute("audioDeviceInChans", "0"), 2);
  const auto cur = deviceManager_.getAudioDeviceSetup();
  if (outName.isNotEmpty() && outName != cur.outputDeviceName) return false;
  if (!inChans.isZero() && (inName != cur.inputDeviceName || cur.inputChannels.isZero()))
    return false;
  return true;
}

std::unique_ptr<juce::XmlElement> AudioEngine::saveAudioState() const {
  if (keptState_ != nullptr) return std::make_unique<juce::XmlElement>(*keptState_);
  return deviceManager_.createStateXml();
}

void AudioEngine::setInputGain(float gain) {
  inputGain_.store(gain, std::memory_order_relaxed);
  host_.setInputGain(gain);
}

bool AudioEngine::hasInput() const {
  auto* dev = deviceManager_.getCurrentAudioDevice();
  return dev != nullptr && !dev->getActiveInputChannels().isZero();
}

// Device (re)start; the device manager calls this before the first callback and
// never while a callback runs, so prepare() cannot race process().
void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device) {
  rate_ = device->getCurrentSampleRate();
  maxBlock_ = (size_t)std::max(device->getCurrentBufferSizeSamples(), 64);
  inMono_.assign(maxBlock_, 0.0f);  // the only allocations; the callback never resizes them
  scratchOut_.assign(maxBlock_, 0.0f);
  host_.prepare(maxBlock_, rate_);
  lastGain_ = volume_.load(std::memory_order_relaxed);
  ready_.store(true, std::memory_order_release);
}

void AudioEngine::audioDeviceStopped() { ready_.store(false, std::memory_order_release); }

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out,
                                                   int numOut, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext&) {
  if (numSamples <= 0) return;
  // The first active output channel (null pointers are inactive ones). With none, the
  // machine still runs, rendering into scratchOut_, so an input-only device keeps time.
  int first = -1;
  for (int c = 0; c < numOut && first < 0; ++c)
    if (out != nullptr && out[c] != nullptr) first = c;
  float* ch0 = first >= 0 ? out[first] : nullptr;
  if (!ready_.load(std::memory_order_acquire)) {
    for (int c = 0; c < numOut; ++c)
      if (out[c] != nullptr) std::fill_n(out[c], numSamples, 0.0f);
    return;
  }
  const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
  // Inactive input channels come as null pointers. No active channel: nothing is plugged in.
  int active = 0;
  if (in != nullptr)
    for (int c = 0; c < numIn; ++c)
      if (in[c] != nullptr) ++active;
  const float norm = active > 0 ? 1.0f / (float)active : 0.0f;
  // A device may deliver a block larger than it announced; render it in pieces.
  for (int done = 0; done < numSamples;) {
    const int n = (int)std::min<size_t>((size_t)(numSamples - done), maxBlock_);
    const float* mono = nullptr;
    if (active > 0) {  // the Mirage samples mono: the average of the active channels
      float* m = inMono_.data();
      std::fill_n(m, n, 0.0f);
      for (int c = 0; c < numIn; ++c)
        if (in[c] != nullptr)
          for (int i = 0; i < n; ++i) m[i] += in[c][done + i];
      for (int i = 0; i < n; ++i) m[i] *= norm;
      mono = m;
    }
    host_.process(mono, ch0 != nullptr ? ch0 + done : scratchOut_.data(), (size_t)n, rate_, now);
    done += n;
  }
  if (ch0 == nullptr) return;  // nothing to hear it on
  // Volume: a linear ramp over the block, so a moving slider does not zipper.
  const float target = volume_.load(std::memory_order_relaxed);
  const float step = (target - lastGain_) / (float)numSamples;
  float g = lastGain_;
  for (int i = 0; i < numSamples; ++i) {
    g += step;
    ch0[i] *= g;
  }
  lastGain_ = target;
  for (int c = first + 1; c < numOut; ++c)
    if (out[c] != nullptr) std::copy_n(ch0, numSamples, out[c]);
}

void AudioEngine::setMidiInput(const juce::String& identifier) {
  if (identifier == midiInId_) return;
  // Remove the old device's callback (which waits for one in progress) before
  // enabling the new one, so two devices' callbacks never overlap.
  if (midiInId_.isNotEmpty()) {
    deviceManager_.removeMidiInputDeviceCallback(midiInId_, this);
    deviceManager_.setMidiInputDeviceEnabled(midiInId_, false);
    midiInId_.clear();
  }
  if (identifier.isNotEmpty()) {
    deviceManager_.setMidiInputDeviceEnabled(identifier, true);
    if (deviceManager_.isMidiInputDeviceEnabled(identifier)) {
      deviceManager_.addMidiInputDeviceCallback(identifier, this);
      midiInId_ = identifier;
    }
  }
}

void AudioEngine::setMidiOutput(const juce::String& identifier) {
  std::unique_ptr<juce::MidiOutput> opened;
  if (identifier.isNotEmpty()) opened = juce::MidiOutput::openDevice(identifier);
  const juce::ScopedLock lock(outLock_);
  midiOut_ = std::move(opened);
  midiOutId_ = midiOut_ != nullptr ? identifier : juce::String();
}

juce::String AudioEngine::midiOutputId() const {
  const juce::ScopedLock lock(outLock_);
  return midiOutId_;
}

void AudioEngine::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& msg) {
  feeder_.push({msg.getRawData(), (size_t)msg.getRawDataSize()}, msg.getTimeStamp());
}

void AudioEngine::run() {
  const auto emit = [this](std::span<const uint8_t> m) {
    const juce::ScopedLock lock(outLock_);
    if (midiOut_ != nullptr) midiOut_->sendMessageNow(juce::MidiMessage(m.data(), (int)m.size()));
  };
  while (!threadShouldExit()) {
    feeder_.pump();  // once per pass: wait(1) below can sleep ~16 ms on Windows, which is plenty
    bool any = false;
    mirage::MidiOutByte b;
    while (host_.popMidiOut(b)) {
      any = true;
      outParser_.feed(b.byte, emit);
    }
    if (!any) wait(1);
  }
}
