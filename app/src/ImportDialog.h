#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

#include "mirage/wav_import.h"

// The File > Import WAV... dialog (spec 6.7). It is a window of its own, so
// the replica panel stays faithful. It shows the file's waveform with
// draggable start and end markers, the import settings (start and end in
// seconds, rate, wave RAM half, start page, normalise) and live text from
// planImport: the selected length, how much of the half it fills, the pages
// it occupies, a truncation warning, the pitch hint and any problem. Import is
// disabled while there is a problem or canImport() says no (polled at 10 Hz:
// the drive LED is on, or no audio device runs). It converts nothing itself:
// Import calls onImport with the settings, and the owner converts, posts the
// write and closes the dialog.
class ImportDialog : public juce::Component, private juce::Timer {
 public:
  static constexpr int kWidth = 760, kHeight = 520;

  ImportDialog(std::shared_ptr<const mirage::WavAudio> audio, const juce::String& fileName,
               std::function<bool()> canImport);
  ~ImportDialog() override;

  std::function<void(const mirage::ImportSettings&)> onImport;
  std::function<void()> onCancel;

  mirage::ImportSettings settings() const;

  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  class WaveformView;

  void timerCallback() override;
  void setSelection(double start, double end);  // keeps end at least one sample after start
  void update();  // the plan, the text and the Import button
  bool canNow() const;

  std::shared_ptr<const mirage::WavAudio> audio_;
  std::function<bool()> canImport_;
  double duration_ = 0;  // the file's length in seconds
  double start_ = 0, end_ = 0;
  bool can_ = true;  // canImport() as update() last saw it

  juce::Label fileLabel_;
  std::unique_ptr<WaveformView> waveform_;
  juce::Label startLabel_, endLabel_, rateLabel_, halfLabel_, pageLabel_;
  juce::Slider startSlider_, endSlider_, rateSlider_, pageSlider_;
  juce::ComboBox halfBox_;
  juce::ToggleButton normalise_{"Normalise"};
  juce::TextButton defaultRate_{"Default"};
  juce::Label info_, warning_;  // warning_: truncation, a problem, or why Import is off
  juce::TextButton importButton_{"Import"}, cancelButton_{"Cancel"};
};
