#include "InputMeter.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>

void InputMeter::update(float peak, bool hasInput) {
  const float next = hasInput ? std::max(peak, shown_ * 0.7f) : 0.0f;
  if (next == shown_ && hasInput == hasInput_) return;
  shown_ = next;
  hasInput_ = hasInput;
  repaint();
}

void InputMeter::paint(juce::Graphics& g) {
  auto r = getLocalBounds().toFloat().reduced(0.0f, 6.0f);
  g.setColour(juce::Colour(0xff17171b));
  g.fillRoundedRectangle(r, 2.0f);
  if (!hasInput_) {
    g.setColour(juce::Colour(0xff7c7c84));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("no input", getLocalBounds(), juce::Justification::centred);
    return;
  }
  const float db = juce::Decibels::gainToDecibels(shown_, -60.0f);
  const float fill = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
  g.setColour(shown_ >= 1.0f ? juce::Colour(0xffe04848)
                             : db > -6.0f ? juce::Colour(0xffe0b040) : juce::Colour(0xff58c070));
  g.fillRoundedRectangle(r.withWidth(r.getWidth() * fill), 2.0f);
}
