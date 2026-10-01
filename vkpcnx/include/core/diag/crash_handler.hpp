#pragma once

// Writes a "*** CRASH" section (cause, build line, backtrace) into the log
// file for uncaught C++ exceptions and, on desktop, fatal signals / Windows
// SEH exceptions. The Switch's hard faults are handled by
// core/platform/switch_exception.cpp, which uses the same writers.
//
// Backtrace frames are printed as "module+0xoffset" so they can be fed to
// addr2line / atos against the build's archived symbols
// (scripts/triage-report.py does this).
namespace vkpcnx::diag {

void installCrashHandlers();

// The "build:" line of a crash section; preformatted, signal-safe
void writeCrashBuildLine();
// Frames of the calling thread
void writeBacktrace();

} // namespace vkpcnx::diag
