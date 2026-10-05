#include "collect/LogcatReader.h"

#include <algorithm>
#include <array>
#include <utility>

#include "core/Log.h"
#include "core/StringUtil.h"

namespace em {
namespace {

constexpr const char* kCategory = "logcat";

// How often to re-check the package's pids while the stream is idle.
constexpr auto kPidRecheckInterval = std::chrono::milliseconds{2000};

// Short enough that option changes and pid churn are picked up promptly,
// long enough that an idle device does not spin the reader thread.
constexpr int kReadTimeoutMs = 200;

constexpr std::size_t kReadChunk = 64 * 1024;

// A stream that survives this long was healthy; ending after that is a fresh
// incident rather than part of a failure streak.
constexpr auto kHealthyStreamLifetime = std::chrono::seconds{3};

std::string humanMillis(std::chrono::milliseconds ms) {
    return ms.count() < 1000 ? std::to_string(ms.count()) + " ms"
                             : format("%.1f s", static_cast<double>(ms.count()) / 1000.0);
}

}  // namespace

std::chrono::milliseconds logcatRetryDelay(int consecutiveFailures) noexcept {
    if (consecutiveFailures <= 0) return std::chrono::milliseconds{0};
    // 0.5 s, 1 s, 2 s, 4 s, 8 s, then 10 s for as long as it keeps failing.
    const int doublings = consecutiveFailures - 1 > 5 ? 5 : consecutiveFailures - 1;
    const auto delay = std::chrono::milliseconds{500} * (1 << doublings);
    return delay > std::chrono::seconds{10} ? std::chrono::milliseconds{10'000} : delay;
}

LogcatLevel parseLogcatLevel(char code) noexcept {
    switch (code) {
        case 'V': case 'v': return LogcatLevel::Verbose;
        case 'D': case 'd': return LogcatLevel::Debug;
        case 'I': case 'i': return LogcatLevel::Info;
        case 'W': case 'w': return LogcatLevel::Warn;
        case 'E': case 'e': return LogcatLevel::Error;
        case 'F': case 'f': return LogcatLevel::Fatal;
        case 'S': case 's': return LogcatLevel::Silent;
        default: break;
    }
    return LogcatLevel::Unknown;
}

std::string_view toString(LogcatLevel level) noexcept {
    switch (level) {
        case LogcatLevel::Verbose: return "VERBOSE";
        case LogcatLevel::Debug:   return "DEBUG";
        case LogcatLevel::Info:    return "INFO";
        case LogcatLevel::Warn:    return "WARN";
        case LogcatLevel::Error:   return "ERROR";
        case LogcatLevel::Fatal:   return "FATAL";
        case LogcatLevel::Silent:  return "SILENT";
        case LogcatLevel::Unknown: break;
    }
    return "?";
}

char toChar(LogcatLevel level) noexcept {
    switch (level) {
        case LogcatLevel::Verbose: return 'V';
        case LogcatLevel::Debug:   return 'D';
        case LogcatLevel::Info:    return 'I';
        case LogcatLevel::Warn:    return 'W';
        case LogcatLevel::Error:   return 'E';
        case LogcatLevel::Fatal:   return 'F';
        case LogcatLevel::Silent:  return 'S';
        case LogcatLevel::Unknown: break;
    }
    return '?';
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

std::vector<std::string> buildLogcatArgs(const LogcatOptions& options,
                                         const std::vector<int>& pids) {
    std::vector<std::string> args;
    args.emplace_back("logcat");

    // threadtime is the one long-stable format that carries pid, tid and tag
    // on a single line, which is what makes host-side filtering possible.
    args.emplace_back("-v");
    args.emplace_back("threadtime");

    for (const auto& buffer : options.buffers) {
        args.emplace_back("-b");
        args.push_back(buffer);
    }

    if (options.backlogLines > 0) {
        args.emplace_back("-T");
        args.push_back(std::to_string(options.backlogLines));
    }

    // logcat accepts a single --pid, so this only applies when the package
    // resolved to exactly one process. Anything else is filtered host side.
    if (options.scope == LogcatScope::SelectedProcess && options.deviceSideFilter &&
        pids.size() == 1) {
        args.push_back("--pid=" + std::to_string(pids.front()));
    }

    if (options.deviceMinLevel != LogcatLevel::Verbose &&
        options.deviceMinLevel != LogcatLevel::Unknown) {
        args.push_back(std::string{"*:"} + toChar(options.deviceMinLevel));
    }
    return args;
}

LogcatEntry parseLogcatLine(std::string_view line, double hostTime) {
    LogcatEntry entry;
    entry.hostTime = hostTime;

    const std::string_view trimmed = trimRight(line);
    if (trimmed.empty()) {
        entry.raw = true;
        return entry;
    }

    // "--------- beginning of main" and similar markers.
    if (trimmed.starts_with("---------")) {
        entry.raw = true;
        entry.message = std::string{trim(trimmed)};
        return entry;
    }

    // Expected: DATE TIME PID TID LEVEL TAG: message
    // The tag may contain spaces, so the fields are taken positionally up to
    // the level and the tag runs to the first ": " after it.
    const auto fields = tokenize(trimmed);
    if (fields.size() < 6) {
        entry.raw = true;
        entry.message = std::string{trimmed};
        return entry;
    }

    const auto pid = parseNumber<int>(fields[2]);
    const auto tid = parseNumber<int>(fields[3]);
    if (!pid || !tid || fields[4].size() != 1) {
        entry.raw = true;
        entry.message = std::string{trimmed};
        return entry;
    }

    const LogcatLevel level = parseLogcatLevel(fields[4].front());
    if (level == LogcatLevel::Unknown) {
        entry.raw = true;
        entry.message = std::string{trimmed};
        return entry;
    }

    entry.deviceTime = std::string{fields[0]} + ' ' + std::string{fields[1]};
    entry.pid = *pid;
    entry.tid = *tid;
    entry.level = level;

    // Everything after the level character: " TAG: message".
    const auto levelOffset = static_cast<std::size_t>(fields[4].data() - trimmed.data());
    std::string_view rest = trimmed.substr(levelOffset + 1);

    const auto separator = rest.find(": ");
    if (separator == std::string_view::npos) {
        // Tag with no message, or a format we do not recognise past this point.
        entry.tag = std::string{trim(rest)};
        if (!entry.tag.empty() && entry.tag.back() == ':') entry.tag.pop_back();
        return entry;
    }

    entry.tag = std::string{trim(rest.substr(0, separator))};
    entry.message = std::string{rest.substr(separator + 2)};
    return entry;
}

void drainCompleteLines(std::string& buffer, std::vector<std::string>& lines) {
    std::size_t start = 0;
    for (;;) {
        const auto newline = buffer.find('\n', start);
        if (newline == std::string::npos) break;

        std::size_t end = newline;
        if (end > start && buffer[end - 1] == '\r') --end;  // Windows adb
        lines.emplace_back(buffer, start, end - start);
        start = newline + 1;
    }
    if (start > 0) buffer.erase(0, start);
}

// ---------------------------------------------------------------------------
// LogcatFilter
// ---------------------------------------------------------------------------

bool LogcatFilter::accepts(const LogcatEntry& entry, const LogcatSnapshot& snapshot) const {
    // Separator lines and anything that did not parse always pass. Hiding them
    // would make a native crash -- whose tombstone does not follow threadtime
    // at all -- look like the log simply stopped.
    if (entry.raw) return true;

    if (static_cast<int>(entry.level) < static_cast<int>(minLevel)) return false;

    if (onlyTrackedPids && scope == LogcatScope::SelectedProcess) {
        // No resolved pid means the package is not running, so nothing can
        // legitimately be attributed to it.
        if (snapshot.trackedPids.empty()) return false;
        if (!snapshot.tracksPid(entry.pid)) return false;
    }

    if (!tag.empty() && !containsIgnoreCase(entry.tag, tag)) return false;

    if (!text.empty()) {
        const bool hit =
            containsIgnoreCase(entry.message, text) || containsIgnoreCase(entry.tag, text);
        if (hit == excludeText) return false;
    }
    return true;
}

std::string LogcatFilter::signature(const LogcatSnapshot& snapshot) const {
    std::string out;
    out.reserve(tag.size() + text.size() + 48);
    out += static_cast<char>('0' + static_cast<int>(minLevel));
    out += excludeText ? '1' : '0';
    out += onlyTrackedPids ? '1' : '0';
    out += scope == LogcatScope::SelectedProcess ? 'S' : 'A';
    out += '|';
    out += tag;
    out += '|';
    out += text;
    out += '|';
    // An app restart changes the pid set, which changes what passes even
    // though nothing the user typed moved.
    for (const int pid : snapshot.trackedPids) {
        out += std::to_string(pid);
        out += ',';
    }
    return out;
}

// ---------------------------------------------------------------------------
// LogcatReader
// ---------------------------------------------------------------------------

LogcatReader::LogcatReader(DeviceManager& devices) : devices_(devices) {
    startTime_ = std::chrono::steady_clock::now();
}

LogcatReader::~LogcatReader() { stop(); }

void LogcatReader::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    startTime_ = std::chrono::steady_clock::now();
    thread_ = std::thread([this] { readerLoop(); });
}

void LogcatReader::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    sleepCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    snapshot_.mutate([](LogcatSnapshot& s) { s.streaming = false; });
}

void LogcatReader::setOptions(LogcatOptions newOptions) {
    bool changed = false;
    {
        const std::lock_guard lock{optionsMutex_};
        changed = !(options_ == newOptions);
        options_ = std::move(newOptions);
    }
    // Only an argv-affecting change needs the stream rebuilt; the panel's own
    // filters are host side and never reach here.
    if (changed) requestRestart();
}

LogcatOptions LogcatReader::options() const {
    const std::lock_guard lock{optionsMutex_};
    return options_;
}

void LogcatReader::setCapacity(std::size_t lines) {
    capacity_.store(std::clamp<std::size_t>(lines, 500, 500'000), std::memory_order_release);
}

void LogcatReader::clearBuffer() {
    snapshot_.mutate([](LogcatSnapshot& s) {
        s.entries.clear();
        s.totalReceived = 0;
        s.droppedToCap = 0;
        s.parseFailures = 0;
    });
}

void LogcatReader::clearDeviceBuffer(ThreadPool& pool) {
    const std::string serial = devices_.selectedSerial();
    if (serial.empty()) return;

    // Suspend the reader across the clear: `logcat -c` while another logcat is
    // attached to the same buffer is unreliable on some devices.
    suspended_.store(true, std::memory_order_release);
    requestRestart();

    pool.submit([this, serial] {
        const std::vector<std::string> args{"logcat", "-c"};
        if (auto result = devices_.adb().transport().device(serial, args, kAdbNormalTimeout);
            !result) {
            EM_LOG_WARN(kCategory, "logcat -c failed: " + result.describe());
        } else {
            EM_LOG_INFO(kCategory, "cleared the device log buffer");
        }
        clearBuffer();
        suspended_.store(false, std::memory_order_release);
        requestRestart();
    });
}

void LogcatReader::requestRestart() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    sleepCv_.notify_all();
}

std::vector<int> LogcatReader::resolvePids(std::string_view serial,
                                           std::string_view packageName) {
    if (packageName.empty()) return {};
    auto result = devices_.adb().pidsOf(serial, packageName);
    if (!result) return {};
    auto pids = std::move(result).value();
    std::sort(pids.begin(), pids.end());
    return pids;
}

void LogcatReader::appendBatch(std::vector<LogcatEntry>&& batch, std::size_t failures) {
    if (batch.empty() && failures == 0) return;

    const std::size_t cap = capacity_.load(std::memory_order_acquire);
    snapshot_.mutate([&batch, failures, cap](LogcatSnapshot& s) {
        s.totalReceived += batch.size();
        s.parseFailures += failures;

        for (auto& entry : batch) s.entries.push_back(std::move(entry));

        if (s.entries.size() > cap) {
            const std::size_t excess = s.entries.size() - cap;
            s.entries.erase(s.entries.begin(),
                            s.entries.begin() + static_cast<std::ptrdiff_t>(excess));
            s.droppedToCap += excess;
        }
    });
}

void LogcatReader::readerLoop() {
    std::unique_ptr<Subprocess> stream;
    std::string pending;                 // partial line carried between reads
    std::vector<std::string> lines;
    std::vector<LogcatEntry> batch;
    std::array<char, kReadChunk> buffer{};

    std::string activeSerial;
    std::string activePackage;
    std::vector<int> activePids;
    std::uint64_t activeGeneration = 0;
    auto lastPidCheck = std::chrono::steady_clock::now() - kPidRecheckInterval;

    // Backoff. Without it a logcat that exits immediately -- unsupported on the
    // device, a permission problem, an unplug race -- was respawned in a tight
    // loop: hundreds of processes a second, and a log flooded with
    // "streaming ..." until it evicted every other entry.
    int consecutiveFailures = 0;
    auto streamStartedAt = std::chrono::steady_clock::time_point{};
    auto nextAttemptAt = std::chrono::steady_clock::time_point{};

    const auto teardown = [&] {
        if (stream) {
            stream->terminate();
            stream.reset();
        }
        pending.clear();
        snapshot_.mutate([](LogcatSnapshot& s) { s.streaming = false; });
    };

    // Every way a running stream can end comes through here.
    const auto endStream = [&](const std::string& why) {
        teardown();
        const auto now = std::chrono::steady_clock::now();
        const auto lived =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - streamStartedAt);

        consecutiveFailures = lived < kHealthyStreamLifetime ? consecutiveFailures + 1 : 1;
        const auto delay = logcatRetryDelay(consecutiveFailures);
        nextAttemptAt = now + delay;

        const std::string message = "logcat " + why + " after " + humanMillis(lived) +
                                    "; retrying in " + humanMillis(delay);
        snapshot_.mutate([&message](LogcatSnapshot& s) { s.lastError = message; });
        // One warning per streak; the rest would only bury the log again.
        if (consecutiveFailures == 1) {
            EM_LOG_WARN(kCategory, message);
        } else {
            EM_LOG_DEBUG(kCategory, message);
        }
    };

    while (running_.load(std::memory_order_acquire)) {
        const std::string serial = devices_.selectedSerial();
        const std::string packageName = devices_.selectedPackage();
        const std::uint64_t generation = generation_.load(std::memory_order_acquire);
        const LogcatOptions opts = options();
        const bool suspended = suspended_.load(std::memory_order_acquire);

        const bool usable = !serial.empty() && devices_.hasUsableSelection() && !suspended;

        // The stream died: device unplugged, or logcat exited.
        if (stream) {
            if (const auto code = stream->tryWait()) {
                endStream("exited with code " + std::to_string(*code));
            }
        }

        const auto now = std::chrono::steady_clock::now();

        // A stream that has stayed up ends any failure streak.
        if (stream && consecutiveFailures > 0 && now - streamStartedAt >= kHealthyStreamLifetime) {
            consecutiveFailures = 0;
            snapshot_.mutate([](LogcatSnapshot& s) { s.lastError.clear(); });
        }

        // Something the user changed: retry at once rather than waiting out a
        // backoff earned by the previous configuration.
        const bool changed = serial != activeSerial || packageName != activePackage ||
                             generation != activeGeneration;
        if (changed) {
            consecutiveFailures = 0;
            nextAttemptAt = {};
        }

        bool restart = usable && (changed || !stream) && now >= nextAttemptAt;

        // An app restart changes the pid, which invalidates a device-side
        // --pid filter and the host-side pid set alike.
        if (usable && !restart && opts.scope == LogcatScope::SelectedProcess &&
            !packageName.empty() && now - lastPidCheck >= kPidRecheckInterval) {
            lastPidCheck = now;
            auto pids = resolvePids(serial, packageName);
            if (pids != activePids) {
                const bool devicePidChanged =
                    opts.deviceSideFilter &&
                    (pids.size() == 1 || activePids.size() == 1);
                activePids = std::move(pids);
                snapshot_.mutate([&activePids](LogcatSnapshot& s) {
                    s.trackedPids = activePids;
                });
                // Only the device-side filter needs a new process; host-side
                // filtering just picks up the new pid set on the next frame.
                if (devicePidChanged) restart = true;
            }
        }

        if (!usable && stream) teardown();

        if (restart) {
            teardown();

            activeSerial = serial;
            activePackage = packageName;
            activeGeneration = generation;
            if (opts.scope == LogcatScope::SelectedProcess && !packageName.empty()) {
                activePids = resolvePids(serial, packageName);
                lastPidCheck = std::chrono::steady_clock::now();
            } else {
                activePids.clear();
            }

            const auto args = buildLogcatArgs(opts, activePids);
            auto spawned = devices_.adb().transport().stream(serial, args, /*pipeStdin=*/false);

            if (!spawned) {
                const std::string message = spawned.error().message;
                snapshot_.mutate([&message](LogcatSnapshot& s) {
                    s.lastError = message;
                    s.streaming = false;
                });
                EM_LOG_ERROR(kCategory, "could not start logcat: " + message);
                ++consecutiveFailures;
                nextAttemptAt = std::chrono::steady_clock::now() + logcatRetryDelay(consecutiveFailures);
            } else {
                stream = std::move(spawned).value();
                streamStartedAt = std::chrono::steady_clock::now();
                const bool deviceFilter =
                    opts.scope == LogcatScope::SelectedProcess && opts.deviceSideFilter &&
                    activePids.size() == 1;

                snapshot_.mutate([&](LogcatSnapshot& s) {
                    s.streaming = true;
                    s.lastError.clear();
                    s.commandLine = "adb -s " + serial + " " + quoteArgv(args);
                    s.trackedPackage = packageName;
                    s.trackedPids = activePids;
                    s.usingDeviceSideFilter = deviceFilter;
                });
                if (consecutiveFailures == 0) {
                    EM_LOG_INFO(kCategory, "streaming " + quoteArgv(args));
                } else {
                    EM_LOG_DEBUG(kCategory, "retrying " + quoteArgv(args));
                }
            }
        }

        if (!stream) {
            std::unique_lock lock{sleepMutex_};
            sleepCv_.wait_for(lock, std::chrono::milliseconds{400}, [this] {
                return !running_.load(std::memory_order_acquire);
            });
            continue;
        }

        // --- drain ---------------------------------------------------------
        bool timedOut = false;
        auto count = stream->readStdout(buffer, kReadTimeoutMs, &timedOut);
        if (!count) {
            endStream("read failed (" + count.error().message + ")");
            continue;
        }
        if (*count == 0) {
            if (!timedOut) endStream("closed its output");  // EOF
            continue;
        }

        pending.append(buffer.data(), *count);
        lines.clear();
        drainCompleteLines(pending, lines);

        // A single unterminated line should not grow without bound if the
        // device emits something pathological.
        if (pending.size() > 1024 * 1024) pending.clear();

        if (lines.empty()) continue;

        const double hostTime =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime_)
                .count();

        batch.clear();
        batch.reserve(lines.size());
        std::size_t failures = 0;
        for (const auto& line : lines) {
            LogcatEntry entry = parseLogcatLine(line, hostTime);
            if (entry.raw && !line.empty() && !line.starts_with("---------")) ++failures;
            batch.push_back(std::move(entry));
        }
        appendBatch(std::move(batch), failures);
    }

    teardown();
    EM_LOG_DEBUG(kCategory, "logcat reader thread exiting");
}

}  // namespace em
