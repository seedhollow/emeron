#include "adb/DeviceManager.h"

#include <utility>

#include "core/Log.h"

namespace em {
namespace {
constexpr const char* kCategory = "devices";
}

DeviceManager::DeviceManager(std::shared_ptr<AdbClient> client) : client_(std::move(client)) {}

DeviceManager::~DeviceManager() { stop(); }

void DeviceManager::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    thread_ = std::thread([this] { pollLoop(); });
}

void DeviceManager::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    sleepCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::string DeviceManager::selectedSerial() const {
    const std::lock_guard lock{selectionMutex_};
    return selectedSerial_;
}

void DeviceManager::selectSerial(std::string serial) {
    {
        const std::lock_guard lock{selectionMutex_};
        if (selectedSerial_ == serial) return;
        selectedSerial_ = std::move(serial);
        // A different device means the package selection is meaningless.
        selectedPackage_.clear();
    }
    EM_LOG_INFO(kCategory, "selected device: " + selectedSerial());
}

std::optional<DeviceRef> DeviceManager::selectedDevice() const {
    const std::string serial = selectedSerial();
    if (serial.empty()) return std::nullopt;

    std::optional<DeviceRef> found;
    devices_.read([&](const DeviceList& list) {
        if (const DeviceRef* ref = list.find(serial)) found = *ref;
    });
    return found;
}

bool DeviceManager::hasUsableSelection() const {
    const auto device = selectedDevice();
    return device && device->usable();
}

std::string DeviceManager::selectedPackage() const {
    const std::lock_guard lock{selectionMutex_};
    return selectedPackage_;
}

void DeviceManager::selectPackage(std::string packageName) {
    const std::lock_guard lock{selectionMutex_};
    selectedPackage_ = std::move(packageName);
}

void DeviceManager::requestRefresh() {
    refreshRequested_.store(true, std::memory_order_release);
    sleepCv_.notify_all();
}

void DeviceManager::setPollInterval(std::chrono::milliseconds interval) {
    pollIntervalMs_.store(interval.count() < 200 ? 200 : interval.count(),
                          std::memory_order_release);
    sleepCv_.notify_all();
}

std::chrono::milliseconds DeviceManager::pollInterval() const {
    return std::chrono::milliseconds{pollIntervalMs_.load(std::memory_order_acquire)};
}

void DeviceManager::applyAutoSelection(const DeviceList& list) {
    const std::lock_guard lock{selectionMutex_};

    const bool stillPresent =
        !selectedSerial_.empty() && list.find(selectedSerial_) != nullptr;
    if (stillPresent) return;

    if (!selectedSerial_.empty()) {
        EM_LOG_WARN(kCategory, "device disappeared: " + selectedSerial_);
        selectedSerial_.clear();
        selectedPackage_.clear();
    }

    // Prefer a physical device over an emulator when both are attached; a
    // perf tool is almost always pointed at real hardware.
    const DeviceRef* best = nullptr;
    for (const auto& d : list.devices) {
        if (!d.usable()) continue;
        if (best == nullptr || (best->isEmulator() && !d.isEmulator())) best = &d;
    }
    if (best != nullptr) {
        selectedSerial_ = best->serial;
        EM_LOG_INFO(kCategory, "auto-selected device: " + selectedSerial_);
    }
}

void DeviceManager::pollLoop() {
    // One version query up front so the UI can show which adb is in play.
    std::string version;
    if (auto v = client_->serverVersion()) {
        version = std::move(v).value();
    } else {
        EM_LOG_WARN(kCategory, "adb version query failed: " + v.describe());
    }

    while (running_.load(std::memory_order_acquire)) {
        refreshRequested_.store(false, std::memory_order_release);

        auto result = client_->listDevices();
        DeviceList list = result ? std::move(result).value() : DeviceList{};
        if (!result) list.lastError = result.error().message;
        list.serverVersion = version;

        applyAutoSelection(list);
        devices_.set(std::move(list));

        std::unique_lock lock{sleepMutex_};
        sleepCv_.wait_for(lock, pollInterval(), [this] {
            return !running_.load(std::memory_order_acquire) ||
                   refreshRequested_.load(std::memory_order_acquire);
        });
    }
}

}  // namespace em
