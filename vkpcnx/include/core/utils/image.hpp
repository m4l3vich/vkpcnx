#pragma once
#include <borealis.hpp>
#include <functional>
#include <string>

namespace vkpcnx::utils {
// isValid (if provided) is checked right before the image is applied, so a response that
// arrives after the caller no longer wants it (e.g. a recycled view bound to a new item) can
// be dropped instead of clobbering newer content.
void loadImageFromUrl(brls::Image *image, std::string url, std::function<bool()> isValid = nullptr);
} // namespace vkpcnx::utils
