#include "PluginProcessor.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "ImportDialog.h"
#include "PluginEditor.h"

namespace {

using namespace std::chrono_literals;

// Block MIDI events beyond this many in one block are dropped and counted (a
// DAW never sends nearly so many). Reserved in prepareToPlay.
constexpr size_t kMaxBlockEvents = 8192;
// The longest MIDI-out SysEx passed on: the most a juce::MidiBuffer event holds
// (its size is a uint16). A longer one is dropped and counted.
constexpr size_t kMidiOutSysex = 65535;
// MIDI out per block: that SysEx plus a full 8 KB MIDI-out queue of single
// bytes, each stored with 6 bytes of MidiBuffer header.
constexpr size_t kMidiOutBytes = kMidiOutSysex + mirage::RealtimeHost::kMidiOutQueueBytes * 7;
// A snapshot is "MIRG", u32 version, then the boot ROM's SHA-1.
constexpr size_t kSnapshotShaOffset = 8;

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout() {
  // The app's sliders: Volume 0..1, Input Gain 0..4 with 1 in the middle.
  juce::NormalisableRange<float> gain(0.0f, 4.0f, 0.01f);
  gain.setSkewForCentre(1.0f);
  juce::AudioProcessorValueTreeState::ParameterLayout layout;
  layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"volume", 1}, "Volume",
                                                         juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"inputGain", 1}, "Input Gain", gain, 1.0f));
  return layout;
}

// DiskSession speaks std::filesystem::path and UTF-8; JUCE speaks String.
std::filesystem::path toPath(const juce::String& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.toRawUTF8())));
}

juce::String text(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return juce::String::fromUTF8(reinterpret_cast<const char*>(u.data()), (int)u.size());
}

void setParameterValue(juce::AudioProcessorValueTreeState& params, const juce::String& id, float value) {
  if (auto* p = params.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(value));
}

}  // namespace

MirageProcessor::MirageProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input Jack", juce::AudioChannelSet::stereo(), false)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      params(*this, nullptr, "MirageParameters", makeLayout()) {
  volume_ = params.getRawParameterValue("volume");
  inputGain_ = params.getRawParameterValue("inputGain");
  startMachine();
  startTimerHz(10);
}

MirageProcessor::~MirageProcessor() { stopTimer(); }

bool MirageProcessor::startMachine() {
  const std::lock_guard lock(stateMutex_);
  return startMachineLocked();
}

bool MirageProcessor::requestStartMachine() {
  // Inside session work this thread holds the lock already, and the session
  // must outlive that work: the timer starts it.
  std::unique_lock lock(stateMutex_, std::defer_lock);
  if (sessionHeld_ || !lock.try_lock()) {
    startPending_ = true;
    return false;
  }
  return startMachineLocked();
}

bool MirageProcessor::startMachineLocked() {
  startPending_ = false;
  // A project's Mirage kept unrestored (silent_: no machine runs) may need
  // another ROM: rebuild from the one found now.
  const bool unrestored = !keptBlob_.isEmpty() && !restorePending_;
  if (host_ && !unrestored) return true;
  juce::File file;
  const auto rom = findBootRom(file);
  if (!rom) return host_ != nullptr;
  if (host_ && *rom == hostRom_) {  // the same ROM (so the same SHA-1): only retry the restore
    restoreLocked(juce::MemoryBlock(keptBlob_));
    return true;
  }
  if (host_) {
    live_.store(nullptr, std::memory_order_release);
    retired_.push_back({std::move(host_), std::move(session_)});
    snapshotsOut_ = restoresOut_ = 0;  // the replaced host's
    pendingIdentity_.reset();
    deferred_.clear();  // queued for the replaced session
  }
  hostRom_ = *rom;
  host_ = std::make_unique<mirage::RealtimeHost>(
      std::span<const uint8_t>(static_cast<const uint8_t*>(rom->getData()), rom->getSize()),
      mirage::HostOptions{.muteCalibration = true, .fastDisk = true});
  session_ = std::make_unique<mirage::DiskSession>(*host_);
  if (preparedBlock_ > 0) host_->prepare((size_t)preparedBlock_, preparedRate_);
  live_.store(host_.get(), std::memory_order_release);
  if (!keptBlob_.isEmpty() && !restorePending_) restoreLocked(juce::MemoryBlock(keptBlob_));
  return true;
}

void MirageProcessor::withSession(std::function<void(mirage::DiskSession&)> work) {
  if (!trySession(work) && session_) deferred_.push_back(std::move(work));
}

bool MirageProcessor::trySession(const std::function<void(mirage::DiskSession&)>& work) {
  if (!session_) return false;
  if (sessionHeld_) {
    work(*session_);
    return true;
  }
  if (!deferred_.empty()) return false;  // after the queued work
  std::unique_lock lock(stateMutex_, std::try_to_lock);
  if (!lock.owns_lock()) return false;
  const juce::ScopedValueSetter held(sessionHeld_, true);
  work(*session_);
  return true;
}

void MirageProcessor::discardSavedState(std::function<void()> then) {
  withSession([this, then = std::move(then)](mirage::DiskSession&) {
    keptBlob_.reset();
    restorePending_ = false;
    pendingIdentity_.reset();
    setRestoreProblem({});
    host_->postReset();
    if (then) then();
  });
}

void MirageProcessor::setRestoreProblem(const juce::String& why) {
  silent_.store(why.isNotEmpty(), std::memory_order_relaxed);  // unrestorable: run no machine
  const std::lock_guard lock(problemMutex_);
  restoreProblem_ = why;
}

juce::String MirageProcessor::restoreProblem() const {
  const std::lock_guard lock(problemMutex_);
  return restoreProblem_;
}

juce::String MirageProcessor::saveProblem() const {
  const std::lock_guard lock(problemMutex_);
  return saveProblem_;
}

void MirageProcessor::setSaveProblem(const juce::String& why) {
  if (why.isNotEmpty()) juce::Logger::writeToLog("Mirage: " + why);
  const std::lock_guard lock(problemMutex_);
  saveProblem_ = why;
}

void MirageProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) {
  const std::lock_guard lock(stateMutex_);
  preparedRate_ = sampleRate;
  preparedBlock_ = std::max(maximumExpectedSamplesPerBlock, 1);
  if (host_) host_->prepare((size_t)preparedBlock_, preparedRate_);
  volumeRamp_.reset(sampleRate, 0.020);
  volumeRamp_.setCurrentAndTargetValue(volume_->load());
  events_.reserve(kMaxBlockEvents);
  if (!midiOutParser_) midiOutParser_ = std::make_unique<mirage::MidiByteParser>(kMidiOutSysex);
  emitMidiOut_ = [this](std::span<const uint8_t> m) {
    if (!midiOutDest_->addEvent(m.data(), (int)m.size(), midiOutFrame_)) ++midiLost_;
  };
  midiOut_.ensureSize(kMidiOutBytes);
  midiOutStorage_ = midiOut_.data.getRawDataPointer();
}

bool MirageProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
  const auto out = layouts.getMainOutputChannelSet();
  if (out != mono && out != stereo) return false;
  if (layouts.inputBuses.isEmpty()) return true;
  const auto jack = layouts.getChannelSet(true, 0);
  return jack.isDisabled() || jack == mono || jack == stereo;
}

void MirageProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
  juce::ScopedNoDenormals noDenormals;
  auto* host = live_.load(std::memory_order_acquire);
  auto out = getBusBuffer(buffer, false, 0);
  if (host == nullptr || silent_.load(std::memory_order_relaxed) || out.getNumChannels() == 0 ||
      midiOutParser_ == nullptr) {
    buffer.clear();
    midi.clear();
    return;
  }
  const int frames = buffer.getNumSamples();
  host->setOffline(isNonRealtime());

  // The jack: nothing plugged in while its bus is off, otherwise the average of
  // its channels. Averaged in place into output channel 0 (the jack's channel 0
  // is the same buffer channel), which the host may read and write at once.
  float* ch0 = out.getWritePointer(0);
  const float* in = nullptr;
  const auto jack = getBusBuffer(buffer, true, 0);
  if (const int n = jack.getNumChannels(); n > 0) {
    const float norm = 1.0f / (float)n;
    for (int i = 0; i < frames; ++i) {
      float s = 0.0f;
      for (int c = 0; c < n; ++c) s += jack.getReadPointer(c)[i];
      ch0[i] = s * norm;
    }
    in = ch0;
  }
  host->setInputGain(inputGain_->load(std::memory_order_relaxed));

  events_.clear();
  for (const auto meta : midi) {
    // ponytail: events past kMaxBlockEvents are dropped (counted); raise it if a DAW ever sends more.
    if (events_.size() == events_.capacity()) {
      midiLost_ += (uint64_t)meta.numBytes;
      continue;
    }
    events_.push_back({(uint32_t)std::max(0, meta.samplePosition),
                       std::span<const uint8_t>(meta.data, (size_t)meta.numBytes)});
  }

  const uint64_t c0 = host->status().cycles;
  host->process(in, ch0, (size_t)frames, getSampleRate(), events_);
  const uint64_t c1 = host->status().cycles;

  volumeRamp_.setTargetValue(volume_->load(std::memory_order_relaxed));
  volumeRamp_.applyGain(ch0, frames);
  for (int c = 1; c < out.getNumChannels(); ++c) out.copyFrom(c, 0, ch0, frames);

  // MIDI out, each message at the frame its cycle maps to. It is written into
  // the storage reserved in prepareToPlay, so the host's buffer never grows
  // here: if the host's buffer already holds that storage (handed over by an
  // earlier block), write there; otherwise write into midiOut_ and swap.
  // ponytail: assumes the host passes the same MidiBuffer each block (JUCE's
  // VST3 wrapper does); one that rotates buffers could make addEvent allocate.
  midiOutDest_ = midi.data.getRawDataPointer() == midiOutStorage_ ? &midi : &midiOut_;
  midiOutDest_->clear();
  const double elapsed = (double)(c1 - c0);
  mirage::MidiOutByte b;
  while (host->popMidiOut(b)) {
    const double at = elapsed > 0 ? (double)(int64_t)(b.cycle - c0) * frames / elapsed : 0.0;
    midiOutFrame_ = (int)std::clamp(at, 0.0, (double)std::max(frames - 1, 0));
    midiOutParser_->feed(b.byte, emitMidiOut_);
  }
  if (midiOutDest_ != &midi) midi.swapWith(midiOut_);
  midiDropped_.store(midiLost_ + midiOutParser_->dropped(), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* MirageProcessor::createEditor() {
  return new MirageEditor(*this);
}

// ---- State -----------------------------------------------------------------

bool MirageProcessor::await(const std::function<bool()>& done) {
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  auto lastChange = start;
  uint64_t blocks = host_->status().blocks;
  const bool neverRan = blocks == 0;  // a project loading: pump at once
  std::optional<clock::time_point> pumpingSince;
  for (;;) {
    if (done()) return true;
    const auto now = clock::now();
    if (const auto b = host_->status().blocks; b != blocks) {
      blocks = b;
      lastChange = now;
      pumpingSince.reset();  // blocks run again: a later fallback gets its full second
    }
    // Blocks stopped for 200 ms: the audio is off. Still running after 2 s: the
    // request is deferred (a disk swap's gap, a key held), so pump anyway. Gives
    // up after 1 s of failed pumps, or 5 s in all.
    if (neverRan || now - lastChange >= 200ms || now - start >= 2s) {
      if (!pumpingSince) pumpingSince = now;
      if (host_->pumpWhileIdle() && done()) return true;
      if (now - *pumpingSince >= 1s || now - start >= 5s) return false;
    }
    std::this_thread::sleep_for(1ms);
  }
}

bool MirageProcessor::confirmRestores() {
  while (restoresOut_ > 0) {
    if (host_->takeReturnedMachine() == nullptr) return false;  // the old machine is freed here
    --restoresOut_;
  }
  return true;
}

void MirageProcessor::finishRestore() {
  restorePending_ = false;
  keptBlob_.reset();
  adoptIdentity();
}

void MirageProcessor::adoptIdentity() {
  // Refused while a save is in flight: the timer retries after poll() ends it.
  if (pendingIdentity_ && session_->adoptRestored(toPath(pendingIdentity_->path),
                                                  (mirage::DiskFormat)juce::jlimit(0, 1, pendingIdentity_->format),
                                                  pendingIdentity_->untitled))
    pendingIdentity_.reset();
}

void MirageProcessor::getStateInformation(juce::MemoryBlock& destData) {
  const std::lock_guard lock(stateMutex_);
  if (!keptBlob_.isEmpty()) {  // pending or unrestorable: as it came
    destData = keptBlob_;
    return;
  }
  PluginStateData d;
  if (host_) {
    // Reserved here, filled by the audio thread (or a pump); twice the size if
    // too small. Sized (zero-filled) and then emptied rather than reserved: its
    // pages are touched here, so the audio thread's write takes no page faults.
    std::optional<mirage::ReturnedSnapshot> got;
    auto buf = std::make_unique<std::vector<uint8_t>>();
    for (size_t capacity = lastSnapshotSize_ + 64 * 1024;; capacity *= 2) {
      buf->resize(capacity);
      buf->clear();
      if (!host_->postSnapshot(std::move(buf))) break;
      ++snapshotsOut_;
      // Snapshots return in order: ours is the last; older ones (from a call
      // that gave up waiting) are freed on the way.
      const bool back = await([&] {
        while (snapshotsOut_ > 0) {
          auto r = host_->takeReturnedSnapshot();
          if (!r) return false;
          if (--snapshotsOut_ == 0) got = std::move(r);
        }
        return true;
      });
      if (!back || got->ok) break;
      buf = std::move(got->bytes);
      got.reset();
    }
    if (!got || !got->ok) {  // no snapshot within the wait: the last state rather than none
      setSaveProblem("The Mirage could not be saved into the project: the audio thread did not answer. " +
                     (lastBlob_.isEmpty()
                          ? juce::String("The project holds no state.")
                          : "The project holds the state from " + lastBlobTime_.toString(true, true) + "."));
      destData = lastBlob_;
      return;
    }
    lastSnapshotSize_ = got->bytes->size();
    d.machine = juce::MemoryBlock(got->bytes->data(), got->bytes->size());
    if (host_->status().diskPresent) {
      if (pendingIdentity_) {
        d.diskPath = pendingIdentity_->path;
        d.diskFormat = pendingIdentity_->format;
        d.diskUntitled = pendingIdentity_->untitled;
      } else if (session_->hasDisk()) {
        d.diskPath = text(session_->path());
        d.diskFormat = (int)session_->format();
        d.diskUntitled = session_->path().empty();
      }
    }
  }
  d.volume = volume_->load();
  d.inputGain = inputGain_->load();
  d.editorW = editorW_.load();
  d.editorH = editorH_.load();
  destData = lastBlob_ = encodePluginState(d);
  lastBlobTime_ = juce::Time::getCurrentTime();
  setSaveProblem({});
}

void MirageProcessor::setStateInformation(const void* data, int sizeInBytes) {
  const std::lock_guard lock(stateMutex_);
  restoreLocked(juce::MemoryBlock(data, (size_t)std::max(sizeInBytes, 0)));
}

void MirageProcessor::restoreLocked(const juce::MemoryBlock& blob) {
  keptBlob_ = blob;  // handed back as it is until the restore is confirmed (or for good)
  restorePending_ = false;
  pendingIdentity_.reset();
  const auto d = decodePluginState(blob.getData(), blob.getSize());
  if (!d)
    return setRestoreProblem("This project's Mirage cannot be read: the saved state is damaged or from a newer "
                      "version of the plugin.");
  setParameterValue(params, "volume", d->volume);
  setParameterValue(params, "inputGain", d->inputGain);
  editorW_ = d->editorW;
  editorH_ = d->editorH;
  if (d->machine.isEmpty()) {  // saved before any ROM was found: nothing to restore
    keptBlob_.reset();
    return setRestoreProblem({});
  }
  if (!host_) return setRestoreProblem("Choose the Mirage boot ROM to restore this project's Mirage.");
  const auto* bytes = static_cast<const uint8_t*>(d->machine.getData());
  const size_t size = d->machine.getSize();
  const auto sha = host_->romSha1();
  if (size >= kSnapshotShaOffset + sha.size() && !std::equal(sha.begin(), sha.end(), bytes + kSnapshotShaOffset))
    return setRestoreProblem("This project's Mirage was saved with boot ROM " +
                      juce::String::toHexString(bytes + kSnapshotShaOffset, (int)sha.size(), 0) +
                      "; choose that ROM to restore it.");
  std::unique_ptr<mirage::Machine> m;
  try {
    m = host_->buildMachine(std::span<const uint8_t>(bytes, size));
  } catch (const std::exception& e) {
    return setRestoreProblem("This project's Mirage cannot be restored: " + juce::String(e.what()));
  }
  if (!host_->postRestore(std::move(m)))
    return setRestoreProblem("This project's Mirage cannot be restored: the plugin is busy.");
  setRestoreProblem({});
  ++restoresOut_;
  restorePending_ = true;
  lastSnapshotSize_ = size;
  lastBlob_ = blob;
  lastBlobTime_ = juce::Time::getCurrentTime();
  pendingIdentity_ = DiskIdentity{d->diskPath, d->diskFormat, d->diskUntitled};
  if (await([this] { return confirmRestores(); })) finishRestore();
  // Otherwise the 10 Hz timer confirms it; the blob stays pending until then.
}

void MirageProcessor::timerCallback() {
  if (host_) {
    while (auto r = host_->takeReturnedWave()) importNotice_ = importNotice(*r);
  }
  std::unique_lock lock(stateMutex_, std::try_to_lock);
  if (!lock.owns_lock()) return;  // a state call is running: next tick
  if (startPending_) startMachineLocked();
  freeRetired();
  if (!session_) return;
  const juce::ScopedValueSetter held(sessionHeld_, true);
  if (auto outcome = session_->poll()) saveOutcome_ = std::move(outcome);
  // Always: this also frees restores that a later, unrestorable blob superseded.
  const bool confirmed = confirmRestores();
  if (!restorePending_)
    adoptIdentity();
  else if (confirmed)
    finishRestore();
  // The editor's disk work that found the lock busy, in order.
  while (!deferred_.empty() && session_) {
    auto work = std::move(deferred_.front());
    deferred_.pop_front();
    work(*session_);
  }
}

void MirageProcessor::freeRetired() {
  if (retired_.empty()) return;
  // The audio thread has moved on to the new host once it ran a block on it
  // (or never ran one), and the editor once it shows the new host.
  if (preparedBlock_ > 0 && host_->status().blocks == 0) return;
  if (auto* editor = dynamic_cast<MirageEditor*>(getActiveEditor()); editor && editor->shownHost() != host_.get())
    return;
  retired_.clear();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MirageProcessor(); }
