#pragma once
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <memory>

#include "mirage/midi_byte_parser.h"
#include "mirage/midi_in_feeder.h"
#include "mirage/realtime_host.h"

// Connects a RealtimeHost to the audio device and the MIDI devices.
//  - Audio: the device callback renders host.process() into channel 0 and
//    copies it to the other outputs.
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

  // Opens the audio device (the saved state, or 0 inputs / 2 outputs) and starts
  // the callback. Returns an error message, empty on success.
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
  std::unique_ptr<juce::XmlElement> saveAudioState() const {
    return deviceManager_.createStateXml();
  }

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
  std::atomic<float> volume_{1.0f};
  float lastGain_ = 1.0f;  // audio thread: gain at the end of the previous block
  size_t maxBlock_ = 0;    // set in audioDeviceAboutToStart, read by the callback
  double rate_ = 0.0;

  juce::String midiInId_;  // message thread

  mutable juce::CriticalSection outLock_;  // guards midiOut_ and midiOutId_
  std::unique_ptr<juce::MidiOutput> midiOut_;
  juce::String midiOutId_;
  mirage::MidiByteParser outParser_;  // MIDI-out thread only
};
