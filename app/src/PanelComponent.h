#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>
#include <vector>

#include "mirage/panel_layout.h"
#include "mirage/realtime_host.h"

// The replica front panel of the later grey DSK-8, drawn as vectors from
// panel_layout.h (panel units, 1000 x 300) and scaled to fit the component,
// keeping its aspect ratio. Each button is a child component that posts its
// presses to the host. The LED display, the drive LED and the disk name follow
// RealtimeHost::status(), polled at 30 Hz and repainted only when they change.
class PanelComponent : public juce::Component, private juce::Timer {
 public:
  explicit PanelComponent(mirage::RealtimeHost& host);
  // A still panel showing `status` and `diskName` (the --panel-png render):
  // no host, no polling; presses only show on the buttons.
  PanelComponent(const mirage::HostStatus& status, const juce::String& diskName);
  ~PanelComponent() override;

  // A press or release of `key` from one source (a button, or one keyboard
  // key). The host sees the press when the first source goes down and the
  // release when the last one comes up; the button shows pressed meanwhile.
  void press(mirage::PanelKey key, bool down);
  bool isHeld(mirage::PanelKey key) const { return holds_[size_t(key)] != 0; }

  void setDiskName(const juce::String& name);  // the inserted disk's file name
  // 0..1, the analog output volume. The owner connects it to the audio engine.
  juce::Slider& volumeSlider() { return volume_; }

  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  class Button;
  class SliderLook;

  PanelComponent(mirage::RealtimeHost* host, const mirage::HostStatus& status);
  void timerCallback() override;

  juce::AffineTransform panelToLocal() const;  // panel units -> component pixels
  void repaintPanelArea(juce::Rectangle<float> panelArea);
  void paintCase(juce::Graphics&) const;
  void paintLegends(juce::Graphics&) const;
  void paintBranding(juce::Graphics&) const;
  void paintDisplay(juce::Graphics&) const;
  void paintDrive(juce::Graphics&) const;

  mirage::RealtimeHost* host_;  // null for a still panel
  mirage::HostStatus status_;
  juce::String diskName_;
  std::array<uint8_t, mirage::kPanelKeyCount> holds_{};  // sources holding each key
  std::unique_ptr<juce::Drawable> ensoniqLogo_;    // the embedded badge artwork
  std::unique_ptr<juce::Drawable> mirageWordmark_;  // the embedded "Mirage" wordmark
  juce::Path subtitle_;  // "DIGITAL SAMPLING KEYBOARD" in Michroma, in panel units
  std::unique_ptr<SliderLook> sliderLook_;  // declared before volume_, so it outlives it
  juce::Slider volume_;
  std::vector<std::unique_ptr<Button>> buttons_;  // indexed by PanelKey
};
