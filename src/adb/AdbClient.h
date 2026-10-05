#pragma once

// Typed operations on top of IAdbTransport. Everything here blocks, so it must
// be called from a ThreadPool worker, never from the UI thread.

#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "adb/AdbTransport.h"
#include "adb/AdbTypes.h"

namespace em {

using PropertyMap = std::map<std::string, std::string>;

class AdbClient {
public:
    explicit AdbClient(std::shared_ptr<IAdbTransport> transport);

    [[nodiscard]] IAdbTransport& transport() noexcept { return *transport_; }
    [[nodiscard]] const IAdbTransport& transport() const noexcept { return *transport_; }
    [[nodiscard]] std::shared_ptr<IAdbTransport> transportPtr() const noexcept {
        return transport_;
    }

    // --- host level ---------------------------------------------------------
    [[nodiscard]] Status startServer();
    [[nodiscard]] Status killServer();
    [[nodiscard]] Result<std::string> serverVersion();
    [[nodiscard]] Result<DeviceList> listDevices();
    [[nodiscard]] Status connectNetworkDevice(std::string_view hostPort);
    [[nodiscard]] Status disconnectNetworkDevice(std::string_view hostPort);

    // --- shell --------------------------------------------------------------
    // Returns stdout. Device-side non-zero exits are not an error here; many
    // dumpsys/toybox commands exit non-zero while still producing usable text.
    [[nodiscard]] Result<std::string> shell(std::string_view serial, std::string_view command,
                                            std::chrono::milliseconds timeout = kAdbNormalTimeout);

    // Fails if the device-side command exited non-zero.
    [[nodiscard]] Result<std::string> shellChecked(
        std::string_view serial, std::string_view command,
        std::chrono::milliseconds timeout = kAdbNormalTimeout);

    [[nodiscard]] Result<CommandOutput> shellRaw(
        std::string_view serial, std::string_view command,
        std::chrono::milliseconds timeout = kAdbNormalTimeout);

    // Runs several commands in ONE adb round trip, separated by a sentinel.
    // Each adb invocation costs 20-60 ms of setup, so the metric sampler
    // batches its whole probe set through here. Output order matches input;
    // a command that produces nothing yields an empty string.
    [[nodiscard]] Result<std::vector<std::string>> shellBatch(
        std::string_view serial, std::span<const std::string> commands,
        std::chrono::milliseconds timeout = kAdbNormalTimeout);

    // --- device facts -------------------------------------------------------
    [[nodiscard]] Result<PropertyMap> properties(std::string_view serial);
    [[nodiscard]] Result<std::vector<std::string>> packages(std::string_view serial,
                                                            bool thirdPartyOnly);
    [[nodiscard]] Result<std::string> foregroundPackage(std::string_view serial);
    [[nodiscard]] Result<std::vector<int>> pidsOf(std::string_view serial,
                                                  std::string_view packageName);
    [[nodiscard]] Result<bool> hasRoot(std::string_view serial);

    // --- file transfer ------------------------------------------------------
    [[nodiscard]] Status pull(std::string_view serial, std::string_view remotePath,
                              const std::filesystem::path& localPath,
                              std::chrono::milliseconds timeout = kAdbLongTimeout);
    [[nodiscard]] Status push(std::string_view serial,
                              const std::filesystem::path& localPath,
                              std::string_view remotePath,
                              std::chrono::milliseconds timeout = kAdbLongTimeout);

private:
    std::shared_ptr<IAdbTransport> transport_;
};

// Exposed for unit tests.
[[nodiscard]] DeviceList parseDevicesOutput(std::string_view text);
[[nodiscard]] PropertyMap parseGetpropOutput(std::string_view text);
[[nodiscard]] std::vector<std::string> splitBatchOutput(std::string_view text,
                                                        std::string_view sentinel,
                                                        std::size_t expectedCount);
}  // namespace em
