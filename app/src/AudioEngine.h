#pragma once
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <memory>
#include <vector>

#include "mirage/midi_byte_parser.h"
#include "mirage/midi_in_feeder.h"
#include "mirage/realtime_host.h"

// Connects a RealtimeHost to the audio device and the MIDI devices.
//  - Audio: the device callback renders host.process() into the first active
//    output channel (into a scratch buffer when there is none, so the machine keeps
//    running) and copies it to the other outputs. The active input channels are averaged
//    into one mono buffer (reserved in audioDeviceAboutToStart) and fed to the
//    emulated input jack; with no active input channel the jack is unplugged
//    (process(nullptr, ...)).
//  - MIDI in: at most ONE input is enabled at a time, so the callbacks of two
//    devices never run concurrently. Everything goes through the MidiInFeeder,
//    the only caller of RealtimeHost::postMidi, which paces long SysEx.
//  - MIDI out: the "MIDI out" thread drains host.popMidiOut into the chosen
//    output (or discards the bytes when there is none), and pumps the feeder.
class AudioEngine : private juce::AudioIODeviceCallback,
                    private juce::MidiInputCallback,
                    private juce::Thread {
 public:
  explicit AudioEngine(mirage::RealtimeHost& host);
  ~AudioEngine() override;

  // Opens the audio device (the saved state, or the default output with no input
  // channel: nothing is plugged into the jack until the user picks an input in Audio
  // Settings) and starts the callback. Returns an error message, empty on success.
  juce::String start(const juce::XmlElement* savedState);

  juce::AudioDeviceManager& devices() { return deviceManager_; }

  void setMidiInput(const juce::String& identifier);  // empty = none
  void setMidiOutput(const juce::String& identifier);
  juce::String midiInputId() const { return midiInId_; }
  juce::String midiOutputId() const;

  // The MIDI input feeder: UI code calls clear() before postPanic and reads
  // droppedBytes() for the status strip.
  mirage::MidiInFeeder& midiFeeder() { return feeder_; }

  double cpuLoad() const { return deviceManager_.getCpuUsage(); }  // 0..1
  void setVolume(float gain01) { volume_.store(gain01, std::memory_order_relaxed); }
  float volume() const { return volume_.load(std::memory_order_relaxed); }
  // Linear gain of the audio input before the jack, 0..4 (RealtimeHost::setInputGain).
  void setInputGain(float gain);
  float inputGain() const { return inputGain_.load(std::memory_order_relaxed); }
  // Message thread: the open device has at least one active input channel.
  bool hasInput() const;
  // The state to save at quit. If start() had to fall back (the saved devices could not be
  // opened) it is the saved state itself, so the user's choice survives a launch with the
  // interface unplugged; acceptCurrentAudioState() ends that (the user is choosing devices).
  std::unique_ptr<juce::XmlElement> saveAudioState() const;
  void acceptCurrentAudioState() { keptState_.reset(); }  // message thread

 private:
  // AudioIODeviceCallback
  void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
  void audioDeviceStopped() override;
  void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out,
                                        int numOut, int numSamples,
                                        const juce::AudioIODeviceCallbackContext&) override;
  // MidiInputCallback
  void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& msg) override;
  // Thread: the MIDI-out pump
  void run() override;

  mirage::RealtimeHost& host_;
  mirage::MidiInFeeder feeder_;
  juce::AudioDeviceManager deviceManager_;

  std::atomic<bool> ready_{false};  // prepare() done and the device running
  bool openedAsSaved(const juce::XmlElement& saved, const juce::String& error) const;
  std::unique_ptr<juce::XmlElement> keptState_;  // message thread: see saveAudioState()

  std::atomic<float> volume_{1.0f};
  std::atomic<float> inputGain_{1.0f};
  float lastGain_ = 1.0f;  // audio thread: gain at the end of the previous block
  size_t maxBlock_ = 0;    // set in audioDeviceAboutToStart, read by the callback
  std::vector<float> inMono_;  // maxBlock_ samples; sized in audioDeviceAboutToStart only
  std::vector<float> scratchOut_;  // the same, where the machine renders with no active output channel
  double rate_ = 0.0;

  juce::String midiInId_;  // message thread

  mutable juce::CriticalSection outLock_;  // guards midiOut_ and midiOutId_
  std::unique_ptr<juce::MidiOutput> midiOut_;
  juce::String midiOutId_;
  mirage::MidiByteParser outParser_;  // MIDI-out thread only
};
