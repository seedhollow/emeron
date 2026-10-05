#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace em {

enum class DeviceState : std::uint8_t {
    Unknown,
    Offline,
    Online,         // "device"
    Unauthorized,   // RSA prompt not accepted
    Bootloader,
    Recovery,
    Sideload,
    NoPermissions,  // udev rules missing on Linux
    Connecting,
};

[[nodiscard]] DeviceState parseDeviceState(std::string_view token) noexcept;
[[nodiscard]] std::string_view toString(DeviceState state) noexcept;

// What `adb devices -l` reports, plus a couple of derived conveniences.
struct DeviceRef {
    std::string serial;
    DeviceState state = DeviceState::Unknown;
    std::string product;
    std::string model;
    std::string device;
    std::string transportId;

    [[nodiscard]] bool usable() const noexcept { return state == DeviceState::Online; }

    [[nodiscard]] bool isNetworkDevice() const noexcept {
        return serial.find(':') != std::string::npos;
    }

    [[nodiscard]] bool isEmulator() const noexcept {
        return serial.rfind("emulator-", 0) == 0;
    }

    // "Pixel 7 (28301FDH2003XY)" -- what the device picker shows.
    [[nodiscard]] std::string displayName() const {
        std::string label = model.empty() ? product : model;
        for (char& c : label) {
            if (c == '_') c = ' ';
        }
        if (label.empty()) return serial;
        return label + " (" + serial + ")";
    }
};

struct DeviceList {
    std::vector<DeviceRef> devices;
    bool serverReachable = false;
    std::string serverVersion;
    std::string lastError;

    [[nodiscard]] const DeviceRef* find(std::string_view serial) const {
        for (const auto& d : devices) {
            if (d.serial == serial) return &d;
        }
        return nullptr;
    }
};

}  // namespace em
