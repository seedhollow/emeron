#pragma once

// Perfetto trace capture, driven entirely from the host.
//
// The recipe is Perfetto's own documented one:
//     cat config.pbtxt | adb shell perfetto -c - --txt -o /data/misc/perfetto-traces/<name>
//     adb pull /data/misc/perfetto-traces/<name> <local>
//
// We build the text config rather than the binary one: it is readable, the
// panel can show exactly what will be sent, and the user can paste in their own.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "adb/DeviceManager.h"
#include "core/TaskQueue.h"

namespace em {

struct PerfettoCaptureOptions {
    int durationMs = 10'000;
    int bufferSizeKb = 63'488;  // 62 MiB, Perfetto's usual starting point

    // atrace categories: gfx, view, input, wm, am, sched, freq, binder_driver...
    std::vector<std::string> atraceCategories{"gfx", "view", "input", "wm", "am", "sched",
                                              "freq", "idle", "binder_driver", "hal", "res",
                                              "dalvik"};
    // Per-app atrace. Empty means "*" is NOT used; we only add what is asked.
    std::vector<std::string> atraceApps;

    bool ftraceSchedAndFreq = true;
    bool processStats = true;
    int processStatsPollMs = 1000;
    bool frameTimeline = true;   // android.surfaceflinger.frametimeline
    bool logcat = false;
    bool javaHeapGraph = false;

    std::string remoteDirectory = "/data/misc/perfetto-traces";
    std::string traceName;  // empty -> auto timestamped name

    // When set, this text is sent verbatim instead of the generated config.
    std::string customConfig;
};

struct CaptureStatus {
    enum class Phase : std::uint8_t { Idle, Starting, Recording, Pulling, Done, Failed };

    Phase phase = Phase::Idle;
    double progress = 0.0;  // 0..1 during Recording
    std::string message;
    std::string remotePath;
    std::filesystem::path localPath;
    std::string configUsed;
    std::string deviceStderr;
    std::chrono::steady_clock::time_point startedAt{};

    [[nodiscard]] bool busy() const noexcept {
        return phase == Phase::Starting || phase == Phase::Recording || phase == Phase::Pulling;
    }
};

class PerfettoCapture {
public:
    explicit PerfettoCapture(DeviceManager& devices);
    ~PerfettoCapture();

    PerfettoCapture(const PerfettoCapture&) = delete;
    PerfettoCapture& operator=(const PerfettoCapture&) = delete;
    PerfettoCapture(PerfettoCapture&&) = delete;
    PerfettoCapture& operator=(PerfettoCapture&&) = delete;

    // Starts a capture on the selected device. Returns false if one is running.
    bool start(PerfettoCaptureOptions options, std::filesystem::path outputDirectory);
    void cancel();

    [[nodiscard]] CaptureStatus status() const { return status_.get(); }

    // Checks whether traced/traced_probes are up and the shell may use them.
    void probeSupport(ThreadPool& pool);
    [[nodiscard]] bool supportKnown() const noexcept {
        return supportKnown_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool supported() const noexcept {
        return supported_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::string supportMessage() const { return supportMessage_.get(); }

private:
    void runCapture(PerfettoCaptureOptions options, std::filesystem::path outputDirectory);

    DeviceManager& devices_;
    Shared<CaptureStatus> status_;
    Shared<std::string> supportMessage_;
    std::atomic<bool> supportKnown_{false};
    std::atomic<bool> supported_{false};
    std::atomic<bool> cancelRequested_{false};
    std::atomic<bool> busy_{false};
    std::thread thread_;
    std::mutex threadMutex_;
};

// Builds the Perfetto text config. Pure function, unit tested.
[[nodiscard]] std::string buildTraceConfig(const PerfettoCaptureOptions& options);

// Default name: emeron-20260104-153012.perfetto-trace
[[nodiscard]] std::string makeTraceName();

}  // namespace em
