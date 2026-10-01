#pragma once

// UI around diag::createReport(): a confirmation dialog, the build running off
// the UI thread, and a result dialog with the file's location.
namespace vkpcnx {

// "Create a debug report?" → build → "Saved to …"
void openDebugReportDialog();

// If the previous run crashed or didn't exit cleanly, offers a report once.
// Call after the first activity is pushed.
void offerReportAfterUncleanExit();

} // namespace vkpcnx
