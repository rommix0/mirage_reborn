#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// The input level meter in the status strip: a bar on a dB scale (-60 to 0 dB
// of the jack's full scale), red when the level reaches 1.0, and "no input" when
// the open device has no active input channel.
class InputMeter : public juce::Component {
 public:
  // `peak`: linear, after the input gain (HostStatus::inputPeak). A rise shows at
  // once and a fall decays, because the status holds only the last block's peak.
  void update(float peak, bool hasInput);
  void paint(juce::Graphics&) override;

 private:
  float shown_ = 0.0f;
  bool hasInput_ = false;
};
