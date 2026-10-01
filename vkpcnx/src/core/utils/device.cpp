#include "core/utils/device.hpp"

#include "core/settings.hpp"
#include <iomanip>
#include <random>
#include <sstream>

namespace vkpcnx::utils {

std::string getDeviceUUID() {
  if (Settings::instance().has("device_id")) {
    return Settings::instance().get<std::string>("device_id", "");
  }

  static std::random_device rd;
  static std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dist;

  uint64_t hi = dist(gen);
  uint64_t lo = dist(gen);

  hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL; // version 4
  lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL; // variant 10

  std::ostringstream ss;
  ss << std::hex << std::setfill('0') << std::setw(8) << (uint32_t)(hi >> 32) << "-"
     << std::setw(4) << (uint16_t)(hi >> 16) << "-" << std::setw(4) << (uint16_t)hi << "-"
     << std::setw(4) << (uint16_t)(lo >> 48) << "-" << std::setw(12) << (lo & 0xFFFFFFFFFFFFULL);

  Settings::instance().set("device_id", ss.str());
  return ss.str();
}

} // namespace vkpcnx::utils
