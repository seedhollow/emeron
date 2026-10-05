#pragma once

// RAII child-process wrapper with piped stdin/stdout/stderr.
//
// Two usage shapes:
//   * runCommand(...)  -- one-shot, collect everything, bounded by a timeout.
//                         Used for every short `adb` invocation.
//   * Subprocess       -- long-lived handle for streaming children such as
//                         `adb logcat`, `adb shell perfetto` and the
//                         trace_processor httpd daemon.
//
// The child is put in its own process group so terminate() reaps grandchildren
// too -- `adb shell <cmd>` always spawns at least one.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.h"

namespace em {

struct CommandOutput {
    int exitCode = 0;
    std::string out;
    std::string err;
    bool timedOut = false;

    [[nodiscard]] bool ok() const noexcept { return exitCode == 0 && !timedOut; }
    [[nodiscard]] std::string_view firstErrLine() const noexcept;
};

class Subprocess {
public:
    Subprocess() = default;
    ~Subprocess();

    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;
    Subprocess(Subprocess&& other) noexcept;
    Subprocess& operator=(Subprocess&& other) noexcept;

    struct Options {
        std::vector<std::string> argv;
        bool pipeStdin = false;
        bool mergeStderrIntoStdout = false;
    };

    [[nodiscard]] static Result<Subprocess> spawn(Options options);

    // Reads at most dst.size() bytes. Returns 0 on EOF. `waitMs < 0` blocks.
    // A timeout is not an error: it returns 0 with `timedOut` set true.
    [[nodiscard]] Result<std::size_t> readStdout(std::span<char> dst, int waitMs,
                                                 bool* timedOut = nullptr);
    [[nodiscard]] Result<std::size_t> readStderr(std::span<char> dst, int waitMs,
                                                 bool* timedOut = nullptr);

    [[nodiscard]] Status writeStdin(std::string_view data);
    void closeStdin();

    [[nodiscard]] bool valid() const noexcept;

    // Non-blocking status check; nullopt while still running.
    [[nodiscard]] std::optional<int> tryWait();
    [[nodiscard]] Result<int> wait(std::chrono::milliseconds timeout);

    void terminate();  // SIGTERM to the group / TerminateProcess
    void killHard();   // SIGKILL to the group

    [[nodiscard]] const std::vector<std::string>& argv() const noexcept { return argv_; }

private:
    void closeAll() noexcept;
    void moveFrom(Subprocess&& other) noexcept;

    std::vector<std::string> argv_;

#if defined(_WIN32)
    void* processHandle_ = nullptr;  // HANDLE
    void* stdoutRead_ = nullptr;
    void* stderrRead_ = nullptr;
    void* stdinWrite_ = nullptr;
    unsigned long pid_ = 0;
#else
    int pid_ = -1;
    int stdoutRead_ = -1;
    int stderrRead_ = -1;
    int stdinWrite_ = -1;
#endif
    std::optional<int> exitCode_;
};

// One-shot execution. `stdinData` is written then stdin is closed.
[[nodiscard]] Result<CommandOutput> runCommand(std::vector<std::string> argv,
                                               std::chrono::milliseconds timeout,
                                               std::string_view stdinData = {});

[[nodiscard]] std::optional<std::filesystem::path> findOnPath(std::string_view name);

// Installs the process-wide signal handling the pipes need (ignores SIGPIPE on
// POSIX). Call once from main before spawning anything. No-op on Windows.
void initProcessSubsystem();

}  // namespace em
