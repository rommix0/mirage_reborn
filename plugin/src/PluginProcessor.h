#pragma once
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "PluginSettings.h"
#include "PluginState.h"
#include "mirage/disk_session.h"
#include "mirage/midi_byte_parser.h"
#include "mirage/realtime_host.h"

// The Mirage as a VST3 instrument (design §4-§6). The audio thread runs the
// machine through RealtimeHost in processBlock. getStateInformation and
// setStateInformation run on whichever thread the DAW calls them from,
// serialised by stateMutex_, and never wait on the message thread. The
// DiskSession and its 10 Hz poll live here, not in the editor; whoever uses the
// DiskSession off the timer holds stateMutex_.
class MirageProcessor : public juce::AudioProcessor, public juce::VST3ClientExtensions, private juce::Timer {
 public:
  MirageProcessor();
  ~MirageProcessor() override;

  void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
  void releaseResources() override {}
  bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
  using AudioProcessor::processBlock;

  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override { return true; }

  const juce::String getName() const override { return JucePlugin_Name; }
  bool acceptsMidi() const override { return true; }
  bool producesMidi() const override { return true; }
  bool isMidiEffect() const override { return false; }
  // The machine runs and can sound with no input (its sequencer, a held note).
  double getTailLengthSeconds() const override { return std::numeric_limits<double>::infinity(); }

  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String&) override {}

  void getStateInformation(juce::MemoryBlock& destData) override;
  void setStateInformation(const void* data, int sizeInBytes) override;

  // The Input Jack is an auxiliary (sidechain) input, not a main one.
  juce::VST3ClientExtensions* getVST3ClientExtensions() override { return this; }
  bool getPluginHasMainInput() const override { return false; }

  // Builds the machine once a boot ROM is found (the constructor tries; the
  // editor calls this after setBootRom). A project state kept for want of a ROM
  // is then restored; when a machine exists but the project's could not be
  // restored, it is rebuilt from the ROM found now and the restore retried.
  // True when a machine exists. Blocks on the state lock: for the constructor.
  bool startMachine();
  // startMachine for the message thread, which never waits for the state lock:
  // when it is busy, the 10 Hz timer does it (false until then).
  bool requestStartMachine();
  // Why the project's Mirage could not be restored; empty when it was (or there
  // was nothing to restore). Any thread.
  juce::String restoreProblem() const;
  // Why the last getStateInformation could not take a snapshot (the audio
  // thread did not answer) and handed the DAW an older state, or none; empty
  // once a later one succeeds. Any thread.
  juce::String saveProblem() const;

  // The editor's view of the processor. Message thread.
  mirage::RealtimeHost* host() const { return host_.get(); }  // null until a ROM is found
  // Runs `work` with the DiskSession under the state lock: now if the lock is
  // free or this call is already inside such work, else queued for the 10 Hz
  // timer (a state call may hold the lock for seconds; the message thread
  // never waits for it). Dropped while there is no machine.
  void withSession(std::function<void(mirage::DiskSession&)> work);
  // As withSession, but only now: false (and nothing run) if it would queue.
  bool trySession(const std::function<void(mirage::DiskSession&)>& work);
  // A finished save's outcome, once. Kept until an editor takes it, so a save
  // that ends while the editor is closed is reported when it next opens.
  std::optional<mirage::SaveOutcome> takeSaveOutcome() { return std::exchange(saveOutcome_, std::nullopt); }
  // An Import WAV's outcome (a wave write back from the audio thread), once.
  juce::String takeImportNotice() { return std::exchange(importNotice_, {}); }
  // Gives up the project's Mirage that could not be restored: power-cycles the
  // machine, which runs again, then runs `then`. Through withSession.
  void discardSavedState(std::function<void()> then);
  int editorWidth() const { return editorW_.load(); }
  int editorHeight() const { return editorH_.load(); }
  void setEditorSize(int w, int h) {
    editorW_ = w;
    editorH_ = h;
  }

  juce::AudioProcessorValueTreeState params;

 private:
  void timerCallback() override;
  // Both under stateMutex_.
  void restoreLocked(const juce::MemoryBlock& blob);
  // Polls `done` each 1 ms until it is true. Pumps the machine directly
  // (pumpWhileIdle) when no block has ever run, when blocks have stopped for
  // 200 ms (audio stopped), or after 2 s in any case; pumps are retried for 1 s.
  // False if `done` never came true.
  bool await(const std::function<bool()>& done);
  void setSaveProblem(const juce::String& why);  // logged when not empty
  bool confirmRestores();  // frees the machines restores replaced; true when none is pending
  void finishRestore();    // a confirmed restore: no longer pending; adopt its disk identity
  void adoptIdentity();    // pendingIdentity_ into the DiskSession, unless a save is in flight
  void setRestoreProblem(const juce::String& why);  // also silences (or not) the machine
  bool startMachineLocked();  // under stateMutex_
  void freeRetired();         // under stateMutex_, on the timer

  juce::SharedResourcePointer<PluginShared> shared_;
  std::unique_ptr<mirage::RealtimeHost> host_;
  std::unique_ptr<mirage::DiskSession> session_;
  juce::MemoryBlock hostRom_;  // the ROM host_ was built from
  bool startPending_ = false;  // requestStartMachine found the lock busy (message thread)
  // Hosts (and their sessions) replaced by startMachine, kept until the audio
  // thread has run a block on the new host (it may still hold a replaced
  // host's pointer) and the editor shows the new one (freeRetired).
  struct Retired {
    std::unique_ptr<mirage::RealtimeHost> host;
    std::unique_ptr<mirage::DiskSession> session;  // destroyed first
  };
  std::vector<Retired> retired_;
  // Message thread: DiskSession work waiting for the state lock, and whether
  // such work (or the timer) holds it now.
  std::deque<std::function<void(mirage::DiskSession&)>> deferred_;
  bool sessionHeld_ = false;
  std::optional<mirage::SaveOutcome> saveOutcome_;  // message thread
  juce::String importNotice_;                       // message thread
  // host_, published as soon as startMachine builds it (prepared first if
  // prepareToPlay already ran); read by the audio thread.
  std::atomic<mirage::RealtimeHost*> live_{nullptr};
  std::atomic<float>* volume_ = nullptr;              // the parameters' raw values
  std::atomic<float>* inputGain_ = nullptr;

  // Audio thread (set up in prepareToPlay).
  juce::LinearSmoothedValue<float> volumeRamp_;
  std::vector<mirage::BlockMidiEvent> events_;
  std::unique_ptr<mirage::MidiByteParser> midiOutParser_;
  std::function<void(std::span<const uint8_t>)> emitMidiOut_;
  juce::MidiBuffer midiOut_;           // reserved storage for MIDI out
  const void* midiOutStorage_ = nullptr;  // its data, wherever it is now (see processBlock)
  juce::MidiBuffer* midiOutDest_ = nullptr;
  int midiOutFrame_ = 0;

  // State (under stateMutex_). keptBlob_: a project blob returned as it is
  // from getStateInformation while its restore is pending or impossible.
  std::mutex stateMutex_;
  juce::MemoryBlock keptBlob_;
  bool restorePending_ = false;
  std::atomic<bool> silent_{false};  // an unrestorable blob: run no machine
  juce::String restoreProblem_, saveProblem_;
  mutable std::mutex problemMutex_;  // guards both problem strings
  size_t snapshotsOut_ = 0, restoresOut_ = 0;  // posted, not yet returned
  size_t lastSnapshotSize_ = 768 * 1024;  // a snapshot with a disk is about 610 KB
  juce::MemoryBlock lastBlob_;  // the last state handed out or restored
  juce::Time lastBlobTime_;     // when it was made
  // The disk identity of a restored machine, waiting for DiskSession to adopt it.
  struct DiskIdentity {
    juce::String path;
    int format = 0;
    bool untitled = false;
  };
  std::optional<DiskIdentity> pendingIdentity_;
  std::atomic<int> editorW_{0}, editorH_{0};
  double preparedRate_ = 0;
  int preparedBlock_ = 0;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MirageProcessor)
};
