#include "core/settings.hpp"

#include <borealis.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace fs = std::filesystem;

static const char* APP_DIR_NAME = "vkpcnx";

Settings& Settings::instance()
{
    static Settings s;
    return s;
}

std::string Settings::configDir()
{
    std::string dir;
#if defined(__SWITCH__)
    dir = std::string("sdmc:/config/") + APP_DIR_NAME;
#elif defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    dir = std::string(base ? base : ".") + "\\" + APP_DIR_NAME;
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    dir = std::string(home ? home : ".") + "/Library/Application Support/" + APP_DIR_NAME;
#else
    const char* xdg  = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    dir = xdg ? std::string(xdg) + "/" + APP_DIR_NAME
              : std::string(home ? home : ".") + "/.config/" + APP_DIR_NAME;
#endif

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        brls::Logger::error("Settings: cannot create {}: {}", dir, ec.message());
    return dir;
}

static bool isPointer(const std::string& key)
{
    return !key.empty() && key[0] == '/';
}

const nlohmann::json* Settings::find(const std::string& key) const
{
    if (isPointer(key))
    {
        nlohmann::json::json_pointer ptr(key);
        return data.contains(ptr) ? &data[ptr] : nullptr;
    }
    auto it = data.find(key);
    return it != data.end() ? &*it : nullptr;
}

nlohmann::json& Settings::ref(const std::string& key)
{
    if (isPointer(key))
        return data[nlohmann::json::json_pointer(key)];
    return data[key];
}

void Settings::remove(const std::string& key)
{
    if (isPointer(key))
    {
        nlohmann::json::json_pointer ptr(key);
        if (!data.contains(ptr))
            return;
        data[ptr.parent_pointer()].erase(ptr.back());
    }
    else
    {
        data.erase(key);
    }
    save();
}

void Settings::wipe()
{
    data = nlohmann::json::object();

    std::string path = configFile();
    std::error_code ec;
    fs::remove(path, ec);
    if (ec)
        brls::Logger::error("Settings: cannot remove {}: {}", path, ec.message());
}

std::string Settings::configFile()
{
    return configDir() + "/settings.json";
}

void Settings::load()
{
    std::string path = configFile();
    std::ifstream in(path);
    if (!in)
    {
        brls::Logger::info("Settings: no file at {}, using defaults", path);
        return;
    }

    try
    {
        data = nlohmann::json::parse(in);
        if (!data.is_object())
            throw std::runtime_error("root is not an object");
        brls::Logger::info("Settings: loaded {}", path);
    }
    catch (const std::exception& e)
    {
        brls::Logger::error("Settings: corrupt file {} ({}), using defaults", path, e.what());
        data = nlohmann::json::object();
    }
}

bool Settings::save()
{
    std::string path = configFile();
    std::string tmp  = path + ".tmp";

    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
        {
            brls::Logger::error("Settings: cannot open {} for writing", tmp);
            return false;
        }
        out << data.dump(2) << '\n';
        if (!out)
        {
            brls::Logger::error("Settings: write to {} failed", tmp);
            return false;
        }
    }

    // std::rename replaces the destination atomically on POSIX and libnx's fs
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
    {
        // Windows rename() refuses to overwrite; fall back to remove + rename
        std::remove(path.c_str());
        if (std::rename(tmp.c_str(), path.c_str()) != 0)
        {
            brls::Logger::error("Settings: cannot move {} -> {}", tmp, path);
            return false;
        }
    }
    return true;
}
