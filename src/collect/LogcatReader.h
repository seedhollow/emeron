#pragma once

// Device logcat, streamed from a long-lived `adb logcat` child and scoped to
// the selected package's process.
//
// Two layers of filtering, deliberately:
//
//   * Device side (`--pid=N`) cuts USB traffic on a chatty device, but logcat
//     takes only one pid, so it is used only when the package resolves to
//     exactly one process. Changing it restarts the stream.
//   * Host side (pid set, level, tag, text) runs at display time over the
//     whole retained buffer. That is what makes this better than a terminal:
//     you can widen a filter and immediately see context you already captured,
//     with no restart and no lost lines.
//
// The buffer therefore holds everything the stream delivered; the panel decides
// what to show. One consequence worth knowing: with scope set to the selected
// process and the app not yet running, lines are still being retained, so the
// app's own startup logs become visible the moment its pid resolves.
//
// Process restarts are handled: the reader re-resolves the package's pids every
// couple of seconds and restarts the stream when they change, so an app that
// crashes and comes back keeps logging without intervention.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "adb/DeviceManager.h"
#include "core/TaskQueue.h"

namespace em {

// The single-character priority logcat prints in the threadtime format.
enum class LogcatLevel : std::uint8_t {
    Verbose = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Silent,
    Unknown,
};

[[nodiscard]] LogcatLevel parseLogcatLevel(char code) noexcept;
[[nodiscard]] std::string_view toString(LogcatLevel level) noexcept;
[[nodiscard]] char toChar(LogcatLevel level) noexcept;

struct LogcatEntry {
    // Seconds since the reader started, so a spike here can be lined up
    // against the dashboard's metric plots.
    double hostTime = 0.0;

    std::string deviceTime;  // "10-04 17:45:01.123" as the device reported it
    int pid = -1;
    int tid = -1;
    LogcatLevel level = LogcatLevel::Unknown;
    std::string tag;
    std::string message;

    // Separator lines ("--------- beginning of main") and anything that did
    // not match the expected format are kept verbatim rather than dropped.
    bool raw = false;

    [[nodiscard]] bool isFatal() const noexcept { return level == LogcatLevel::Fatal; }
};

enum class LogcatScope : std::uint8_t { AllProcesses, SelectedProcess };

struct LogcatOptions {
    LogcatScope scope = LogcatScope::SelectedProcess;

    // Pass --pid to logcat when the package resolves to a single process.
    bool deviceSideFilter = true;

    // -T N: how much of the device's existing ring to replay on connect.
    int backlogLines = 400;

    // -b: empty means logcat's default set (main, system, crash).
    std::vector<std::string> buffers;

    // Minimum priority requested from the device (-> "*:<level>").
    // Host-side level filtering is separate and lives in the panel.
    LogcatLevel deviceMinLevel = LogcatLevel::Verbose;

    [[nodiscard]] bool operator==(const LogcatOptions&) const = default;
};

struct LogcatSnapshot {
    std::deque<LogcatEntry> entries;

    std::uint64_t totalReceived = 0;
    std::uint64_t droppedToCap = 0;
    std::uint64_t parseFailures = 0;

    bool streaming = false;
    std::string lastError;
    std::string commandLine;  // the argv actually spawned, shown in the UI

    std::string trackedPackage;
    std::vector<int> trackedPids;
    bool usingDeviceSideFilter = false;

    [[nodiscard]] bool tracksPid(int pid) const {
        for (const int candidate : trackedPids) {
            if (candidate == pid) return true;
        }
        return false;
    }
};

// Host-side display filter.
//
// This lives here rather than in the panel because it is the rule that defines
// "logcat from process", not a presentation detail -- and keeping it out of the
// UI is what makes it testable.
struct LogcatFilter {
    LogcatLevel minLevel = LogcatLevel::Verbose;
    std::string tag;
    std::string text;

    // Treat `text` as something to hide rather than something to keep.
    bool excludeText = false;

    // Restrict to the pids the reader resolved for the selected package.
    bool onlyTrackedPids = true;
    LogcatScope scope = LogcatScope::SelectedProcess;

    [[nodiscard]] bool accepts(const LogcatEntry& entry,
                               const LogcatSnapshot& snapshot) const;

    // Changes to any of these invalidate a cached filter result.
    [[nodiscard]] std::string signature(const LogcatSnapshot& snapshot) const;
};

class LogcatReader {
public:
    explicit LogcatReader(DeviceManager& devices);
    ~LogcatReader();

    LogcatReader(const LogcatReader&) = delete;
    LogcatReader& operator=(const LogcatReader&) = delete;
    LogcatReader(LogcatReader&&) = delete;
    LogcatReader& operator=(LogcatReader&&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // Restarts the stream if the new options would change the argv.
    void setOptions(LogcatOptions options);
    [[nodiscard]] LogcatOptions options() const;

    void setCapacity(std::size_t lines);
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_.load(std::memory_order_acquire);
    }

    // Drops the host-side buffer only.
    void clearBuffer();

    // `adb logcat -c`, which wipes the device's ring. Stops and restarts the
    // stream around it so the reader does not race the clear.
    void clearDeviceBuffer(ThreadPool& pool);

    // Forces the stream to be torn down and re-established.
    void requestRestart();

    template <typename Fn>
    void readSnapshot(Fn&& fn) const {
        snapshot_.read(std::forward<Fn>(fn));
    }
    [[nodiscard]] std::uint64_t revision() const { return snapshot_.revision(); }

private:
    void readerLoop();
    [[nodiscard]] std::vector<int> resolvePids(std::string_view serial,
                                               std::string_view packageName);
    void appendBatch(std::vector<LogcatEntry>&& batch, std::size_t failures);

    DeviceManager& devices_;
    Shared<LogcatSnapshot> snapshot_;

    mutable std::mutex optionsMutex_;
    LogcatOptions options_;

    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::size_t> capacity_{20'000};
    std::atomic<bool> suspended_{false};

    std::chrono::steady_clock::time_point startTime_{};

    std::mutex sleepMutex_;
    std::condition_variable sleepCv_;
    std::thread thread_;
};

// --- exposed for unit tests -------------------------------------------------

// Delay before restarting a stream that keeps dying: 0 when healthy, then
// 0.5 s doubling to a 10 s ceiling.
[[nodiscard]] std::chrono::milliseconds logcatRetryDelay(int consecutiveFailures) noexcept;

// Builds the argv after the leading "logcat".
[[nodiscard]] std::vector<std::string> buildLogcatArgs(const LogcatOptions& options,
                                                       const std::vector<int>& pids);

// Parses one line of `logcat -v threadtime`:
//   "10-04 17:45:01.123  1234  1256 I MyTag: message text"
[[nodiscard]] LogcatEntry parseLogcatLine(std::string_view line, double hostTime);

// Moves every complete line out of `buffer` into `lines`, leaving a trailing
// partial line behind for the next read. Returns owned strings rather than
// views precisely because `buffer` is mutated and reused across reads.
// Tolerates CRLF from the Windows adb build.
void drainCompleteLines(std::string& buffer, std::vector<std::string>& lines);

}  // namespace em
