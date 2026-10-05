#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "core/CrashHandler.h"

using namespace em;

namespace {

// install() installs process-wide signal handlers exactly once and is not
// meant to be re-pointed at a different directory mid-run, so these tests
// share one workspace rather than calling install() per test.
std::filesystem::path testWorkspace() {
    const auto dir = std::filesystem::temp_directory_path() / "emeron-test-crash-handler-7f3a";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

}  // namespace

TEST_CASE("crash::install creates the crash-logs directory") {
    const auto workspace = testWorkspace();
    crash::install(workspace);

    CHECK(std::filesystem::is_directory(crash::logDirectory()));
    CHECK_EQ(crash::logDirectory(), workspace / "crash-logs");
}

TEST_CASE("crash::listReports only returns .txt files, newest first") {
    const auto workspace = testWorkspace();
    crash::install(workspace);
    const auto dir = crash::logDirectory();

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
        if (entry.path().extension() == ".txt") std::filesystem::remove(entry.path());
    }

    { std::ofstream{dir / "crash-1-11.txt"} << "older"; }
    { std::ofstream{dir / "not-a-report.log"} << "ignored"; }
    std::filesystem::last_write_time(
        dir / "crash-1-11.txt",
        std::filesystem::file_time_type::clock::now() - std::chrono::seconds{10});
    { std::ofstream{dir / "crash-2-6.txt"} << "newer"; }

    const auto reports = crash::listReports();
    CHECK_EQ(reports.size(), std::size_t{2});
    if (reports.size() == 2) {
        CHECK_EQ(reports[0].fileName, std::string{"crash-2-6.txt"});
        CHECK_EQ(reports[1].fileName, std::string{"crash-1-11.txt"});
    }
}

TEST_CASE("crash::readReport returns a report's text") {
    const auto workspace = testWorkspace();
    crash::install(workspace);
    const auto path = crash::logDirectory() / "crash-9-11.txt";
    { std::ofstream{path} << "emeron crash report\nsignal: SIGSEGV\n"; }

    const std::string text = crash::readReport(path);
    CHECK(text.find("SIGSEGV") != std::string::npos);
}

TEST_CASE("crash::readReport returns empty for a missing file") {
    const auto workspace = testWorkspace();
    crash::install(workspace);
    CHECK(crash::readReport(crash::logDirectory() / "no-such-file.txt").empty());
}

TEST_CASE("crash::setBreadcrumb writes a context file the next crash report can read") {
    const auto workspace = testWorkspace();
    crash::install(workspace);
    crash::setBreadcrumb("Device Files: opening /sdcard/DCIM");

    const auto context = crash::logDirectory() / ".context";
    CHECK(std::filesystem::exists(context));

    std::ifstream in{context};
    std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    CHECK(text.find("Device Files: opening /sdcard/DCIM") != std::string::npos);
}
