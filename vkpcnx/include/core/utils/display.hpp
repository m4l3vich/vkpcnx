#pragma once

#include <utility>

namespace vkpcnx::utils {
// Size of the app's drawing surface in physical pixels (the "monitor" size of
// protocol doc §6.3). brls::Application::windowWidth is already pixels with
// the OpenGL backend, so it must not be multiplied by the scale factor again.
std::pair<int, int> displayPixelSize();
} // namespace vkpcnx::utils
