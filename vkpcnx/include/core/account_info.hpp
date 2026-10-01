#pragma once
#include <borealis.hpp>
#include <nlohmann/json.hpp>
#include <optional>

class AccountInfo {
public:
  static AccountInfo &instance();
  const std::optional<nlohmann::json> &get() const;
  void fetch();
  void clear();

  brls::Event<const nlohmann::json &> &updated() { return onUpdated; }

private:
  std::optional<nlohmann::json> cached;
  bool inFlight = false;
  brls::Event<const nlohmann::json &> onUpdated;
};