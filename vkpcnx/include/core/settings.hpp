#pragma once

#include "borealis/core/logger.hpp"
#include <nlohmann/json.hpp>
#include <string>

// Persistent key-value preferences, backed by a JSON file in the
// platform config directory. Access via Settings::instance().
class Settings {
public:
  static Settings &instance();

  // Reads the settings file; missing or corrupt files fall back to defaults.
  void load();
  // Writes atomically (tmp file + rename) so a crash mid-write can't corrupt it.
  bool save();

  // Keys are top-level names ("token") or JSON Pointers ("/stream/bitrate")
  // when they start with '/'. Pointer set() creates intermediate objects.

  template <typename T> T get(const std::string &key, const T &def) const {
    const nlohmann::json *v = find(key);
    if (!v || v->is_null())
      return def;
    try {
      return v->get<T>();
    } catch (const nlohmann::json::exception &) {
      return def;
    }
  }

  template <typename T> void set(const std::string &key, const T &value) {
    nlohmann::json &slot = ref(key);
    slot = value;
    if (key.rfind("account", 0) < 2) {
      brls::Logger::debug("account details updated");
    } else {
      brls::Logger::debug("cfg updated, key={}, value={}", key, slot.dump());
    }
    save();
  }

  bool has(const std::string &key) const { return find(key) != nullptr; }
  void remove(const std::string &key);
  // Deletes the settings file and clears everything held in memory.
  void wipe();

  // Raw access for nested structures
  nlohmann::json &json() { return data; }

  // Platform config directory (created on first use)
  static std::string configDir();
  static std::string configFile();

private:
  Settings() = default;
  const nlohmann::json *find(const std::string &key) const;
  nlohmann::json &ref(const std::string &key);
  nlohmann::json data = nlohmann::json::object();
};
