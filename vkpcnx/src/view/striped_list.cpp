#include "view/striped_list.hpp"

namespace vkpcnx {

namespace {

// Sampled off a 1280x720 Switch screenshot of the connection test screen: the
// page sits at 235,235,235 and the tinted rows at 240,240,240. The dark values
// are the same idea with a slightly wider gap, since +5 is invisible on 45,45,45.
const NVGcolor lightStripe = nvgRGB(240, 240, 240);
const NVGcolor lightStripeEven = nvgRGBA(0, 0, 0, 0);
const NVGcolor darkStripe = nvgRGB(54, 54, 54);
const NVGcolor darkStripeEven = nvgRGBA(0, 0, 0, 0);

// Row metrics from the same screenshot.
constexpr float rowHeight = 38.0f;
constexpr float rowIndent = 10.0f;
constexpr float rowFontSize = 19.0f;

bool themeColorsRegistered = false;

} // namespace

void StripedList::registerThemeColors() {
  if (themeColorsRegistered)
    return;
  themeColorsRegistered = true;

  brls::Theme::getLightTheme().addColor("vkpcnx/striped_list/stripe", lightStripe);
  brls::Theme::getLightTheme().addColor("vkpcnx/striped_list/stripe_even", lightStripeEven);
  brls::Theme::getDarkTheme().addColor("vkpcnx/striped_list/stripe", darkStripe);
  brls::Theme::getDarkTheme().addColor("vkpcnx/striped_list/stripe_even", darkStripeEven);
}

// MARK: - StripedListRow

StripedListRow::StripedListRow() {
  StripedList::registerThemeColors();

  brls::Theme theme = brls::Application::getTheme();
  this->defaultValueColor = theme["brls/text"];

  this->setAxis(brls::Axis::ROW);
  this->setAlignItems(brls::AlignItems::CENTER);
  this->setHeight(rowHeight);
  this->setPaddingLeft(rowIndent);
  this->setPaddingRight(rowIndent);

  // The title keeps its natural width; a long value yields to it and gets
  // ellipsised. Both must be single-line: a wrapping label with nothing to
  // break on (a hostname, say) just overflows its box instead of truncating.
  //
  // The value stays left-aligned on purpose. brls::Label::onLayout measures
  // glyph positions with the label's own alignment, so a right-aligned label
  // never finds a glyph past its width and never truncates. The box is
  // content-sized, so a value that fits still sits flush right.
  this->titleLabel = new brls::Label();
  this->titleLabel->setFontSize(rowFontSize);
  this->titleLabel->setGrow(1.0f);
  this->titleLabel->setShrink(0.0f);
  this->titleLabel->setSingleLine(true);
  this->titleLabel->setMarginRight(rowIndent);

  this->valueLabel = new brls::Label();
  this->valueLabel->setFontSize(rowFontSize);
  this->valueLabel->setShrink(1.0f);
  this->valueLabel->setSingleLine(true);
  this->valueLabel->setTextColor(this->defaultValueColor);

  brls::Box::addView(this->titleLabel);
  brls::Box::addView(this->valueLabel);

  this->registerStringXMLAttribute("title", [this](std::string value) { this->setTitle(value); });
  this->registerStringXMLAttribute("value", [this](std::string value) { this->setValue(value); });
  this->registerColorXMLAttribute("titleColor", [this](NVGcolor color) {
    this->setTitleColor(color);
  });
  this->registerColorXMLAttribute("valueColor", [this](NVGcolor color) {
    this->setValueColor(color);
  });
}

StripedListRow::StripedListRow(const std::string &title, const std::string &value)
    : StripedListRow() {
  this->setTitle(title);
  this->setValue(value);
}

// Label::setText invalidates unconditionally, and a relayout restarts the
// label's marquee timer, so a caller refreshing values every tick (the stream
// overlay does) would never see a long value scroll. Only touch the label
// when the text actually changed.
static void setTextIfChanged(brls::Label *label, const std::string &text) {
  if (label->getFullText() != text)
    label->setText(text);
}

void StripedListRow::setTitle(const std::string &title) {
  setTextIfChanged(this->titleLabel, title);
}

void StripedListRow::setValue(const std::string &value) {
  setTextIfChanged(this->valueLabel, value);
}

void StripedListRow::setTitleColor(NVGcolor color) { this->titleLabel->setTextColor(color); }

void StripedListRow::setValueColor(NVGcolor color) { this->valueLabel->setTextColor(color); }

void StripedListRow::resetValueColor() { this->valueLabel->setTextColor(this->defaultValueColor); }

brls::View *StripedListRow::create() { return new StripedListRow(); }

// MARK: - StripedList

StripedList::StripedList() {
  StripedList::registerThemeColors();

  brls::Theme theme = brls::Application::getTheme();
  this->stripeColor = theme["vkpcnx/striped_list/stripe"];
  this->stripeColorEven = theme["vkpcnx/striped_list/stripe_even"];

  this->setAxis(brls::Axis::COLUMN);

  this->registerColorXMLAttribute("stripeColor", [this](NVGcolor color) {
    this->setStripeColor(color);
  });
  this->registerColorXMLAttribute("stripeColorEven", [this](NVGcolor color) {
    this->setStripeColorEven(color);
  });
  this->registerBoolXMLAttribute("startsTinted", [this](bool value) {
    this->setStartsTinted(value);
  });
}

StripedListRow *StripedList::addRow(const std::string &title, const std::string &value) {
  auto *row = new StripedListRow(title, value);
  this->addView(row);
  return row;
}

void StripedList::addView(brls::View *view) {
  brls::Box::addView(view);
  this->restripe();
}

void StripedList::addView(brls::View *view, size_t position) {
  brls::Box::addView(view, position);
  this->restripe();
}

void StripedList::removeView(brls::View *view, bool free) {
  brls::Box::removeView(view, free);
  this->restripe();
}

void StripedList::clearViews(bool free) { brls::Box::clearViews(free); }

void StripedList::setStripeColor(NVGcolor color) {
  this->stripeColor = color;
  this->restripe();
}

void StripedList::setStripeColorEven(NVGcolor color) {
  this->stripeColorEven = color;
  this->restripe();
}

void StripedList::setStartsTinted(bool startsTinted) {
  this->startsTinted = startsTinted;
  this->restripe();
}

void StripedList::restripe() {
  size_t index = 0;
  for (brls::View *child : this->getChildren()) {
    bool tinted = (index % 2 == 0) == this->startsTinted;
    child->setBackgroundColor(tinted ? this->stripeColor : this->stripeColorEven);
    index++;
  }
}

brls::View *StripedList::create() { return new StripedList(); }

} // namespace vkpcnx
