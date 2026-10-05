#pragma once

// The one seam between emeron and adb.
//
// Only AdbCliTransport exists today (it shells out to the `adb` binary). The
// interface is here so a native smartsockets client -- speaking the protocol
// to the adb server on tcp/5037 directly, which removes a process spawn per
// call and gives real binary streams for file transfer -- can be dropped in
// without touching AdbClient or any panel.

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.h"
#include "core/Subprocess.h"

namespace em {

inline constexpr std::chrono::milliseconds kAdbQuickTimeout{4'000};
inline constexpr std::chrono::milliseconds kAdbNormalTimeout{15'000};
inline constexpr std::chrono::milliseconds kAdbLongTimeout{120'000};

class IAdbTransport {
public:
    virtual ~IAdbTransport() = default;

    IAdbTransport() = default;
    IAdbTransport(const IAdbTransport&) = delete;
    IAdbTransport& operator=(const IAdbTransport&) = delete;
    IAdbTransport(IAdbTransport&&) = delete;
    IAdbTransport& operator=(IAdbTransport&&) = delete;

    // True once the adb binary (or server) has been located.
    [[nodiscard]] virtual bool available() const = 0;

    // Human-readable description of where adb came from, for the UI.
    [[nodiscard]] virtual std::string describe() const = 0;

    // Runs a host-level adb command, e.g. {"devices", "-l"}.
    [[nodiscard]] virtual Result<CommandOutput> host(std::span<const std::string> args,
                                                     std::chrono::milliseconds timeout) = 0;

    // Runs a command on a device: adb -s <serial> <args...>
    [[nodiscard]] virtual Result<CommandOutput> device(std::string_view serial,
                                                       std::span<const std::string> args,
                                                       std::chrono::milliseconds timeout) = 0;

    // adb -s <serial> shell <command>. `command` is passed to the device's sh.
    [[nodiscard]] virtual Result<CommandOutput> shell(std::string_view serial,
                                                      std::string_view command,
                                                      std::chrono::milliseconds timeout) = 0;

    // Long-lived child for streaming output (logcat, perfetto, screenrecord).
    // The caller owns the handle and must keep reading or kill it.
    [[nodiscard]] virtual Result<std::unique_ptr<Subprocess>> stream(
        std::string_view serial, std::span<const std::string> args, bool pipeStdin) = 0;
};

}  // namespace em
