#pragma once
#include <optional>
#include <string>

namespace vkpcnx::utils {
std::optional<std::string>
Iso8601ToLocal(const std::string &iso, const char *format = "%Y-%m-%d %H:%M:%S");
}