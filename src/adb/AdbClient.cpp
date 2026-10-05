#include "adb/AdbClient.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "core/Log.h"
#include "core/StringUtil.h"

namespace em {
namespace {

constexpr const char* kCategory = "adb";

// Unlikely to appear in any dumpsys or /proc output.
constexpr std::string_view kBatchSentinel = "@@EMERON_SEP_7f3a@@";

}  // namespace

DeviceState parseDeviceState(std::string_view token) noexcept {
    if (token == "device") return DeviceState::Online;
    if (token == "offline") return DeviceState::Offline;
    if (token == "unauthorized") return DeviceState::Unauthorized;
    if (token == "bootloader") return DeviceState::Bootloader;
    if (token == "recovery") return DeviceState::Recovery;
    if (token == "sideload") return DeviceState::Sideload;
    if (token == "no" || token == "no permissions") return DeviceState::NoPermissions;
    if (token == "connecting" || token == "authorizing") return DeviceState::Connecting;
    return DeviceState::Unknown;
}

std::string_view toString(DeviceState state) noexcept {
    switch (state) {
        case DeviceState::Online:
            return "online";
        case DeviceState::Offline:
            return "offline";
        case DeviceState::Unauthorized:
            return "unauthorized";
        case DeviceState::Bootloader:
            return "bootloader";
        case DeviceState::Recovery:
            return "recovery";
        case DeviceState::Sideload:
            return "sideload";
        case DeviceState::NoPermissions:
            return "no permissions";
        case DeviceState::Connecting:
            return "connecting";
        case DeviceState::Unknown:
            break;
    }
    return "unknown";
}

// Parses `adb devices -l`:
//   List of devices attached
//   28301FDH2003XY  device product:cheetah model:Pixel_7_Pro device:cheetah transport_id:3
//   emulator-5554   device product:sdk_gphone64_arm64 model:sdk_gphone64_arm64 ...
DeviceList parseDevicesOutput(std::string_view text) {
    DeviceList list;
    list.serverReachable = true;

    for (const auto line : splitLines(text)) {
        const std::string_view trimmed = trim(line);
        if (trimmed.empty()) continue;
        if (trimmed.starts_with("List of devices")) continue;
        if (trimmed.starts_with("*")) continue;  // "* daemon started successfully"
        if (trimmed.starts_with("adb:")) continue;

        const auto fields = tokenize(trimmed);
        if (fields.size() < 2) continue;

        DeviceRef ref;
        ref.serial = std::string{fields[0]};
        ref.state = parseDeviceState(fields[1]);

        for (std::size_t i = 2; i < fields.size(); ++i) {
            if (const auto v = stripPrefix(fields[i], "product:")) {
                ref.product = std::string{*v};
            } else if (const auto m = stripPrefix(fields[i], "model:")) {
                ref.model = std::string{*m};
            } else if (const auto d = stripPrefix(fields[i], "device:")) {
                ref.device = std::string{*d};
            } else if (const auto t = stripPrefix(fields[i], "transport_id:")) {
                ref.transportId = std::string{*t};
            }
        }
        list.devices.push_back(std::move(ref));
    }

    std::sort(list.devices.begin(), list.devices.end(),
              [](const DeviceRef& a, const DeviceRef& b) {
                  // Usable devices first, then alphabetical by serial.
                  if (a.usable() != b.usable()) return a.usable();
                  return a.serial < b.serial;
              });
    return list;
}

PropertyMap parseGetpropOutput(std::string_view text) {
    PropertyMap props;
    // Lines look like: [ro.product.model]: [Pixel 7 Pro]
    for (const auto line : splitLines(text)) {
        const std::string_view t = trim(line);
        if (t.size() < 5 || t.front() != '[') continue;

        const auto keyEnd = t.find(']');
        if (keyEnd == std::string_view::npos) continue;
        const auto valStart = t.find('[', keyEnd);
        if (valStart == std::string_view::npos) continue;
        const auto valEnd = t.rfind(']');
        if (valEnd == std::string_view::npos || valEnd <= valStart) continue;

        props.emplace(std::string{t.substr(1, keyEnd - 1)},
                      std::string{t.substr(valStart + 1, valEnd - valStart - 1)});
    }
    return props;
}

std::vector<std::string> splitBatchOutput(std::string_view text, std::string_view sentinel,
                                          std::size_t expectedCount) {
    std::vector<std::string> parts;
    parts.reserve(expectedCount);

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const auto found = text.find(sentinel, pos);
        const auto end = (found == std::string_view::npos) ? text.size() : found;
        std::string_view chunk = text.substr(pos, end - pos);
        // Each sentinel is emitted by its own `echo`, so strip the newline
        // that precedes it and the one that follows it.
        parts.emplace_back(trimRight(chunk));
        if (found == std::string_view::npos) break;
        pos = found + sentinel.size();
        if (pos < text.size() && text[pos] == '\n') ++pos;
    }

    parts.resize(expectedCount);
    return parts;
}

AdbClient::AdbClient(std::shared_ptr<IAdbTransport> transport)
    : transport_(std::move(transport)) {}

Status AdbClient::startServer() {
    const std::vector<std::string> args{"start-server"};
    EM_TRY(out, transport_->host(args, kAdbNormalTimeout));
    if (!out.ok()) {
        return makeError(ErrorKind::ProcessFailed,
                         "adb start-server failed: " + std::string{out.firstErrLine()},
                         out.exitCode);
    }
    return {};
}

Status AdbClient::killServer() {
    const std::vector<std::string> args{"kill-server"};
    EM_TRY(out, transport_->host(args, kAdbNormalTimeout));
    (void)out;
    return {};
}

Result<std::string> AdbClient::serverVersion() {
    const std::vector<std::string> args{"version"};
    EM_TRY(out, transport_->host(args, kAdbQuickTimeout));
    if (!out.ok()) {
        return makeError(ErrorKind::ProcessFailed, "adb version failed", out.exitCode);
    }
    const auto lines = splitLines(out.out);
    return lines.empty() ? std::string{} : std::string{trim(lines.front())};
}

Result<DeviceList> AdbClient::listDevices() {
    const std::vector<std::string> args{"devices", "-l"};
    auto result = transport_->host(args, kAdbQuickTimeout);
    if (!result) {
        DeviceList empty;
        empty.serverReachable = false;
        empty.lastError = result.error().message;
        return empty;
    }

    const CommandOutput out = std::move(result).value();
    if (out.timedOut) {
        DeviceList empty;
        empty.serverReachable = false;
        empty.lastError = "adb devices timed out";
        return empty;
    }
    if (out.exitCode != 0) {
        DeviceList empty;
        empty.serverReachable = false;
        empty.lastError = std::string{out.firstErrLine()};
        return empty;
    }
    return parseDevicesOutput(out.out);
}

Status AdbClient::connectNetworkDevice(std::string_view hostPort) {
    const std::vector<std::string> args{"connect", std::string{hostPort}};
    EM_TRY(out, transport_->host(args, kAdbNormalTimeout));
    // `adb connect` exits 0 even when it failed; the message is the truth.
    if (containsIgnoreCase(out.out, "unable to connect") ||
        containsIgnoreCase(out.out, "failed to connect")) {
        return makeError(ErrorKind::Io, std::string{trim(out.out)});
    }
    return {};
}

Status AdbClient::disconnectNetworkDevice(std::string_view hostPort) {
    const std::vector<std::string> args{"disconnect", std::string{hostPort}};
    EM_TRY(out, transport_->host(args, kAdbNormalTimeout));
    (void)out;
    return {};
}

Result<CommandOutput> AdbClient::shellRaw(std::string_view serial, std::string_view command,
                                          std::chrono::milliseconds timeout) {
    return transport_->shell(serial, command, timeout);
}

Result<std::string> AdbClient::shell(std::string_view serial, std::string_view command,
                                     std::chrono::milliseconds timeout) {
    EM_TRY(out, transport_->shell(serial, command, timeout));
    if (out.timedOut) {
        return makeError(ErrorKind::Timeout, "shell timed out: " + std::string{command});
    }
    if (!out.err.empty()) {
        EM_LOG_DEBUG(kCategory, "stderr from '" + std::string{command} +
                                    "': " + std::string{out.firstErrLine()});
    }
    return std::move(out.out);
}

Result<std::string> AdbClient::shellChecked(std::string_view serial, std::string_view command,
                                            std::chrono::milliseconds timeout) {
    EM_TRY(out, transport_->shell(serial, command, timeout));
    if (out.timedOut) {
        return makeError(ErrorKind::Timeout, "shell timed out: " + std::string{command});
    }
    if (out.exitCode != 0) {
        const std::string detail =
            out.err.empty() ? std::string{trim(out.out)} : std::string{out.firstErrLine()};
        const ErrorKind kind = containsIgnoreCase(detail, "permission denied")
                                   ? ErrorKind::PermissionDenied
                                   : ErrorKind::ProcessFailed;
        return makeError(kind, std::string{command} + ": " + detail, out.exitCode);
    }
    return std::move(out.out);
}

Result<std::vector<std::string>> AdbClient::shellBatch(std::string_view serial,
                                                       std::span<const std::string> commands,
                                                       std::chrono::milliseconds timeout) {
    if (commands.empty()) return std::vector<std::string>{};

    std::string script;
    script.reserve(commands.size() * 64);
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (i > 0) {
            script += "echo ";
            script += kBatchSentinel;
            script += "; ";
        }
        // `{ ...; }` keeps a command with its own redirections from swallowing
        // the following echo, and 2>/dev/null stops device stderr from
        // interleaving into the sentinel stream.
        script += "{ ";
        script += commands[i];
        script += " ; } 2>/dev/null; ";
    }

    EM_TRY(out, transport_->shell(serial, script, timeout));
    if (out.timedOut) return makeError(ErrorKind::Timeout, "shell batch timed out");

    return splitBatchOutput(out.out, kBatchSentinel, commands.size());
}

Result<PropertyMap> AdbClient::properties(std::string_view serial) {
    EM_TRY(text, shell(serial, "getprop", kAdbNormalTimeout));
    auto props = parseGetpropOutput(text);
    if (props.empty()) {
        return makeError(ErrorKind::ParseFailure, "getprop returned nothing parseable");
    }
    return props;
}

Result<std::vector<std::string>> AdbClient::packages(std::string_view serial,
                                                     bool thirdPartyOnly) {
    const std::string cmd = thirdPartyOnly ? "pm list packages -3" : "pm list packages";
    EM_TRY(text, shell(serial, cmd, kAdbNormalTimeout));

    std::vector<std::string> out;
    for (const auto line : splitLines(text)) {
        if (const auto name = stripPrefix(trim(line), "package:")) {
            if (!name->empty()) out.emplace_back(*name);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

// Pulls "com.example.app" out of a line containing "com.example.app/.MainActivity".
std::optional<std::string> extractComponentPackage(std::string_view text) {
    for (const auto line : splitLines(text)) {
        const auto slash = line.find('/');
        if (slash == std::string_view::npos) continue;

        // Walk back to the start of the component token.
        std::size_t start = slash;
        while (start > 0) {
            const char c = line[start - 1];
            const bool partOfName = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                    (c >= '0' && c <= '9') || c == '.' || c == '_';
            if (!partOfName) break;
            --start;
        }
        const std::string_view candidate = line.substr(start, slash - start);
        if (candidate.find('.') != std::string_view::npos) {
            return std::string{candidate};
        }
    }
    return std::nullopt;
}

}  // namespace

Result<std::string> AdbClient::foregroundPackage(std::string_view serial) {
    // Field names differ across releases, so try the activity dump first and
    // fall back to the window dump.
    EM_TRY(activityDump,
           shell(serial,
                 "dumpsys activity activities | grep -E 'topResumedActivity|mResumedActivity'",
                 kAdbNormalTimeout));
    if (const auto pkg = extractComponentPackage(activityDump)) return *pkg;

    EM_TRY(windowDump, shell(serial, "dumpsys window | grep -E 'mCurrentFocus|mFocusedApp'",
                             kAdbNormalTimeout));
    if (const auto pkg = extractComponentPackage(windowDump)) return *pkg;

    return makeError(ErrorKind::NotFound, "could not determine foreground package");
}

Result<std::vector<int>> AdbClient::pidsOf(std::string_view serial,
                                           std::string_view packageName) {
    EM_TRY(text, shell(serial, "pidof " + std::string{packageName}, kAdbQuickTimeout));

    std::vector<int> pids;
    for (const auto token : tokenize(text)) {
        if (const auto pid = parseNumber<int>(token)) pids.push_back(*pid);
    }
    if (pids.empty()) {
        return makeError(ErrorKind::NotFound,
                         "no running process for " + std::string{packageName});
    }
    return pids;
}

Result<bool> AdbClient::hasRoot(std::string_view serial) {
    EM_TRY(text, shell(serial, "id -u", kAdbQuickTimeout));
    const auto uid = parseNumber<int>(trim(text));
    return uid.has_value() && *uid == 0;
}

Status AdbClient::pull(std::string_view serial, std::string_view remotePath,
                       const std::filesystem::path& localPath,
                       std::chrono::milliseconds timeout) {
    const std::vector<std::string> args{"pull", std::string{remotePath}, localPath.string()};
    EM_TRY(out, transport_->device(serial, args, timeout));
    if (!out.ok()) {
        const std::string detail =
            out.err.empty() ? std::string{trim(out.out)} : std::string{out.firstErrLine()};
        return makeError(containsIgnoreCase(detail, "permission denied")
                             ? ErrorKind::PermissionDenied
                             : ErrorKind::Io,
                         "pull " + std::string{remotePath} + ": " + detail, out.exitCode);
    }
    return {};
}

Status AdbClient::push(std::string_view serial, const std::filesystem::path& localPath,
                       std::string_view remotePath, std::chrono::milliseconds timeout) {
    const std::vector<std::string> args{"push", localPath.string(), std::string{remotePath}};
    EM_TRY(out, transport_->device(serial, args, timeout));
    if (!out.ok()) {
        const std::string detail =
            out.err.empty() ? std::string{trim(out.out)} : std::string{out.firstErrLine()};
        return makeError(containsIgnoreCase(detail, "permission denied")
                             ? ErrorKind::PermissionDenied
                             : ErrorKind::Io,
                         "push to " + std::string{remotePath} + ": " + detail, out.exitCode);
    }
    return {};
}

}  // namespace em
