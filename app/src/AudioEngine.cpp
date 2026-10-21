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
  auto error = deviceManager_.initialise(0, 2, savedState, true);
  deviceManager_.addAudioCallback(this);
  return error;
}

// Device (re)start; the device manager calls this before the first callback and
// never while a callback runs, so prepare() cannot race process().
void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device) {
  rate_ = device->getCurrentSampleRate();
  maxBlock_ = (size_t)std::max(device->getCurrentBufferSizeSamples(), 64);
  host_.prepare(maxBlock_, rate_);
  lastGain_ = volume_.load(std::memory_order_relaxed);
  ready_.store(true, std::memory_order_release);
}

void AudioEngine::audioDeviceStopped() { ready_.store(false, std::memory_order_release); }

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* out,
                                                   int numOut, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext&) {
  if (numOut <= 0 || numSamples <= 0) return;
  float* ch0 = out[0];
  if (ch0 == nullptr) return;
  if (!ready_.load(std::memory_order_acquire)) {
    for (int c = 0; c < numOut; ++c)
      if (out[c] != nullptr) std::fill_n(out[c], numSamples, 0.0f);
    return;
  }
  const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
  // A device may deliver a block larger than it announced; render it in pieces.
  for (int done = 0; done < numSamples;) {
    const int n = (int)std::min<size_t>((size_t)(numSamples - done), maxBlock_);
    host_.process(ch0 + done, (size_t)n, rate_, now);
    done += n;
  }
  // Volume: a linear ramp over the block, so a moving slider does not zipper.
  const float target = volume_.load(std::memory_order_relaxed);
  const float step = (target - lastGain_) / (float)numSamples;
  float g = lastGain_;
  for (int i = 0; i < numSamples; ++i) {
    g += step;
    ch0[i] *= g;
  }
  lastGain_ = target;
  for (int c = 1; c < numOut; ++c)
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
