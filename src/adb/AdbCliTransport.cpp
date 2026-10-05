#include "adb/AdbCliTransport.h"

#include <cstdlib>

#include "core/Log.h"
#include "core/StringUtil.h"

namespace em {
namespace {

namespace fs = std::filesystem;

constexpr const char* kCategory = "adb";

#if defined(_WIN32)
constexpr const char* kAdbExe = "adb.exe";
#else
constexpr const char* kAdbExe = "adb";
#endif

std::optional<fs::path> envPath(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return fs::path{value};
}

bool isExecutableFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) || fs::is_symlink(p, ec);
}

}  // namespace

std::vector<fs::path> AdbCliTransport::searchCandidates() {
    std::vector<fs::path> out;
    const auto addSdkRoot = [&out](const fs::path& root) {
        out.push_back(root / "platform-tools" / kAdbExe);
    };

    if (const auto p = envPath("EMERON_ADB")) out.push_back(*p);
    if (const auto p = envPath("ANDROID_HOME")) addSdkRoot(*p);
    if (const auto p = envPath("ANDROID_SDK_ROOT")) addSdkRoot(*p);

    if (const auto home = envPath("HOME")) {
#if defined(__APPLE__)
        addSdkRoot(*home / "Library" / "Android" / "sdk");
#endif
        addSdkRoot(*home / "Android" / "Sdk");
        addSdkRoot(*home / "android-sdk");
    }
    if (const auto local = envPath("LOCALAPPDATA")) {
        addSdkRoot(*local / "Android" / "Sdk");
    }

    out.emplace_back("/usr/local/bin/adb");
    out.emplace_back("/opt/homebrew/bin/adb");
    out.emplace_back("/usr/bin/adb");
    return out;
}

AdbCliTransport::AdbCliTransport(std::optional<fs::path> explicitPath) {
    relocate(std::move(explicitPath));
}

void AdbCliTransport::relocate(std::optional<fs::path> explicitPath) {
    const std::lock_guard lock{mutex_};
    binary_.reset();
    origin_.clear();

    if (explicitPath && isExecutableFile(*explicitPath)) {
        binary_ = *explicitPath;
        origin_ = "explicit path";
    }

    // EMERON_ADB is documented as equivalent to --adb, so it has to outrank
    // PATH. It used to be checked after PATH, which silently ignored it on any
    // machine that already had an adb installed.
    if (!binary_) {
        if (const auto env = envPath("EMERON_ADB"); env && isExecutableFile(*env)) {
            binary_ = *env;
            origin_ = "EMERON_ADB";
        }
    }

    // Then PATH: it is what the user's own shell would resolve.
    if (!binary_) {
        if (auto found = findOnPath(kAdbExe)) {
            binary_ = *found;
            origin_ = "PATH";
        }
    }

    if (!binary_) {
        for (const auto& candidate : searchCandidates()) {
            if (isExecutableFile(candidate)) {
                binary_ = candidate;
                origin_ = "SDK location";
                break;
            }
        }
    }

    if (binary_) {
        EM_LOG_INFO(kCategory, "using adb at " + binary_->string() + " (" + origin_ + ")");
    } else {
        EM_LOG_WARN(kCategory,
                    "adb not found; set EMERON_ADB or ANDROID_HOME, or put adb on PATH");
    }
}

bool AdbCliTransport::available() const {
    const std::lock_guard lock{mutex_};
    return binary_.has_value();
}

std::string AdbCliTransport::describe() const {
    const std::lock_guard lock{mutex_};
    if (!binary_) return "adb not found";
    return binary_->string() + " [" + origin_ + "]";
}

std::optional<fs::path> AdbCliTransport::binaryPath() const {
    const std::lock_guard lock{mutex_};
    return binary_;
}

std::vector<std::string> AdbCliTransport::buildArgv(std::string_view serial,
                                                    std::span<const std::string> args) const {
    std::vector<std::string> argv;
    argv.reserve(args.size() + 3);
    {
        const std::lock_guard lock{mutex_};
        argv.push_back(binary_ ? binary_->string() : std::string{kAdbExe});
    }
    if (!serial.empty()) {
        argv.emplace_back("-s");
        argv.emplace_back(serial);
    }
    for (const auto& a : args) argv.push_back(a);
    return argv;
}

Result<CommandOutput> AdbCliTransport::host(std::span<const std::string> args,
                                            std::chrono::milliseconds timeout) {
    if (!available()) {
        return makeError(ErrorKind::NotFound, "adb executable not found");
    }
    return runCommand(buildArgv({}, args), timeout);
}

Result<CommandOutput> AdbCliTransport::device(std::string_view serial,
                                              std::span<const std::string> args,
                                              std::chrono::milliseconds timeout) {
    if (!available()) {
        return makeError(ErrorKind::NotFound, "adb executable not found");
    }
    if (serial.empty()) {
        return makeError(ErrorKind::Unsupported, "no device selected");
    }
    return runCommand(buildArgv(serial, args), timeout);
}

Result<CommandOutput> AdbCliTransport::shell(std::string_view serial, std::string_view command,
                                             std::chrono::milliseconds timeout) {
    // Pass the command as a single argv element. adb concatenates its shell
    // arguments with spaces before handing them to the device's sh, so sending
    // one pre-composed string is the only way to keep quoting predictable.
    const std::vector<std::string> args{"shell", std::string{command}};
    return device(serial, args, timeout);
}

Result<std::unique_ptr<Subprocess>> AdbCliTransport::stream(std::string_view serial,
                                                            std::span<const std::string> args,
                                                            bool pipeStdin) {
    if (!available()) {
        return makeError(ErrorKind::NotFound, "adb executable not found");
    }

    Subprocess::Options options;
    options.argv = buildArgv(serial, args);
    options.pipeStdin = pipeStdin;

    EM_LOG_DEBUG(kCategory, "stream: " + quoteArgv(options.argv));

    auto spawned = Subprocess::spawn(std::move(options));
    if (!spawned) return std::move(spawned).error();
    return std::make_unique<Subprocess>(std::move(spawned).value());
}

}  // namespace em
