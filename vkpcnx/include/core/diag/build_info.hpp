#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// What a crash address or a bug report has to be matched against: the exact
// build (version, git, build ID) and where its image is mapped at runtime.
namespace vkpcnx::diag {

struct BuildInfo {
  std::string version;   // "1.0.0" (CMake VERSION_*)
  std::string git;       // commit, "-dirty" with local changes
  std::string buildType; // CMAKE_BUILD_TYPE
  std::string platform;  // "switch-aarch64", "macos-arm64", "linux-x86_64", "windows-x86_64"
  std::string compiler;
  // GNU build-id (Switch: the NRO's, which Atmosphère prints as Module Id;
  // Linux/Windows: --build-id note/RSDS record) or the Mach-O LC_UUID. Hex,
  // empty when unavailable.
  std::string buildId;
  uintptr_t moduleBase = 0; // runtime address of the main image
  // Address the image was linked at, so a symbolizer can rebase:
  // file address = pc - moduleBase + linkBase.
  uintptr_t linkBase = 0;
};

const BuildInfo &buildInfo();

// "1.0.0 (40777ef52d4a, Release, switch-aarch64)"
std::string buildSummary();

// OS / firmware / hardware facts for the log header and report.txt, in
// display order. Collected once; extra entries (GPU, display) are added at
// runtime with setSystemInfo().
std::vector<std::pair<std::string, std::string>> systemInfo();
void setSystemInfo(const std::string &key, const std::string &value);

} // namespace vkpcnx::diag
