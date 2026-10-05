#pragma once

#include <filesystem>
#include <mutex>
#include <optional>

#include "adb/AdbTransport.h"

namespace em {

// Shells out to the `adb` executable. Located, in order of preference:
//   1. an explicit path passed to the constructor or EMERON_ADB
//   2. $ANDROID_HOME / $ANDROID_SDK_ROOT platform-tools
//   3. PATH
//   4. the usual per-OS SDK install locations
class AdbCliTransport final : public IAdbTransport {
public:
    explicit AdbCliTransport(std::optional<std::filesystem::path> explicitPath = std::nullopt);

    [[nodiscard]] bool available() const override;
    [[nodiscard]] std::string describe() const override;

    [[nodiscard]] Result<CommandOutput> host(std::span<const std::string> args,
                                             std::chrono::milliseconds timeout) override;
    [[nodiscard]] Result<CommandOutput> device(std::string_view serial,
                                               std::span<const std::string> args,
                                               std::chrono::milliseconds timeout) override;
    [[nodiscard]] Result<CommandOutput> shell(std::string_view serial,
                                              std::string_view command,
                                              std::chrono::milliseconds timeout) override;
    [[nodiscard]] Result<std::unique_ptr<Subprocess>> stream(
        std::string_view serial, std::span<const std::string> args, bool pipeStdin) override;

    // Re-runs discovery, e.g. after the user points at a different SDK.
    void relocate(std::optional<std::filesystem::path> explicitPath = std::nullopt);

    [[nodiscard]] std::optional<std::filesystem::path> binaryPath() const;

    // Candidate SDK locations probed during discovery. Exposed so the UI can
    // tell the user where it looked when adb is missing.
    [[nodiscard]] static std::vector<std::filesystem::path> searchCandidates();

private:
    [[nodiscard]] std::vector<std::string> buildArgv(std::string_view serial,
                                                     std::span<const std::string> args) const;

    mutable std::mutex mutex_;
    std::optional<std::filesystem::path> binary_;
    std::string origin_;
};

}  // namespace em
