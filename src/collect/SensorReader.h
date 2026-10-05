#pragma once

// Sensor enumeration and best-effort live values from `dumpsys sensorservice`.
//
// WHAT IS AND IS NOT POSSIBLE FROM THE HOST
// -----------------------------------------
// Enumeration is complete and reliable: every sensor the HAL exposes shows up
// with its type, vendor, ranges, rates, FIFO depth and reporting mode.
//
// Live values are NOT. `dumpsys sensorservice` only prints recent events for
// sensors that some on-device app has already activated, and only the last
// handful of them. So:
//   * gyroscope/accelerometer values appear while an app is listening,
//   * polling tops out around 2 Hz before the dumpsys cost dominates,
//   * nothing appears for an idle sensor.
//
// Genuine high-rate streaming needs a small on-device agent (a tiny APK, or an
// `app_process` script) that registers a SensorEventListener and writes to a
// socket that emeron reads over `adb forward`. SensorSource is the seam for
// that: add an AgentSensorSource and the panel does not change.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "adb/DeviceManager.h"
#include "core/RingBuffer.h"
#include "core/TaskQueue.h"

namespace em {

struct SensorDescriptor {
    std::uint32_t handle = 0;
    std::string name;
    std::string vendor;
    std::string typeString;  // "android.sensor.gyroscope"
    int typeId = -1;
    int version = 0;
    std::string permission;
    std::string reportingMode;  // continuous | on-change | one-shot | special
    double minRateHz = -1.0;
    double maxRateHz = -1.0;
    int fifoMax = -1;
    int fifoReserved = -1;
    bool wakeUp = false;
    std::uint32_t flags = 0;

    // Short label for the UI: "Gyroscope" out of "android.sensor.gyroscope".
    [[nodiscard]] std::string shortType() const;
    [[nodiscard]] int axisCount() const;
};

struct SensorReading {
    std::uint32_t handle = 0;
    double timestampSec = 0.0;
    std::vector<double> values;
};

// Rolling history for the sensors the user chose to chart.
struct SensorTrace {
    RingBuffer<double> t;
    std::vector<RingBuffer<double>> axes;

    void setCapacity(std::size_t capacity);
    void push(double time, const std::vector<double>& values);
    void clear();

private:
    std::size_t capacity_ = 600;
};

struct SensorSnapshot {
    std::vector<SensorDescriptor> sensors;
    std::unordered_map<std::uint32_t, SensorReading> latest;
    std::unordered_map<std::uint32_t, SensorTrace> traces;
    int activeSensorCount = 0;
    std::string rawDump;  // kept so the user can read what we could not parse
    std::string lastError;
    bool loaded = false;
    std::uint64_t pollCount = 0;
};

class SensorReader {
public:
    explicit SensorReader(DeviceManager& devices);
    ~SensorReader();

    SensorReader(const SensorReader&) = delete;
    SensorReader& operator=(const SensorReader&) = delete;
    SensorReader(SensorReader&&) = delete;
    SensorReader& operator=(SensorReader&&) = delete;

    // One-shot enumeration; cheap and safe to call on device change.
    void enumerate(ThreadPool& pool);

    // Background polling of recent events. Off by default: it costs a full
    // `dumpsys sensorservice` per tick.
    void startPolling();
    void stopPolling();
    [[nodiscard]] bool polling() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    void setInterval(std::chrono::milliseconds interval);
    [[nodiscard]] std::chrono::milliseconds interval() const;

    template <typename Fn>
    void readSnapshot(Fn&& fn) const {
        snapshot_.read(std::forward<Fn>(fn));
    }
    [[nodiscard]] SensorSnapshot snapshot() const { return snapshot_.get(); }

    void clear();

private:
    void pollLoop();
    void pollOnce(std::string_view serial);

    DeviceManager& devices_;
    Shared<SensorSnapshot> snapshot_;
    std::atomic<bool> enumerating_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::int64_t> intervalMs_{1000};
    std::chrono::steady_clock::time_point startTime_{};

    std::mutex sleepMutex_;
    std::condition_variable sleepCv_;
    std::thread thread_;
};

// --- parsers, exposed for unit tests ---------------------------------------
[[nodiscard]] std::vector<SensorDescriptor> parseSensorList(std::string_view dump);
[[nodiscard]] std::vector<SensorReading> parseSensorEvents(std::string_view dump);
[[nodiscard]] std::optional<int> parseActiveSensorCount(std::string_view dump);

}  // namespace em
