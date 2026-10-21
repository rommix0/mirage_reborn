#include "PanelComponent.h"

#include <BinaryData.h>

namespace {

using juce::Colour;
using juce::Point;
using juce::Rectangle;

const Colour kSurround{0xff101012};  // around the panel when the window's aspect differs
const Colour kCaseTop{0xff2c2c30}, kCaseBottom{0xff222225};
const Colour kLipTop{0xff1b1b1e}, kLipBottom{0xff111113};
const Colour kWell{0xff0b0b0d};      // the recess around each cap
const Colour kLegend{0xfff2f2f0};    // labels, headings, rules
const Colour kBranding{0xffc6c6c4};  // the ensoniq badge and the subtitle
const Colour kMirageYellow{0xfff2c22e};
const Colour kDigitOn{0xffffd030}, kDigitOff{0xff352b08};  // the yellow two-digit readout
const Colour kLedOn{0xffff3322}, kLedOff{0xff3a0e0b};      // the drive activity LED

constexpr float kLabelFont = 9.5f, kHeadingFont = 8.5f;
constexpr float kRuleThickness = 1.0f;
constexpr float kSlant = 0.07f;  // the 7-segment digits lean right by this much per unit of height

Rectangle<float> toRect(const mirage::PanelRect& r) { return {r.x, r.y, r.w, r.h}; }

juce::Font panelFont(float height, bool bold = false) {
  return juce::Font(
      juce::FontOptions("Arial", height, bold ? juce::Font::bold : juce::Font::plain));
}

juce::String labelOf(const mirage::PanelButton& b) {
  return juce::String::fromUTF8(b.label.data(), int(b.label.size()));
}

Colour capColour(mirage::CapColour c) {
  switch (c) {
    case mirage::CapColour::Yellow: return Colour(0xffe8b42a);
    case mirage::CapColour::Grey: return Colour(0xffb8b7b1);
    case mirage::CapColour::White: return Colour(0xffece6d3);
    case mirage::CapColour::Black: return Colour(0xff1e1e20);
  }
  return Colour(0xffb8b7b1);
}

// `text` as an outline in `font`, scaled to fit `area` (aspect kept, centred).
juce::Path textPath(const juce::String& text, const juce::Font& font, Rectangle<float> area) {
  juce::GlyphArrangement glyphs;
  glyphs.addLineOfText(font, text, 0.0f, 0.0f);
  juce::Path p;
  glyphs.createPath(p);
  p.applyTransform(p.getTransformToScaleToFit(area, true, juce::Justification::centred));
  return p;
}

// The ensoniq badge (box and logotype) from the embedded ensoniq_logo.svg, inked
// in the panel's branding grey.
std::unique_ptr<juce::Drawable> makeEnsoniqLogo() {
  auto logo = juce::Drawable::createFromImageData(MirageBranding::ensoniq_logo_svg,
                                                  size_t(MirageBranding::ensoniq_logo_svgSize));
  jassert(logo != nullptr);
  if (logo) {
    logo->replaceColour(juce::Colours::black, kBranding);   // paths with no fill
    logo->replaceColour(Colour(0xff231f20), kBranding);     // the "e" (class cls-1)
  }
  return logo;
}

// The drive's faceplate on the lip: the slot and its activity LED.
Rectangle<float> driveBezel() {
  const auto& geo = mirage::panelGeometry();
  return toRect(geo.driveSlot).getUnion(toRect(geo.driveLed)).expanded(12.0f, 9.0f);
}

void fillLed(juce::Graphics& g, const juce::Path& p, bool lit, float glow) {
  if (lit) {
    g.setColour(kDigitOn.withAlpha(0.3f));
    g.strokePath(p, juce::PathStrokeType(glow));
  }
  g.setColour(lit ? kDigitOn : kDigitOff);
  g.fillPath(p);
}

// One 7-segment digit in `box`, of segment thickness kSegment: bit n of `bits`
// lights segment a..g, and bit 7 the decimal point, centred kDotOffset left of
// the box's lower left corner.
constexpr float kSegment = 4.4f, kDotRadius = 2.4f, kDotOffset = 4.0f;
void paintDigit(juce::Graphics& g, Rectangle<float> box, uint8_t bits) {
  const float w = box.getWidth(), h = box.getHeight();
  const float t = kSegment, gap = t * 0.16f;
  const auto lean = [&](Point<float> p) {
    return Point<float>(box.getX() + p.x + kSlant * (h - p.y), box.getY() + p.y);
  };
  const float l = t / 2, r = w - t / 2, top = t / 2, mid = h / 2, bot = h - t / 2;
  const Point<float> ends[7][2] = {
      {{l, top}, {r, top}},  // a
      {{r, top}, {r, mid}},  // b
      {{r, mid}, {r, bot}},  // c
      {{l, bot}, {r, bot}},  // d
      {{l, mid}, {l, bot}},  // e
      {{l, top}, {l, mid}},  // f
      {{l, mid}, {r, mid}},  // g
  };
  for (int i = 0; i < 7; ++i) {
    // A hexagon with pointed ends, so neighbouring segments mitre.
    const Point<float> a = ends[i][0], b = ends[i][1];
    const Point<float> dir = (b - a) / a.getDistanceFrom(b);
    const Point<float> n(-dir.y * t / 2, dir.x * t / 2);
    const Point<float> a1 = a + dir * gap, b1 = b - dir * gap;
    const Point<float> half = dir * (t / 2);
    juce::Path seg;
    seg.startNewSubPath(lean(a1));
    seg.lineTo(lean(a1 + half + n));
    seg.lineTo(lean(b1 - half + n));
    seg.lineTo(lean(b1));
    seg.lineTo(lean(b1 - half - n));
    seg.lineTo(lean(a1 + half - n));
    seg.closeSubPath();
    fillLed(g, seg, (bits >> i) & 1, t * 0.7f);
  }
  juce::Path dot;
  dot.addEllipse(Rectangle<float>(2 * kDotRadius, 2 * kDotRadius)
                     .withCentre({box.getX() - kDotOffset, box.getBottom() - kDotRadius}));
  fillLed(g, dot, (bits >> 7) & 1, t * 0.7f);
}

}  // namespace

// ---- Button ----------------------------------------------------------------

// One calculator-style cap. Drawn in panel units: the parent gives it the
// panel-to-pixels transform.
class PanelComponent::Button : public juce::Component {
 public:
  Button(PanelComponent& panel, const mirage::PanelButton& b)
      : panel_(panel),
        key_(b.key),
        cap_(capColour(b.cap)),
        black_(b.cap == mirage::CapColour::Black) {
    setTitle(labelOf(b));
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
  }

  void paint(juce::Graphics& g) override {
    const bool down = panel_.isHeld(key_);
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    if (down) r.translate(0.0f, 0.7f);
    // The cap's lower edge, then its top face.
    g.setColour(cap_.darker(black_ ? 0.4f : 0.55f));
    g.fillRoundedRectangle(r, 2.2f);
    const auto face = r.withTrimmedBottom(down ? 1.2f : 2.2f);
    const Colour base = down ? cap_.darker(0.18f) : cap_;
    g.setGradientFill(juce::ColourGradient(base.brighter(black_ ? 0.35f : 0.1f), 0.0f, face.getY(),
                                           base.darker(0.06f), 0.0f, face.getBottom(), false));
    g.fillRoundedRectangle(face, 2.0f);
    g.setColour(juce::Colours::white.withAlpha(black_ ? 0.16f : 0.35f));
    g.fillRect(face.getX() + 2.0f, face.getY() + 0.4f, face.getWidth() - 4.0f, 0.6f);
  }

  void mouseDown(const juce::MouseEvent& e) override {
    if (e.mods.isLeftButtonDown()) setHeld(true);  // right and middle clicks don't press
  }
  void mouseDrag(const juce::MouseEvent& e) override {
    if (!contains(e.getPosition())) setHeld(false);
  }
  void mouseUp(const juce::MouseEvent&) override { setHeld(false); }
  void mouseExit(const juce::MouseEvent&) override { setHeld(false); }

 private:
  void setHeld(bool held) {
    if (held == held_) return;
    held_ = held;
    panel_.press(key_, held);
  }

  PanelComponent& panel_;
  mirage::PanelKey key_;
  Colour cap_;
  bool black_;
  bool held_ = false;  // held by the mouse
};

// ---- Volume slider look ----------------------------------------------------

// A fader: tick marks at the left, a dark slot and a grey cap with a centre line.
class PanelComponent::SliderLook : public juce::LookAndFeel_V4 {
 public:
  static constexpr int kThumbHalfHeight = 6;

  int getSliderThumbRadius(juce::Slider&) override { return kThumbHalfHeight; }

  void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                        float, float, juce::Slider::SliderStyle, juce::Slider&) override {
    const float cx = x + width / 2.0f, top = float(y), bottom = float(y + height);
    g.setColour(kLegend.withAlpha(0.7f));
    for (int i = 0; i <= 10; ++i) {
      const float ty = juce::jmap(float(i), 0.0f, 10.0f, top, bottom);
      const float len = (i % 5 == 0) ? 6.0f : 4.0f;
      g.fillRect(float(x) + 6.0f - len, ty - 0.4f, len, 0.8f);
    }
    const Rectangle<float> slot(cx - 1.8f, top - 4.0f, 3.6f, bottom - top + 8.0f);
    g.setColour(Colour(0xff050506));
    g.fillRoundedRectangle(slot, 1.6f);
    g.setColour(Colour(0xff3a3a3f));
    g.fillRect(slot.getRight(), slot.getY() + 1.0f, 0.6f, slot.getHeight() - 2.0f);

    const Rectangle<float> cap(cx - 8.0f, sliderPos - kThumbHalfHeight, 16.0f,
                               2.0f * kThumbHalfHeight);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillRoundedRectangle(cap.translated(0.0f, 1.2f), 1.5f);
    g.setGradientFill(juce::ColourGradient(Colour(0xffd2d2ce), 0.0f, cap.getY(), Colour(0xff8a8a86),
                                           0.0f, cap.getBottom(), false));
    g.fillRoundedRectangle(cap, 1.5f);
    g.setColour(Colour(0xff1a1a1a));
    g.fillRect(cap.getX() + 1.0f, sliderPos - 0.5f, cap.getWidth() - 2.0f, 1.0f);
  }
};

// ---- PanelComponent --------------------------------------------------------

PanelComponent::PanelComponent(mirage::RealtimeHost* host, const mirage::HostStatus& status)
    : host_(host),
      status_(status),
      ensoniqLogo_(makeEnsoniqLogo()),
      sliderLook_(std::make_unique<SliderLook>()) {
  setOpaque(true);
  for (const mirage::PanelButton& b : mirage::panelButtons()) {
    buttons_.push_back(std::make_unique<Button>(*this, b));
    addAndMakeVisible(*buttons_.back());
  }
  volume_.setSliderStyle(juce::Slider::LinearVertical);
  volume_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  volume_.setRange(0.0, 1.0);
  volume_.setValue(1.0, juce::dontSendNotification);
  volume_.setLookAndFeel(sliderLook_.get());
  volume_.setWantsKeyboardFocus(false);  // the arrow keys belong to Off/On
  volume_.setMouseClickGrabsKeyboardFocus(false);
  volume_.setTitle("Volume");
  addAndMakeVisible(volume_);
  const auto& geo = mirage::panelGeometry();
  setSize(juce::roundToInt(geo.width), juce::roundToInt(geo.height));
}

PanelComponent::PanelComponent(mirage::RealtimeHost& host) : PanelComponent(&host, host.status()) {
  startTimerHz(30);
}

PanelComponent::PanelComponent(const mirage::HostStatus& status, const juce::String& diskName)
    : PanelComponent(nullptr, status) {
  diskName_ = diskName;
}

PanelComponent::~PanelComponent() { volume_.setLookAndFeel(nullptr); }

void PanelComponent::press(mirage::PanelKey key, bool down) {
  auto& holds = holds_[size_t(key)];
  if (!down && holds == 0) return;
  holds = uint8_t(down ? holds + 1 : holds - 1);
  if (holds == (down ? 1 : 0) && host_ != nullptr) host_->postKey(key, down);
  buttons_[size_t(key)]->repaint();
}

void PanelComponent::setDiskName(const juce::String& name) {
  if (name == diskName_) return;
  diskName_ = name;
  repaintPanelArea(driveBezel());
}

void PanelComponent::timerCallback() {
  const auto s = host_->status();
  if (s.display.left != status_.display.left || s.display.right != status_.display.right)
    repaintPanelArea(toRect(mirage::panelGeometry().display));
  if (s.driveActive != status_.driveActive || s.diskPresent != status_.diskPresent)
    repaintPanelArea(driveBezel());
  status_ = s;
}

juce::AffineTransform PanelComponent::panelToLocal() const {
  const auto& geo = mirage::panelGeometry();
  const float s = juce::jmin(getWidth() / geo.width, getHeight() / geo.height);
  return juce::AffineTransform::scale(s).translated((getWidth() - geo.width * s) / 2,
                                                    (getHeight() - geo.height * s) / 2);
}

void PanelComponent::repaintPanelArea(Rectangle<float> panelArea) {
  repaint(panelArea.transformedBy(panelToLocal()).getSmallestIntegerContainer().expanded(1));
}

void PanelComponent::resized() {
  const auto t = panelToLocal();
  for (const mirage::PanelButton& b : mirage::panelButtons()) {
    auto& button = *buttons_[size_t(b.key)];
    button.setBounds(toRect(b.r).toNearestInt());
    button.setTransform(t);
  }
  volume_.setBounds(toRect(mirage::panelGeometry().volume).toNearestInt());
  volume_.setTransform(t);
}

void PanelComponent::paint(juce::Graphics& g) {
  g.fillAll(kSurround);
  g.addTransform(panelToLocal());
  paintCase(g);
  paintLegends(g);
  paintBranding(g);
  paintDisplay(g);
  paintDrive(g);
}

void PanelComponent::paintCase(juce::Graphics& g) const {
  const auto& geo = mirage::panelGeometry();
  const float w = geo.width, lip = geo.lipY;
  g.setGradientFill(juce::ColourGradient(kCaseTop, 0.0f, 0.0f, kCaseBottom, 0.0f, lip, false));
  g.fillRect(0.0f, 0.0f, w, lip);
  g.setColour(Colour(0xff3d3d42));
  g.fillRect(0.0f, 0.0f, w, 1.2f);
  // The front lip, below a crease.
  g.setGradientFill(juce::ColourGradient(kLipTop, 0.0f, lip, kLipBottom, 0.0f, geo.height, false));
  g.fillRect(0.0f, lip, w, geo.height - lip);
  g.setColour(Colour(0xff08080a));
  g.fillRect(0.0f, lip - 1.0f, w, 2.0f);
  g.setColour(Colour(0xff35353a));
  g.fillRect(0.0f, lip + 1.0f, w, 0.8f);
}

// The recesses under the caps, the labels above them, the rules and the
// section headings.
void PanelComponent::paintLegends(juce::Graphics& g) const {
  using mirage::PanelKey;
  const auto& geo = mirage::panelGeometry();
  const auto buttons = mirage::panelButtons();
  const auto rectOf = [&](PanelKey k) { return toRect(buttons[size_t(k)].r); };

  g.setFont(panelFont(kLabelFont));
  for (const mirage::PanelButton& b : buttons) {
    const auto r = toRect(b.r);
    g.setColour(kWell);
    g.fillRoundedRectangle(r.expanded(1.4f), 3.0f);
    g.setColour(Colour(0xff3a3a3f));
    g.fillRect(r.getX(), r.getBottom() + 1.4f, r.getWidth(), 0.6f);
    g.setColour(kLegend);
    g.drawText(labelOf(b),
               Rectangle<float>(r.getX() - 12.0f, r.getY() - 14.0f, r.getWidth() + 24.0f, 11.0f),
               juce::Justification::centred, false);
  }

  g.setColour(kLegend);
  // Rules under rows 1 and 2 from key 1 to the LOAD column; the pinstripe above
  // row 4 from the ensoniq badge across the panel, broken by the volume slider.
  const float ruleLeft = rectOf(PanelKey::K1).getX(),
              ruleRight = rectOf(PanelKey::LoadUpper).getRight();
  for (float y : geo.rowRuleY)
    g.fillRect(ruleLeft, y - kRuleThickness / 2, ruleRight - ruleLeft, kRuleThickness);
  const float pinLeft = geo.ensoniqBadge.x + geo.ensoniqBadge.w + 6.0f,
              pinRight = geo.width - 24.0f;
  const float sliderLeft = geo.volume.x - 2.0f, sliderRight = geo.volume.x + geo.volume.w + 2.0f;
  g.fillRect(pinLeft, geo.pinstripeY - 0.45f, sliderLeft - pinLeft, 0.9f);
  g.fillRect(sliderRight, geo.pinstripeY - 0.45f, pinRight - sliderRight, 0.9f);

  // The bottom rule from VOLUME to LOAD; each heading under a short tab bar.
  const float bottomLeft = geo.volume.x;
  g.fillRect(bottomLeft, geo.headingRuleY - kRuleThickness / 2, ruleRight - bottomLeft,
             kRuleThickness);
  const auto headingFont = panelFont(kHeadingFont, true).withExtraKerningFactor(0.08f);
  g.setFont(headingFont);
  for (const mirage::PanelHeading& h : mirage::panelHeadings()) {
    const auto text = juce::String(h.text.data(), h.text.size());
    const float tw = juce::GlyphArrangement::getStringWidth(headingFont, text);
    g.fillRect(h.centreX - tw / 2 - 3.0f, geo.headingRuleY, tw + 6.0f, 2.2f);
    g.drawText(text, Rectangle<float>(h.centreX - 50.0f, geo.headingY - 5.0f, 100.0f, 11.0f),
               juce::Justification::centred, false);
  }
}

void PanelComponent::paintBranding(juce::Graphics& g) const {
  const auto& geo = mirage::panelGeometry();
  // ensoniq: the logotype in its box.
  if (ensoniqLogo_)
    ensoniqLogo_->drawWithin(g, toRect(geo.ensoniqBadge), juce::RectanglePlacement::centred, 1.0f);
  // Mirage in solid yellow, its subtitle in small widely spaced caps.
  g.setColour(kMirageYellow);
  g.fillPath(textPath("Mirage",
                      juce::Font(juce::FontOptions("Arial Black", 100.0f, juce::Font::plain)),
                      toRect(geo.mirageLogo)));
  g.setColour(kBranding);
  g.setFont(panelFont(9.0f).withExtraKerningFactor(0.32f));
  g.drawText("DIGITAL SAMPLING KEYBOARD", toRect(geo.mirageSubtitle), juce::Justification::centred,
             false);
}

// The two-digit LED behind its dark window, and the inverted-T and L brackets
// from the digits' dots (the Param and Value indicators) down to those labels.
void PanelComponent::paintDisplay(juce::Graphics& g) const {
  using mirage::PanelKey;
  const auto& geo = mirage::panelGeometry();
  const auto buttons = mirage::panelButtons();
  const auto param = toRect(buttons[size_t(PanelKey::Param)].r);
  const auto value = toRect(buttons[size_t(PanelKey::Value)].r);
  const auto d = toRect(geo.display);

  g.setColour(Colour(0xff09090a));
  g.fillRoundedRectangle(d, 3.0f);
  const auto window = d.reduced(3.0f);
  g.setGradientFill(juce::ColourGradient(Colour(0xff1a1606), 0.0f, window.getY(),
                                         Colour(0xff0b0a03), 0.0f, window.getBottom(), false));
  g.fillRoundedRectangle(window, 2.0f);

  // Each dot sits at its digit's lower left; the left dot is over Param.
  const float digitW = 22.0f, digitH = 40.0f, pitch = 39.0f;
  const float dotX[2] = {param.getCentreX(), param.getCentreX() + pitch};
  const uint8_t bits[2] = {status_.display.left, status_.display.right};
  const float digitY = d.getCentreY() - digitH / 2;
  for (int i = 0; i < 2; ++i)
    paintDigit(g, Rectangle<float>(dotX[i] + kDotOffset, digitY, digitW, digitH), bits[i]);

  g.setColour(juce::Colours::white.withAlpha(0.05f));  // glass sheen
  g.fillRoundedRectangle(window.withHeight(window.getHeight() * 0.42f), 2.0f);

  const float stemTop = d.getBottom() + 2.5f, barY = param.getY() - 16.0f, halfBar = 13.0f;
  const float th = 0.9f;
  g.setColour(kLegend);
  g.fillRect(dotX[0] - th / 2, stemTop, th, barY - stemTop);
  g.fillRect(param.getCentreX() - halfBar, barY - th / 2, 2 * halfBar, th);
  g.fillRect(dotX[1] - th / 2, stemTop, th, barY - stemTop + th / 2);
  g.fillRect(dotX[1] - th / 2, barY - th / 2, value.getCentreX() + halfBar - dotX[1], th);
}

// The drive faceplate on the lip: the slot (showing the disk's file name while
// a disk is in) and the activity LED.
void PanelComponent::paintDrive(juce::Graphics& g) const {
  const auto& geo = mirage::panelGeometry();
  const auto bezel = driveBezel();
  g.setColour(Colour(0xff17171a));
  g.fillRoundedRectangle(bezel, 3.0f);
  g.setColour(Colour(0xff2e2e33));
  g.drawRoundedRectangle(bezel, 3.0f, 0.8f);

  const auto slot = toRect(geo.driveSlot);
  g.setColour(Colour(0xff040404));
  g.fillRoundedRectangle(slot, 2.0f);
  if (status_.diskPresent) {
    const auto disk = slot.reduced(5.0f, 4.0f);
    g.setColour(Colour(0xff2b2b30));
    g.fillRoundedRectangle(disk, 1.0f);
    g.setColour(Colour(0xffd4d4d0));
    g.setFont(panelFont(9.5f));
    g.drawText(diskName_, disk.reduced(4.0f, 0.0f), juce::Justification::centred, true);
  }
  g.setColour(Colour(0xff2a2a2e));
  g.fillRect(slot.getX() + 2.0f, slot.getBottom() + 0.5f, slot.getWidth() - 4.0f, 0.6f);

  const auto led = toRect(geo.driveLed);
  const auto c = led.getCentre();
  const float r = led.getWidth() / 2;
  if (status_.driveActive) {
    g.setGradientFill(juce::ColourGradient(kLedOn.withAlpha(0.45f), c, kLedOn.withAlpha(0.0f),
                                           c.translated(r * 2.8f, 0.0f), true));
    g.fillEllipse(Rectangle<float>(r * 5.6f, r * 5.6f).withCentre(c));
    g.setGradientFill(juce::ColourGradient(Colour(0xffff9a88), c.translated(-r * 0.3f, -r * 0.3f),
                                           kLedOn, c.translated(r, 0.0f), true));
  } else {
    g.setColour(kLedOff);
  }
  g.fillEllipse(led);
  g.setColour(juce::Colours::black.withAlpha(0.6f));
  g.drawEllipse(led, 0.6f);
}
