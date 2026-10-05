#pragma once

// One-shot collection of everything static about a device: build properties,
// SoC, display, storage, network, security posture.
//
// All of it comes from `getprop` plus a single batched shell round trip, so a
// full refresh is one adb call.

#include <memory>
#include <string>
#include <vector>

#include "adb/DeviceManager.h"
#include "core/TaskQueue.h"

namespace em {

struct InfoField {
    std::string label;
    std::string value;
    std::string sourceKey;  // the getprop key or command it came from
    // Plain-language note shown with sourceKey on hover; may be empty.
    std::string explanation;
};

struct InfoSection {
    std::string title;
    std::vector<InfoField> fields;
};

struct DeviceInfoSnapshot {
    std::string serial;
    std::vector<InfoSection> sections;
    PropertyMap properties;  // the full raw getprop dump
    std::string cpuInfo;
    std::string lastError;
    bool loaded = false;
    bool loading = false;
};

class DeviceInfoCollector {
public:
    explicit DeviceInfoCollector(DeviceManager& devices);

    // Kicks off an async refresh on `pool`. Safe to call repeatedly; a second
    // call while one is in flight is ignored.
    void refresh(ThreadPool& pool);

    template <typename Fn>
    void readSnapshot(Fn&& fn) const {
        snapshot_.read(std::forward<Fn>(fn));
    }
    [[nodiscard]] DeviceInfoSnapshot snapshot() const { return snapshot_.get(); }

    void clear();

private:
    DeviceManager& devices_;
    Shared<DeviceInfoSnapshot> snapshot_;
    std::atomic<bool> inFlight_{false};
};

// Exposed for unit tests: turns a property map plus the extra command outputs
// into the display sections.
struct DeviceInfoExtras {
    std::string screenSize;
    std::string screenDensity;
    std::string displayDump;
    std::string storageDump;
    std::string cpuInfo;
    std::string uptime;
    std::string networkDump;
    std::string selinux;
    std::string kernel;
    std::string sensorCount;
};
[[nodiscard]] std::vector<InfoSection> buildInfoSections(const PropertyMap& props,
                                                         const DeviceInfoExtras& extras);

}  // namespace em
