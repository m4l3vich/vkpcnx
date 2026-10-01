#include "core/account_info.hpp"
#include "core/vkpcapi.hpp"

AccountInfo &AccountInfo::instance() {
  static AccountInfo s;
  return s;
}

const std::optional<nlohmann::json> &AccountInfo::get() const { return cached; }

void AccountInfo::fetch() {
  try {
    auto me = VKPCAPI::usersMe();
    this->cached = me;
    this->onUpdated.fire(me);
  } catch (const VKPCAPI::NotLoggedInException) {
    // noop
  }
}

void AccountInfo::clear() { this->cached = std::nullopt; }