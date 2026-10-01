#include "core/controls.hpp"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>

#include "core/settings.hpp"

namespace vkpcnx::controls {

static constexpr float kSnap = 0.05f;

static float normalize(const Sensitivity &s, float value) {
  if (!std::isfinite(value))
    return 1.0f;
  value = std::round(value / kSnap) * kSnap;
  return std::clamp(value, s.min, s.max);
}

float get(const Sensitivity &s) { return normalize(s, Settings::instance().get<float>(s.key, 1.0f)); }

void set(const Sensitivity &s, float value, bool persist) {
  value = normalize(s, value);
  if (persist) {
    Settings::instance().set(s.key, value);
    return;
  }
  Settings::instance().json()[nlohmann::json::json_pointer(s.key)] = value;
}

void reset(const Sensitivity &s, bool persist) { set(s, 1.0f, persist); }

float toProgress(const Sensitivity &s, float value) {
  value = std::clamp(value, s.min, s.max);
  return std::log(value / s.min) / std::log(s.max / s.min);
}

float fromProgress(const Sensitivity &s, float progress) {
  progress = std::clamp(progress, 0.0f, 1.0f);
  return normalize(s, s.min * std::pow(s.max / s.min, progress));
}

std::string format(float value) { return fmt::format("{:.2f}×", value); }

} // namespace vkpcnx::controls
