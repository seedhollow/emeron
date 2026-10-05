#pragma once

// Periodic device-wide and per-app metric collection.
//
// Everything the dashboard needs is gathered in ONE batched `adb shell` round
// trip per tick (see AdbClient::shellBatch). A separate invocation per probe
// would cost 6-8 process spawns and 150+ ms per sample, which is visible as
// jitter in the plots.
//
// Sources, all readable without root on a normal userdebug/user build:
//   /proc/stat                     -> total and per-core CPU
//   /proc/meminfo                  -> RAM and swap
//   dumpsys battery                -> level, temperature, current, voltage
//   /sys/class/thermal/*           -> thermal zones (throttling correlation)
//   /proc/<pid>/stat, /proc/<pid>/statm, /proc/<pid>/status -> per-app

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "adb/DeviceManager.h"
#include "core/RingBuffer.h"
#include "core/TaskQueue.h"

namespace em {

// Raw jiffy counters from one /proc/stat cpu line.
struct CpuTimes {
    std::uint64_t user = 0, nice = 0, system = 0, idle = 0;
    std::uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;

    [[nodiscard]] std::uint64_t total() const noexcept {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
    [[nodiscard]] std::uint64_t busy() const noexcept { return total() - idle - iowait; }
};

struct CpuSnapshot {
    CpuTimes aggregate;
    std::vector<CpuTimes> cores;
};

struct MetricSample {
    double t = 0.0;  // seconds since the sampler started

    double cpuTotalPct = -1.0;
    std::vector<double> cpuCorePct;

    double memTotalMiB = -1.0;
    double memAvailableMiB = -1.0;
    double memUsedMiB = -1.0;
    double swapUsedMiB = -1.0;

    double batteryLevelPct = -1.0;
    double batteryTempC = -1.0;
    double batteryCurrentMa = 0.0;  // negative while discharging
    double batteryVoltageV = -1.0;

    double maxThermalC = -1.0;
    std::vector<double> thermalC;

    // Per-app, only populated when a package is selected and running.
    int appPid = -1;
    double appCpuPct = -1.0;
    double appRssMiB = -1.0;
    int appThreads = -1;

    [[nodiscard]] bool hasApp() const noexcept { return appPid > 0; }
};

// Plot-ready column store. Every series shares the `t` axis, so ImPlot can
// draw straight out of these buffers with no copying.
struct MetricHistory {
    RingBuffer<double> t;
    RingBuffer<double> cpuTotal;
    std::vector<RingBuffer<double>> cpuCores;
    RingBuffer<double> memUsed;
    RingBuffer<double> memAvailable;
    RingBuffer<double> batteryTemp;
    RingBuffer<double> batteryCurrent;
    RingBuffer<double> batteryLevel;
    RingBuffer<double> maxThermal;
    RingBuffer<double> appCpu;
    RingBuffer<double> appRss;

    MetricSample latest;
    std::uint64_t sampleCount = 0;
    std::string lastError;

    void setCapacity(std::size_t capacity);
    void push(const MetricSample& sample);
    void clear();

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return t.empty(); }

private:
    std::size_t capacity_ = 1800;  // 3 minutes at 100 ms
};

// Which probes actually returned data on this device, so the UI can grey out
// panels instead of showing empty plots.
struct ProbeSupport {
    bool procStat = false;
    bool meminfo = false;
    bool battery = false;
    bool thermal = false;
    bool perApp = false;
    std::vector<std::string> thermalZoneNames;
    int coreCount = 0;
};

class MetricSampler {
public:
    explicit MetricSampler(DeviceManager& devices);
    ~MetricSampler();

    MetricSampler(const MetricSampler&) = delete;
    MetricSampler& operator=(const MetricSampler&) = delete;
    MetricSampler(MetricSampler&&) = delete;
    MetricSampler& operator=(MetricSampler&&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    void setInterval(std::chrono::milliseconds interval);
    [[nodiscard]] std::chrono::milliseconds interval() const;

    void setHistorySeconds(double seconds);
    [[nodiscard]] double historySeconds() const;

    void reset();

    // Borrow the history under the lock -- no copy. Safe to call from the UI
    // thread each frame; the writer only touches it once per interval.
    template <typename Fn>
    void readHistory(Fn&& fn) const {
        history_.read(std::forward<Fn>(fn));
    }

    [[nodiscard]] MetricSample latest() const;
    [[nodiscard]] ProbeSupport support() const { return support_.get(); }

private:
    void sampleLoop();
    [[nodiscard]] std::optional<MetricSample> collectOnce(std::string_view serial,
                                                          std::string_view packageName);

    DeviceManager& devices_;
    Shared<MetricHistory> history_;
    Shared<ProbeSupport> support_;

    // Previous counters, needed to turn jiffies into percentages.
    CpuSnapshot previousCpu_;
    bool hasPreviousCpu_ = false;
    std::uint64_t previousAppJiffies_ = 0;
    bool hasPreviousApp_ = false;
    double previousSampleTime_ = 0.0;
    int cachedPid_ = -1;
    std::string cachedPidPackage_;
    long clockTicksPerSecond_ = 100;

    std::chrono::steady_clock::time_point startTime_;

    std::atomic<bool> running_{false};
    std::atomic<std::int64_t> intervalMs_{500};
    std::atomic<std::int64_t> historyMs_{180'000};

    std::mutex sleepMutex_;
    std::condition_variable sleepCv_;
    std::thread thread_;
};

// --- parsers, exposed for unit tests ---------------------------------------
[[nodiscard]] std::optional<CpuSnapshot> parseProcStat(std::string_view text);
[[nodiscard]] std::optional<CpuTimes> parseCpuLine(std::string_view line);

struct MemInfo {
    double totalMiB = -1.0, availableMiB = -1.0, freeMiB = -1.0;
    double swapTotalMiB = -1.0, swapFreeMiB = -1.0;
};
[[nodiscard]] std::optional<MemInfo> parseMemInfo(std::string_view text);

struct BatteryInfo {
    double levelPct = -1.0, tempC = -1.0, currentMa = 0.0, voltageV = -1.0;
    bool charging = false;
};
[[nodiscard]] std::optional<BatteryInfo> parseBatteryDump(std::string_view text);

// Each line is a millidegree value from one thermal zone.
[[nodiscard]] std::vector<double> parseThermalZones(std::string_view text);

// utime+stime in jiffies from /proc/<pid>/stat.
[[nodiscard]] std::optional<std::uint64_t> parseProcPidStatJiffies(std::string_view text);
// VmRSS in MiB from /proc/<pid>/status, plus Threads.
struct ProcStatus {
    double rssMiB = -1.0;
    int threads = -1;
};
[[nodiscard]] std::optional<ProcStatus> parseProcPidStatus(std::string_view text);

}  // namespace em
