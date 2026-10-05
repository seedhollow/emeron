#pragma once

// Owns the device list and the current selection.
//
// A single background thread polls `adb devices -l`; everything else reads a
// snapshot. The selected serial and package are the app's session state -- the
// collectors and panels all read them from here rather than passing them down.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "adb/AdbClient.h"
#include "adb/AdbTypes.h"
#include "core/TaskQueue.h"

namespace em {

class DeviceManager {
public:
    explicit DeviceManager(std::shared_ptr<AdbClient> client);
    ~DeviceManager();

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;
    DeviceManager(DeviceManager&&) = delete;
    DeviceManager& operator=(DeviceManager&&) = delete;

    void start();
    void stop();

    [[nodiscard]] AdbClient& adb() noexcept { return *client_; }

    [[nodiscard]] DeviceList devices() const { return devices_.get(); }
    [[nodiscard]] std::uint64_t devicesRevision() const { return devices_.revision(); }

    [[nodiscard]] std::string selectedSerial() const;
    void selectSerial(std::string serial);
    [[nodiscard]] std::optional<DeviceRef> selectedDevice() const;
    [[nodiscard]] bool hasUsableSelection() const;

    // The package the per-app collectors target (gfxinfo, meminfo, atrace).
    [[nodiscard]] std::string selectedPackage() const;
    void selectPackage(std::string packageName);

    // Wakes the poll thread immediately.
    void requestRefresh();

    void setPollInterval(std::chrono::milliseconds interval);
    [[nodiscard]] std::chrono::milliseconds pollInterval() const;

    [[nodiscard]] bool polling() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    void pollLoop();
    void applyAutoSelection(const DeviceList& list);

    std::shared_ptr<AdbClient> client_;
    Shared<DeviceList> devices_;

    mutable std::mutex selectionMutex_;
    std::string selectedSerial_;
    std::string selectedPackage_;

    std::atomic<bool> running_{false};
    std::atomic<std::int64_t> pollIntervalMs_{1500};
    std::atomic<bool> refreshRequested_{false};

    std::mutex sleepMutex_;
    std::condition_variable sleepCv_;
    std::thread thread_;
};

}  // namespace em
