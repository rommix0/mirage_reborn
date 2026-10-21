#include "ImportDialog.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace {

const juce::Colour kPlotBackground(0xff141418), kOutside(0xff4a4a58), kInside(0xff7ec8e3),
    kCutOff(0xffc07a40), kStartMarker(0xff7ee07e), kEndMarker(0xffe0a040), kWarning(0xffffb060);

// MIDI note name with C4 = 60 (47 is B2).
juce::String noteName(int key) {
  static const char* const kNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                       "F#", "G",  "G#", "A",  "A#", "B"};
  const int octave = (key >= 0 ? key / 12 : (key - 11) / 12) - 1;
  return juce::String(kNames[((key % 12) + 12) % 12]) + juce::String(octave);
}

juce::String hex(int value, int digits) {
  return juce::String::toHexString(value).toUpperCase().paddedLeft('0', digits);
}

juce::String seconds(double s) { return juce::String(s, 3) + " s"; }

}  // namespace

// ---- WaveformView ----------------------------------------------------------

// The file's mono mix as a min/max envelope (one pair per pixel column, made on
// resize), the selection between the start and end markers, and in another
// colour the part of the selection that does not fit in the half. Dragging
// moves the nearer marker.
class ImportDialog::WaveformView : public juce::Component {
 public:
  explicit WaveformView(const mirage::WavAudio& audio)
      : audio_(audio),
        duration_(audio.rate > 0 ? double(audio.mono.size()) / audio.rate : 0.0) {
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
  }

  std::function<void(double start, double end)> onDrag;

  // fitEnd: where the part that fits in the half ends (= end unless truncated).
  void setSelection(double start, double end, double fitEnd) {
    start_ = start;
    end_ = end;
    fitEnd_ = fitEnd;
    repaint();
  }

  void resized() override {
    const int w = plot().getWidth();
    envelope_.assign(size_t(std::max(w, 0)), {0.0f, 0.0f});
    const size_t n = audio_.mono.size();
    if (n == 0) return;
    for (int c = 0; c < w; ++c) {
      const size_t from = size_t(double(c) * double(n) / w);
      const size_t to = std::max(from + 1, size_t(double(c + 1) * double(n) / w));
      float lo = audio_.mono[from], hi = lo;
      for (size_t i = from; i < std::min(to, n); ++i) {
        lo = std::min(lo, audio_.mono[i]);
        hi = std::max(hi, audio_.mono[i]);
      }
      envelope_[size_t(c)] = {lo, hi};
    }
  }

  void paint(juce::Graphics& g) override {
    const auto area = plot();
    g.setColour(kPlotBackground);
    g.fillRect(getLocalBounds());
    const float mid = float(area.getCentreY()), half = area.getHeight() * 0.5f;
    g.setColour(kOutside.darker(0.4f));
    g.drawHorizontalLine(int(mid), float(area.getX()), float(area.getRight()));
    for (size_t c = 0; c < envelope_.size(); ++c) {
      const double t = (double(c) + 0.5) / double(envelope_.size()) * duration_;
      const bool in = t >= start_ && t <= end_;
      g.setColour(!in ? kOutside : t <= fitEnd_ ? kInside : kCutOff);
      const float x = float(area.getX() + int(c));
      const float top = mid - std::clamp(envelope_[c].second, -1.0f, 1.0f) * half;
      const float bottom = mid - std::clamp(envelope_[c].first, -1.0f, 1.0f) * half;
      g.drawVerticalLine(int(x), top, std::max(bottom, top + 1.0f));
    }
    drawMarker(g, start_, kStartMarker, true);
    drawMarker(g, end_, kEndMarker, false);
    g.setColour(juce::Colours::white.withAlpha(0.6f));
    g.setFont(juce::FontOptions(12.0f));
    const auto axis = getLocalBounds().removeFromBottom(kAxis).reduced(4, 0);
    g.drawText("0 s", axis, juce::Justification::centredLeft);
    g.drawText(seconds(duration_), axis, juce::Justification::centredRight);
  }

  void mouseDown(const juce::MouseEvent& e) override {
    const float x = e.position.x;
    const float ds = std::abs(x - toX(start_)), de = std::abs(x - toX(end_));
    draggingEnd_ = x > toX(end_) || (x >= toX(start_) && de < ds);
    mouseDrag(e);
  }

  void mouseDrag(const juce::MouseEvent& e) override {
    if (!onDrag) return;
    const double t = toSeconds(e.position.x);
    if (draggingEnd_)
      onDrag(start_, t);
    else
      onDrag(t, end_);
  }

 private:
  static constexpr int kAxis = 16;  // the time labels under the plot

  juce::Rectangle<int> plot() const {
    return getLocalBounds().withTrimmedBottom(kAxis).reduced(6, 6);
  }
  float toX(double t) const {
    const auto a = plot();
    return float(a.getX() + (duration_ > 0 ? t / duration_ : 0.0) * a.getWidth());
  }
  double toSeconds(float x) const {
    const auto a = plot();
    return std::clamp(double(x - a.getX()) / std::max(1, a.getWidth()), 0.0, 1.0) * duration_;
  }
  void drawMarker(juce::Graphics& g, double t, juce::Colour colour, bool isStart) const {
    const auto a = plot();
    const float x = toX(t);
    g.setColour(colour);
    g.drawLine(x, float(a.getY()), x, float(a.getBottom()), 2.0f);
    juce::Path flag;  // a small tab pointing into the selection
    const float dir = isStart ? 1.0f : -1.0f, y = float(a.getY());
    flag.addTriangle(x, y, x + 9.0f * dir, y, x, y + 10.0f);
    g.fillPath(flag);
  }

  const mirage::WavAudio& audio_;
  const double duration_;
  std::vector<std::pair<float, float>> envelope_;  // min, max per plot column
  double start_ = 0, end_ = 0, fitEnd_ = 0;
  bool draggingEnd_ = false;
};

// ---- ImportDialog ----------------------------------------------------------

ImportDialog::ImportDialog(std::shared_ptr<const mirage::WavAudio> audio,
                           const juce::String& fileName, std::function<bool()> canImport)
    : audio_(std::move(audio)),
      canImport_(std::move(canImport)),
      duration_(audio_->rate > 0 ? double(audio_->mono.size()) / audio_->rate : 0.0),
      end_(duration_) {
  const auto& a = *audio_;
  fileLabel_.setText(fileName + "    " + juce::String(juce::roundToInt(a.rate)) + " Hz, " +
                         juce::String(a.bits) + "-bit" + (a.isFloat ? " float" : "") + ", " +
                         (a.channels == 1 ? juce::String("mono")
                          : a.channels == 2 ? juce::String("stereo")
                                            : juce::String(a.channels) + " channels") +
                         ", " + seconds(duration_),
                     juce::dontSendNotification);
  fileLabel_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
  addAndMakeVisible(fileLabel_);

  waveform_ = std::make_unique<WaveformView>(a);
  waveform_->onDrag = [this](double s, double e) { setSelection(s, e); };
  addAndMakeVisible(*waveform_);

  auto label = [this](juce::Label& l, const juce::String& text) {
    l.setText(text, juce::dontSendNotification);
    l.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(l);
  };
  label(startLabel_, "Start");
  label(endLabel_, "End");
  label(rateLabel_, "Rate");
  label(halfLabel_, "Into");
  label(pageLabel_, "Start page");

  for (auto* s : {&startSlider_, &endSlider_}) {
    s->setSliderStyle(juce::Slider::LinearHorizontal);
    s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 22);
    s->setRange(0.0, std::max(duration_, 1e-6), 0.0);
    s->setNumDecimalPlacesToDisplay(3);
    s->setTextValueSuffix(" s");
    addAndMakeVisible(*s);
  }
  startSlider_.setValue(0.0, juce::dontSendNotification);
  endSlider_.setValue(duration_, juce::dontSendNotification);
  startSlider_.onValueChange = [this] { setSelection(startSlider_.getValue(), end_); };
  endSlider_.onValueChange = [this] { setSelection(start_, endSlider_.getValue()); };

  rateSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
  rateSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 90, 22);
  rateSlider_.setRange(mirage::kMinImportRate, mirage::kMaxImportRate, 1.0);
  rateSlider_.setTextValueSuffix(" Hz");
  rateSlider_.setValue(mirage::kDefaultImportRate, juce::dontSendNotification);
  rateSlider_.onValueChange = [this] { update(); };
  addAndMakeVisible(rateSlider_);
  defaultRate_.setTooltip("29,412 Hz, the Mirage's default sample rate");
  defaultRate_.onClick = [this] { rateSlider_.setValue(mirage::kDefaultImportRate); };
  addAndMakeVisible(defaultRate_);

  // Under OS 3.2 the lower sound lives in $00000-$0FFFF and the upper sound in
  // $10000-$1FFFF (docs/app.md).
  halfBox_.addItem("Lower sound ($00000)", 1);
  halfBox_.addItem("Upper sound ($10000)", 2);
  halfBox_.setSelectedId(1, juce::dontSendNotification);
  halfBox_.onChange = [this] { update(); };
  addAndMakeVisible(halfBox_);

  pageSlider_.setSliderStyle(juce::Slider::IncDecButtons);
  pageSlider_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 80, 22);
  pageSlider_.setRange(0.0, 255.0, 1.0);
  pageSlider_.textFromValueFunction = [](double v) {
    const int page = juce::roundToInt(v);
    return juce::String(page) + " ($" + hex(page, 2) + ")";
  };
  pageSlider_.valueFromTextFunction = [](const juce::String& t) { return double(t.getIntValue()); };
  pageSlider_.setValue(0.0, juce::dontSendNotification);
  pageSlider_.updateText();
  pageSlider_.onValueChange = [this] { update(); };
  addAndMakeVisible(pageSlider_);

  normalise_.setToggleState(true, juce::dontSendNotification);
  normalise_.setTooltip("Scale the selection so its peak uses the full 8 bits");
  normalise_.onClick = [this] { update(); };
  addAndMakeVisible(normalise_);

  info_.setJustificationType(juce::Justification::topLeft);
  info_.setFont(juce::FontOptions(15.0f));
  addAndMakeVisible(info_);
  warning_.setJustificationType(juce::Justification::topLeft);
  warning_.setFont(juce::FontOptions(15.0f));
  warning_.setColour(juce::Label::textColourId, kWarning);
  addAndMakeVisible(warning_);

  importButton_.onClick = [this] {
    if (onImport) onImport(settings());
  };
  cancelButton_.onClick = [this] {
    if (onCancel) onCancel();
  };
  addAndMakeVisible(importButton_);
  addAndMakeVisible(cancelButton_);

  setSize(kWidth, kHeight);
  update();
  startTimerHz(10);
}

ImportDialog::~ImportDialog() { stopTimer(); }

mirage::ImportSettings ImportDialog::settings() const {
  mirage::ImportSettings s;
  s.rate = rateSlider_.getValue();
  s.startSeconds = start_;
  s.endSeconds = end_;  // setSelection keeps it after the start, so never the "to the end" 0
  s.half = halfBox_.getSelectedId() == 2 ? mirage::WaveHalf::Upper : mirage::WaveHalf::Lower;
  s.startPage = juce::roundToInt(pageSlider_.getValue());
  s.normalize = normalise_.getToggleState();
  return s;
}

void ImportDialog::setSelection(double start, double end) {
  const double sample = audio_->rate > 0 ? 1.0 / audio_->rate : 0.0;
  start = std::clamp(start, 0.0, std::max(0.0, duration_ - sample));
  end = std::clamp(end, start + sample, std::max(duration_, start + sample));
  start_ = start;
  end_ = end;
  startSlider_.setValue(start_, juce::dontSendNotification);
  endSlider_.setValue(end_, juce::dontSendNotification);
  update();
}

void ImportDialog::update() {
  const mirage::ImportSettings s = settings();
  const mirage::ImportPlan plan = mirage::planImport(*audio_, s);
  const bool ok = plan.problem.empty();
  // The source time the written bytes cover, for the waveform's cut-off colour.
  const double fitEnd = ok ? start_ + plan.seconds : end_;
  waveform_->setSelection(start_, end_, fitEnd);

  juce::String text;
  if (ok) {
    const int firstPage = s.startPage;
    const uint32_t last = plan.address + uint32_t(plan.bytes) - 1;
    text << "Selected " << seconds(end_ - start_) << ": " << juce::String((juce::int64)plan.bytes)
         << " bytes at " << juce::String(juce::roundToInt(s.rate)) << " Hz, "
         << juce::String(plan.fractionOfHalf * 100.0, 1) << "% of the half.\n"
         << "Writes $" << hex(int(plan.address), 5) << "-$" << hex(int(last), 5) << ": pages "
         << firstPage << " to " << plan.lastPage << ", last page " << plan.lastPage
         << " (in hex as the OS shows them: " << hex(firstPage, 2) << " to "
         << hex(plan.lastPage, 2) << ").\n";
  }
  text << "Plays at original pitch around " << noteName(mirage::originalPitchKey(s.rate))
       << " (with the loaded sound's tuning).";
  info_.setText(text, juce::dontSendNotification);

  juce::String warning;
  if (!ok)
    warning = juce::String::fromUTF8(plan.problem.c_str());
  else if (plan.truncated)
    warning = "Too long for the half: only the first " + seconds(plan.seconds) +
              " fits before the end of the half; the rest is cut off.";
  const bool can = canNow();
  can_ = can;
  if (!can) {
    if (warning.isNotEmpty()) warning << "\n";
    warning << "Import is off while the drive light is on or no audio device runs.";
  }
  warning_.setText(warning, juce::dontSendNotification);
  importButton_.setEnabled(ok && can);
}

bool ImportDialog::canNow() const { return !canImport_ || canImport_(); }

void ImportDialog::timerCallback() {
  if (canNow() != can_) update();
}

void ImportDialog::paint(juce::Graphics& g) {
  g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void ImportDialog::resized() {
  auto r = getLocalBounds().reduced(16, 12);
  fileLabel_.setBounds(r.removeFromTop(24));
  r.removeFromTop(6);
  waveform_->setBounds(r.removeFromTop(180));
  r.removeFromTop(12);

  constexpr int kRow = 26, kLabel = 76, kGap = 8;
  auto row = r.removeFromTop(kRow);
  auto left = row.removeFromLeft(row.getWidth() / 2);
  startLabel_.setBounds(left.removeFromLeft(kLabel));
  startSlider_.setBounds(left.withTrimmedLeft(kGap).withTrimmedRight(kGap));
  endLabel_.setBounds(row.removeFromLeft(kLabel));
  endSlider_.setBounds(row.withTrimmedLeft(kGap));
  r.removeFromTop(kGap);

  row = r.removeFromTop(kRow);
  rateLabel_.setBounds(row.removeFromLeft(kLabel));
  defaultRate_.setBounds(row.removeFromRight(80));
  rateSlider_.setBounds(row.withTrimmedLeft(kGap).withTrimmedRight(kGap));
  r.removeFromTop(kGap);

  row = r.removeFromTop(kRow);
  halfLabel_.setBounds(row.removeFromLeft(kLabel));
  halfBox_.setBounds(row.removeFromLeft(220).withTrimmedLeft(kGap));
  pageLabel_.setBounds(row.removeFromLeft(110));
  pageSlider_.setBounds(row.removeFromLeft(150).withTrimmedLeft(kGap));
  normalise_.setBounds(row.withTrimmedLeft(24));
  r.removeFromTop(14);

  auto buttons = r.removeFromBottom(30);
  importButton_.setBounds(buttons.removeFromRight(100));
  buttons.removeFromRight(kGap);
  cancelButton_.setBounds(buttons.removeFromRight(100));
  r.removeFromBottom(8);
  warning_.setBounds(r.removeFromBottom(40));
  info_.setBounds(r);
}
