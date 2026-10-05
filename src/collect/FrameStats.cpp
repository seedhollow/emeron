#include "collect/FrameStats.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/Log.h"
#include "core/RingBuffer.h"
#include "core/StringUtil.h"

namespace em {
namespace {

constexpr const char* kCategory = "frames";
constexpr std::string_view kMarker = "---PROFILEDATA---";
constexpr double kNsToMs = 1.0 / 1e6;

// Reads one column, returning 0 when the column is absent on this release.
std::uint64_t column(const std::vector<std::uint64_t>& row, const ColumnIndex& index,
                     std::string_view name) {
    const auto it = index.find(std::string{name});
    if (it == index.end() || it->second >= row.size()) return 0;
    return row[it->second];
}

// Milliseconds between two timestamp columns; -1 if either is missing or the
// pair is out of order (which happens for skipped phases).
double phaseMs(const std::vector<std::uint64_t>& row, const ColumnIndex& index,
               std::string_view fromName, std::string_view toName) {
    const std::uint64_t from = column(row, index, fromName);
    const std::uint64_t to = column(row, index, toName);
    if (from == 0 || to == 0 || to < from) return -1.0;
    return static_cast<double>(to - from) * kNsToMs;
}

long parseLongField(std::string_view line, std::string_view key) {
    const auto value = fieldAfter(line, key);
    if (!value) return -1;
    const auto parsed = parseNumber<long>(*value);
    return parsed.value_or(-1);
}

}  // namespace

std::vector<ProfileBlock> extractProfileBlocks(std::string_view dump) {
    std::vector<ProfileBlock> blocks;
    std::size_t cursor = 0;
    while (true) {
        const auto open = dump.find(kMarker, cursor);
        if (open == std::string_view::npos) break;
        const std::size_t bodyStart = open + kMarker.size();
        const auto close = dump.find(kMarker, bodyStart);
        const std::size_t bodyEnd = close == std::string_view::npos ? dump.size() : close;

        // The window this block belongs to is the last "Window:" line between
        // the previous block and this one.
        ProfileBlock block;
        const std::string_view preamble = dump.substr(cursor, open - cursor);
        for (const auto line : splitLines(preamble)) {
            if (const auto name = stripPrefix(trim(line), "Window:")) block.window = trim(*name);
        }
        block.body = trim(dump.substr(bodyStart, bodyEnd - bodyStart));
        blocks.push_back(block);

        if (close == std::string_view::npos) break;
        cursor = close + kMarker.size();
    }
    return blocks;
}

std::optional<std::string_view> extractProfileData(std::string_view dump) {
    const auto first = dump.find(kMarker);
    if (first == std::string_view::npos) return std::nullopt;

    const std::size_t bodyStart = first + kMarker.size();
    const auto second = dump.find(kMarker, bodyStart);
    const std::size_t bodyEnd = (second == std::string_view::npos) ? dump.size() : second;
    return trim(dump.substr(bodyStart, bodyEnd - bodyStart));
}

ColumnIndex parseFrameStatsHeader(std::string_view headerLine) {
    ColumnIndex index;
    const auto names = split(trim(headerLine), ',', true);
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string_view name = trim(names[i]);
        if (!name.empty()) index.emplace(std::string{name}, i);
    }
    return index;
}

Result<ParsedFrameStats> parseFrameStats(std::string_view dump, double refreshPeriodMs) {
    const auto blocks = extractProfileBlocks(dump);
    if (blocks.empty()) {
        return makeError(ErrorKind::ParseFailure,
                         "no ---PROFILEDATA--- block; is the package in the foreground "
                         "and hardware accelerated?");
    }

    ParsedFrameStats parsed;
    std::vector<std::uint64_t> row;
    std::vector<double> intervalsMs;
    std::size_t dataRows = 0;
    std::string headerProblem;

    for (const auto& block : blocks) {
        const auto lines = splitLines(block.body);
        if (lines.size() < 2) continue;  // a window that has not drawn yet

        // The header is the first line that is not itself numeric.
        std::size_t headerLine = 0;
        while (headerLine < lines.size() &&
               (trim(lines[headerLine]).empty() ||
                parseNumber<std::int64_t>(split(lines[headerLine], ',', true).front()))) {
            ++headerLine;
        }
        if (headerLine >= lines.size()) {
            headerProblem = "could not locate the framestats header";
            continue;
        }
        const ColumnIndex index = parseFrameStatsHeader(lines[headerLine]);
        if (!index.contains("IntendedVsync") || !index.contains("FrameCompleted")) {
            headerProblem = "framestats header lacks IntendedVsync/FrameCompleted";
            continue;
        }

        if (parsed.columns.empty()) {
            std::vector<std::pair<std::size_t, std::string>> ordered;
            ordered.reserve(index.size());
            for (const auto& [name, position] : index) ordered.emplace_back(position, name);
            std::sort(ordered.begin(), ordered.end());
            for (auto& [position, name] : ordered) parsed.columns.push_back(std::move(name));
        }

        for (std::size_t i = headerLine + 1; i < lines.size(); ++i) {
            const std::string_view line = trim(lines[i]);
            if (line.empty()) continue;
            ++dataRows;

            // Real devices end every line with a comma, and report columns they
            // cannot fill with sentinels such as -2 (DisplayPresentTime before the
            // frame is on screen). Both used to reject the whole row -- every row,
            // on an Android 14 phone -- so a field that is empty, negative or not a
            // number now means "not reported" for that column only. A row is
            // dropped only if the two timestamps every calculation needs are gone.
            row.clear();
            for (const auto field : split(line, ',', true)) {
                const auto value = parseNumber<std::int64_t>(trim(field));
                row.push_back(value && *value > 0 ? static_cast<std::uint64_t>(*value) : 0);
            }
            if (row.size() < 3) continue;

            // The per-frame interval. On some builds (seen on Android 14) the
            // header has "FrameInterval" and "FrameStartTime" the wrong way round,
            // so whichever of the two holds an interval-sized value is used, not
            // whichever is named FrameInterval.
            for (const char* name : {"FrameInterval", "FrameStartTime"}) {
                const auto value = static_cast<double>(column(row, index, name)) * kNsToMs;
                if (value >= 1.0 && value <= 100.0) intervalsMs.push_back(value);
            }

            FrameRecord frame;
            frame.flags = column(row, index, "Flags");
            frame.intendedVsyncNs = column(row, index, "IntendedVsync");
            frame.vsyncNs = column(row, index, "Vsync");
            frame.frameCompletedNs = column(row, index, "FrameCompleted");
            frame.frameDeadlineNs = column(row, index, "FrameDeadline");
            frame.gpuCompletedNs = column(row, index, "GpuCompleted");

            // A zero FrameCompleted means the frame never finished (dropped).
            if (frame.intendedVsyncNs == 0 || frame.frameCompletedNs == 0) continue;
            if (frame.frameCompletedNs < frame.intendedVsyncNs) continue;

            frame.excluded = (frame.flags & kExcludedFrameFlags) != 0;
            frame.window = std::string{block.window};
            frame.totalMs =
                static_cast<double>(frame.frameCompletedNs - frame.intendedVsyncNs) * kNsToMs;

            frame.inputMs     = phaseMs(row, index, "HandleInputStart", "AnimationStart");
            frame.animationMs = phaseMs(row, index, "AnimationStart", "PerformTraversalsStart");
            frame.layoutMs    = phaseMs(row, index, "PerformTraversalsStart", "DrawStart");
            frame.drawMs      = phaseMs(row, index, "DrawStart", "SyncQueued");
            frame.syncMs      = phaseMs(row, index, "SyncStart", "IssueDrawCommandsStart");
            frame.issueMs     = phaseMs(row, index, "IssueDrawCommandsStart", "SwapBuffers");
            frame.swapMs      = phaseMs(row, index, "SwapBuffers", "FrameCompleted");
            frame.gpuMs       = phaseMs(row, index, "SwapBuffers", "GpuCompleted");

            if (const auto dequeue = column(row, index, "DequeueBufferDuration"); dequeue != 0) {
                frame.dequeueMs = static_cast<double>(dequeue) * kNsToMs;
            }
            if (const auto queue = column(row, index, "QueueBufferDuration"); queue != 0) {
                frame.queueMs = static_cast<double>(queue) * kNsToMs;
            }

            parsed.frames.push_back(frame);
        }
    }

    if (parsed.frames.empty() && !headerProblem.empty()) {
        return makeError(ErrorKind::ParseFailure, headerProblem);
    }

    // Sections present but empty: the app has drawn nothing since its counters
    // were last reset (an idle screen). That is a state to wait in, not an
    // error -- reporting it as one, with hints about hardware acceleration,
    // sent a user hunting for a problem their app did not have.
    if (parsed.frames.empty() && dataRows == 0) {
        parsed.refreshPeriodMs = refreshPeriodMs > 0.0 ? refreshPeriodMs : 16.667;
        return parsed;
    }

    if (parsed.frames.empty()) {
        return makeError(ErrorKind::ParseFailure,
                         "no usable frames in PROFILEDATA: none of its " +
                             std::to_string(dataRows) +
                             " rows has both IntendedVsync and FrameCompleted");
    }

    std::sort(parsed.frames.begin(), parsed.frames.end(),
              [](const FrameRecord& a, const FrameRecord& b) {
                  return a.intendedVsyncNs < b.intendedVsyncNs;
              });

    // Refresh period, best source first. FrameDeadline - IntendedVsync is NOT
    // one of them: it includes the pipelining slack Android allows (20 ms on a
    // 120 Hz phone), and an earlier version that used it judged jank against a
    // budget 2.4x too generous.
    if (refreshPeriodMs > 0.0) {
        parsed.refreshPeriodMs = refreshPeriodMs;
    } else if (!intervalsMs.empty()) {
        parsed.refreshPeriodMs = percentileInPlace(intervalsMs, 50.0);
    } else {
        // Older releases have no interval column. Frames rendered back to back
        // land one vsync apart, so the short end of the spacing is the period.
        std::vector<double> spacing;
        for (std::size_t i = 1; i < parsed.frames.size(); ++i) {
            const auto a = parsed.frames[i - 1].intendedVsyncNs;
            const auto b = parsed.frames[i].intendedVsyncNs;
            if (b > a) spacing.push_back(static_cast<double>(b - a) * kNsToMs);
        }
        const double inferred = spacing.empty() ? 0.0 : percentileInPlace(spacing, 10.0);
        parsed.refreshPeriodMs = (inferred >= 4.0 && inferred <= 100.0) ? inferred : 16.667;
    }

    const std::uint64_t base = parsed.frames.front().intendedVsyncNs;
    for (auto& frame : parsed.frames) {
        frame.startSec = static_cast<double>(frame.intendedVsyncNs - base) / 1e9;
        frame.janky = missedDeadline(frame, parsed.refreshPeriodMs);
    }
    return parsed;
}

bool missedDeadline(const FrameRecord& frame, double refreshPeriodMs) noexcept {
    if (frame.excluded) return false;
    // Android 12+: the frame has its own deadline, which already allows for
    // pipelining. This is the rule behind dumpsys gfxinfo's "Janky frames".
    if (frame.frameDeadlineNs > frame.intendedVsyncNs) {
        return frame.frameCompletedNs > frame.frameDeadlineNs;
    }
    return frame.totalMs > refreshPeriodMs;
}

int framesLate(const FrameRecord& frame, double refreshPeriodMs) noexcept {
    const double period = refreshPeriodMs > 0.0 ? refreshPeriodMs : 16.667;
    if (frame.frameDeadlineNs > frame.intendedVsyncNs) {
        if (frame.frameCompletedNs <= frame.frameDeadlineNs) return 0;
        const double lateMs =
            static_cast<double>(frame.frameCompletedNs - frame.frameDeadlineNs) * kNsToMs;
        return 1 + static_cast<int>(lateMs / period);
    }
    if (frame.totalMs <= period) return 0;
    return static_cast<int>(std::ceil(frame.totalMs / period)) - 1;
}

GfxSummary parseGfxSummary(std::string_view dump) {
    GfxSummary summary;

    for (const auto line : splitLines(dump)) {
        const std::string_view t = trim(line);
        if (t.empty()) continue;

        // The process-wide summary comes first; per-window sections with the
        // same field names follow. Reading on let the last window's numbers
        // overwrite the process totals.
        if (t.starts_with("Window:") || t == kMarker) break;
        // "Janky frames (legacy)" and friends share a prefix with the current
        // fields and would otherwise overwrite them.
        if (t.find("(legacy)") != std::string_view::npos) continue;

        if (t.starts_with("** Graphics info for pid")) {
            // ** Graphics info for pid 12345 [com.example.app] **
            if (const auto pidText = fieldAfter(t, "pid")) {
                if (const auto pid = parseNumber<int>(tokenize(*pidText).empty()
                                                          ? std::string_view{}
                                                          : tokenize(*pidText).front())) {
                    summary.pid = *pid;
                }
            }
            const auto open = t.find('[');
            const auto close = t.find(']', open == std::string_view::npos ? 0 : open);
            if (open != std::string_view::npos && close != std::string_view::npos &&
                close > open) {
                summary.packageName = std::string{t.substr(open + 1, close - open - 1)};
            }
            continue;
        }

        if (t.starts_with("Total frames rendered")) {
            summary.totalFrames = parseLongField(t, "Total frames rendered");
        } else if (t.starts_with("Janky frames")) {
            // Janky frames: 25 (5.00%)
            if (const auto value = fieldAfter(t, "Janky frames")) {
                const auto fields = tokenize(*value);
                if (!fields.empty()) {
                    if (const auto count = parseNumber<long>(fields[0])) {
                        summary.jankyFrames = *count;
                    }
                }
                const auto open = value->find('(');
                if (open != std::string_view::npos) {
                    if (const auto pct = parseNumber<double>(value->substr(open + 1))) {
                        summary.jankyPct = *pct;
                    }
                }
            }
        } else if (t.starts_with("50th percentile")) {
            summary.p50Ms = static_cast<double>(parseLongField(t, "50th percentile"));
        } else if (t.starts_with("90th percentile")) {
            summary.p90Ms = static_cast<double>(parseLongField(t, "90th percentile"));
        } else if (t.starts_with("95th percentile")) {
            summary.p95Ms = static_cast<double>(parseLongField(t, "95th percentile"));
        } else if (t.starts_with("99th percentile")) {
            summary.p99Ms = static_cast<double>(parseLongField(t, "99th percentile"));
        } else if (t.starts_with("Number Missed Vsync")) {
            summary.missedVsync = parseLongField(t, "Number Missed Vsync");
        } else if (t.starts_with("Number High input latency")) {
            summary.highInputLatency = parseLongField(t, "Number High input latency");
        } else if (t.starts_with("Number Slow UI thread")) {
            summary.slowUiThread = parseLongField(t, "Number Slow UI thread");
        } else if (t.starts_with("Number Slow bitmap uploads")) {
            summary.slowBitmapUploads = parseLongField(t, "Number Slow bitmap uploads");
        } else if (t.starts_with("Number Slow issue draw commands")) {
            summary.slowDrawCommands = parseLongField(t, "Number Slow issue draw commands");
        } else if (t.starts_with("Number Frame deadline missed")) {
            summary.deadlineMissed = parseLongField(t, "Number Frame deadline missed");
        }
    }
    return summary;
}

JankStats computeJankStats(const std::vector<FrameRecord>& frames, double refreshPeriodMs) {
    JankStats stats;
    stats.refreshPeriodMs = refreshPeriodMs > 0.0 ? refreshPeriodMs : 16.667;

    std::vector<double> durations;
    durations.reserve(frames.size());

    for (const auto& frame : frames) {
        if (frame.excluded) continue;
        durations.push_back(frame.totalMs);

        // Buckets count vsyncs missed, by the same rule as jankCount, so the
        // two always agree: "on time" is exactly the frames that are not janky.
        const int late = framesLate(frame, stats.refreshPeriodMs);
        if (late <= 0)      ++stats.within1;
        else if (late == 1) ++stats.within2;
        else if (late <= 3) ++stats.within4;
        else                ++stats.beyond4;

        if (missedDeadline(frame, stats.refreshPeriodMs)) ++stats.jankCount;
        if (frame.frameDeadlineNs > frame.intendedVsyncNs) ++stats.deadlineFrames;
        stats.maxMs = std::max(stats.maxMs, frame.totalMs);
    }

    stats.frameCount = durations.size();
    if (stats.frameCount == 0) return stats;

    double sum = 0.0;
    for (const double d : durations) sum += d;
    stats.meanMs = sum / static_cast<double>(stats.frameCount);
    stats.jankPct =
        100.0 * static_cast<double>(stats.jankCount) / static_cast<double>(stats.frameCount);

    stats.p50Ms = percentileInPlace(durations, 50.0);
    stats.p90Ms = percentileInPlace(durations, 90.0);
    stats.p95Ms = percentileInPlace(durations, 95.0);
    stats.p99Ms = percentileInPlace(durations, 99.0);

    const double span = frames.back().startSec - frames.front().startSec;
    if (span > 0.0) stats.estimatedFps = static_cast<double>(frames.size()) / span;

    return stats;
}

// ---------------------------------------------------------------------------
// FrameStatsCollector
// ---------------------------------------------------------------------------

FrameStatsCollector::FrameStatsCollector(DeviceManager& devices) : devices_(devices) {}

FrameStatsCollector::~FrameStatsCollector() { stop(); }

void FrameStatsCollector::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    thread_ = std::thread([this] { pollLoop(); });
}

void FrameStatsCollector::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    sleepCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void FrameStatsCollector::setInterval(std::chrono::milliseconds newInterval) {
    intervalMs_.store(std::clamp<std::int64_t>(newInterval.count(), 250, 10'000),
                      std::memory_order_release);
    sleepCv_.notify_all();
}

std::chrono::milliseconds FrameStatsCollector::interval() const {
    return std::chrono::milliseconds{intervalMs_.load(std::memory_order_acquire)};
}

void FrameStatsCollector::setMaxFrames(std::size_t frames) {
    maxFrames_.store(std::clamp<std::size_t>(frames, 120, 100'000), std::memory_order_release);
}

std::size_t FrameStatsCollector::maxFrames() const {
    return maxFrames_.load(std::memory_order_acquire);
}

void FrameStatsCollector::setRefreshPeriodOverrideMs(double ms) {
    refreshOverrideMs_.store(ms, std::memory_order_release);
}

double FrameStatsCollector::refreshPeriodOverrideMs() const {
    return refreshOverrideMs_.load(std::memory_order_acquire);
}

void FrameStatsCollector::requestReset() {
    resetRequested_.store(true, std::memory_order_release);
    sleepCv_.notify_all();
}

void FrameStatsCollector::pollOnce(std::string_view serial, std::string_view packageName) {
    if (trackedPackage_ != packageName) {
        trackedPackage_ = std::string{packageName};
        seenVsyncsByWindow_.clear();
        firstVsyncNs_ = 0;
        snapshot_.mutate([&](FrameStatsSnapshot& s) {
            s.frames.clear();
            s.packageName = std::string{packageName};
            s.lastError.clear();
        });
    }

    if (resetRequested_.exchange(false, std::memory_order_acq_rel)) {
        const std::string cmd =
            "dumpsys gfxinfo " + shellQuote(packageName) + " reset";
        (void)devices_.adb().shell(serial, cmd, kAdbNormalTimeout);
        seenVsyncsByWindow_.clear();
        firstVsyncNs_ = 0;
        snapshot_.mutate([](FrameStatsSnapshot& s) {
            s.frames.clear();
            s.stats = JankStats{};
            s.lastError.clear();
        });
        return;
    }

    const std::string cmd = "dumpsys gfxinfo " + shellQuote(packageName) + " framestats";
    auto dump = devices_.adb().shell(serial, cmd, kAdbNormalTimeout);
    if (!dump) {
        snapshot_.mutate([msg = dump.error().message](FrameStatsSnapshot& s) {
            s.lastError = msg;
        });
        return;
    }
    const std::string text = std::move(dump).value();

    const GfxSummary summary = parseGfxSummary(text);
    const double override = refreshOverrideMs_.load(std::memory_order_acquire);
    auto parsed = parseFrameStats(text, override);

    if (!parsed) {
        snapshot_.mutate([&](FrameStatsSnapshot& s) {
            s.summary = summary;
            s.lastError = parsed.error().message;
            ++s.pollCount;
        });
        return;
    }

    ParsedFrameStats fresh = std::move(parsed).value();

    snapshot_.mutate([&](FrameStatsSnapshot& s) {
        s.packageName = std::string{packageName};
        s.pid = summary.pid > 0 ? summary.pid : s.pid;
        s.summary = summary;
        s.lastError.clear();
        ++s.pollCount;

        // Nothing new drawn: keep the frames, columns and refresh period
        // already held rather than overwriting them with an empty poll's.
        if (fresh.frames.empty()) return;

        s.columns = fresh.columns;
        s.refreshPeriodMs = fresh.refreshPeriodMs;

        if (firstVsyncNs_ == 0 && !fresh.frames.empty()) {
            firstVsyncNs_ = fresh.frames.front().intendedVsyncNs;
        }

        // Append only frames newer than the last one we kept. The device ring
        // is re-read in full each poll, so without this the history would be
        // mostly duplicates.
        for (auto& frame : fresh.frames) {
            auto& seen = seenVsyncsByWindow_[frame.window];
            if (!seen.insert(frame.intendedVsyncNs).second) continue;
            // The device ring holds at most ~120 frames, so anything this far
            // behind the newest can never be re-reported.
            while (seen.size() > 512) seen.erase(seen.begin());
            frame.startSec =
                (static_cast<double>(frame.intendedVsyncNs) - static_cast<double>(firstVsyncNs_)) / 1e9;
            s.frames.push_back(frame);
        }

        // A late-completing frame can be older than ones already stored.
        std::stable_sort(s.frames.begin(), s.frames.end(),
                         [](const FrameRecord& a, const FrameRecord& b) {
                             return a.intendedVsyncNs < b.intendedVsyncNs;
                         });

        const std::size_t cap = maxFrames_.load(std::memory_order_acquire);
        if (s.frames.size() > cap) {
            s.frames.erase(s.frames.begin(),
                           s.frames.begin() +
                               static_cast<std::ptrdiff_t>(s.frames.size() - cap));
        }

        s.stats = computeJankStats(s.frames, s.refreshPeriodMs);
    });
}

void FrameStatsCollector::pollLoop() {
    while (running_.load(std::memory_order_acquire)) {
        const std::string serial = devices_.selectedSerial();
        const std::string packageName = devices_.selectedPackage();

        if (!serial.empty() && !packageName.empty() && devices_.hasUsableSelection()) {
            pollOnce(serial, packageName);
        }

        std::unique_lock lock{sleepMutex_};
        sleepCv_.wait_for(lock, interval(), [this] {
            return !running_.load(std::memory_order_acquire) ||
                   resetRequested_.load(std::memory_order_acquire);
        });
    }
    EM_LOG_DEBUG(kCategory, "framestats thread exiting");
}

}  // namespace em
