#pragma once

#include <cstddef>
#include <string>

// Bundles everything needed to investigate a bug into one .zip in reportDir():
//   report.txt         build, system, previous-run state, what's included
//   logs/*.log         the newest run logs (already redacted when written)
//   settings.json      app settings with the account and secrets removed
//   crash_reports/     Switch: Atmosphère reports for these builds
//                      macOS: the system's .ips crash reports for vkpcnx
// Nothing is sent anywhere; the user shares the file themselves.
namespace vkpcnx::diag {

struct ReportResult {
  bool ok = false;
  std::string path;
  std::string error;
  size_t bytes = 0;
};

// Blocking file IO (a second or two on the Switch's SD card): call off the UI
// thread. `reason` goes into report.txt ("manual", "after crash", ...).
ReportResult createReport(const std::string &reason);

std::string reportDir();

// Desktop: shows the file in Finder / Explorer / the file manager
void revealInFileManager(const std::string &path);

} // namespace vkpcnx::diag
