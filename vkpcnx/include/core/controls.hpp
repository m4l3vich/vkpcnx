#pragma once

#include <string>

// Pointer sensitivity multipliers, persisted in Settings under
// /controls/sensitivity/*. 1.0 is the built-in speed; values are clamped to
// [min, max]. UI sliders work on progress 0..1, mapped logarithmically so
// that 1.0× sits in the middle and halving/doubling take the same travel.
namespace vkpcnx::controls {

struct Sensitivity {
  const char *key;
  float min, max;
};

// Gamepad pointer: right-stick cursor speed (acceleration scales with it)
inline constexpr Sensitivity kGamepadCursor{"/controls/sensitivity/gamepad_cursor", 0.25f, 4.0f};
// Gamepad pointer: left-stick wheel speed
inline constexpr Sensitivity kGamepadScroll{"/controls/sensitivity/gamepad_scroll", 0.25f, 4.0f};
// Touchscreen in trackpad mode: finger → cursor gain (acceleration scales
// with it); two-finger scrolling isn't affected
inline constexpr Sensitivity kTrackpad{"/controls/sensitivity/trackpad", 0.25f, 4.0f};

// Current value; 1.0 when unset or not a number
float get(const Sensitivity &s);
// Stores the value (clamped, snapped to 0.05). With persist = false only the
// in-memory settings change -- use that while a slider is being dragged and
// call Settings::instance().save() once at the end.
void set(const Sensitivity &s, float value, bool persist = true);
void reset(const Sensitivity &s, bool persist = true); // back to 1.0

// Slider mapping
float toProgress(const Sensitivity &s, float value);
float fromProgress(const Sensitivity &s, float progress); // clamped + snapped

// "1.25×"
std::string format(float value);

} // namespace vkpcnx::controls
