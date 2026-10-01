#include "core/utils/lifecycle.hpp"

#include <borealis.hpp>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <unistd.h>
#endif

namespace vkpcnx::utils {

namespace {
std::string &execPath() {
  static std::string path;
  return path;
}
} // namespace

void setExecPath(const std::string &path) { execPath() = path; }

void relaunch() {
#ifdef __SWITCH__
  envSetNextLoad(execPath().c_str(), execPath().c_str());
  brls::Application::quit();
#else
  char *args[] = {execPath().data(), nullptr};
  brls::Application::quit();
  execv(execPath().c_str(), args);
#endif
}
} // namespace vkpcnx::utils
