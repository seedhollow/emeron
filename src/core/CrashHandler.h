#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace em::crash {

// Installs process-wide handlers that write a crash report to
// <workspaceDir>/crash-logs/ before the process dies the way it normally
// would (so a debugger or the OS's own crash dialog still sees it).
//
// Call once, as early in startup as the workspace directory is known --
// before window/graphics init, since a bad driver is one of the likelier
// ways to end up here.
//
//   POSIX:   SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS.
//   Windows: SetUnhandledExceptionFilter.
void install(const std::filesystem::path& workspaceDir);

// Where reports are written: <workspaceDir>/crash-logs.
[[nodiscard]] std::filesystem::path logDirectory();

// A short note on what emeron was doing, included in the next crash report.
// Safe to call often from the UI thread: it only updates an in-memory buffer
// and, at most, writes one small file -- never from a signal handler.
void setBreadcrumb(std::string_view text);

struct CrashReport {
    std::filesystem::path path;
    std::string fileName;
    std::uintmax_t sizeBytes = 0;
    std::filesystem::file_time_type modified;
};

// Reports found in logDirectory(), newest first.
[[nodiscard]] std::vector<CrashReport> listReports();

// Reads a report's full text. Empty on failure.
[[nodiscard]] std::string readReport(const std::filesystem::path& path);

}  // namespace em::crash
