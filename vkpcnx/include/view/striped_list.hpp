#pragma once
#include <borealis.hpp>
#include <string>

namespace vkpcnx {

/// One "title .......... value" line of a StripedList.
///
/// Rows never pick their own background: StripedList paints them as it lays
/// them out, so inserting or removing a row re-zebras everything below it.
class StripedListRow : public brls::Box {
public:
  StripedListRow();
  StripedListRow(const std::string &title, const std::string &value);

  void setTitle(const std::string &title);
  void setValue(const std::string &value);

  void setTitleColor(NVGcolor color);
  void setValueColor(NVGcolor color);

  /// Restores the value colour picked up from the theme at construction time.
  void resetValueColor();

  static brls::View *create();

private:
  brls::Label *titleLabel;
  brls::Label *valueLabel;

  NVGcolor defaultValueColor;
};

/// The Switch settings-style striped table: flat full-bleed rows, no
/// separators, every other one tinted slightly against the page background.
class StripedList : public brls::Box {
public:
  StripedList();

  /// Appends a row and returns it, so the caller can recolour or rebind later.
  StripedListRow *addRow(const std::string &title, const std::string &value = "");

  void addView(brls::View *view) override;
  void addView(brls::View *view, size_t position) override;
  void removeView(brls::View *view, bool free = true) override;
  void clearViews(bool free = true) override;

  void setStripeColor(NVGcolor color);
  void setStripeColorEven(NVGcolor color);

  /// Whether the first row is the tinted one. True matches the Switch, where
  /// the table opens on a tinted row.
  void setStartsTinted(bool startsTinted);

  static brls::View *create();

  /// Registers the vkpcnx/striped_list/* theme colours. Called automatically by
  /// the first StripedList/StripedListRow, safe to call more than once.
  static void registerThemeColors();

private:
  void restripe();

  NVGcolor stripeColor;
  NVGcolor stripeColorEven;
  bool startsTinted = true;
};

}; // namespace vkpcnx
