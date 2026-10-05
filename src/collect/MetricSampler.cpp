#include "collect/MetricSampler.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/Log.h"
#include "core/StringUtil.h"

namespace em {
namespace {

constexpr const char* kCategory = "metrics";
constexpr double kKibToMib = 1.0 / 1024.0;

// Index into the batch result. Keep in sync with buildProbeCommands().
enum class Probe : std::size_t {
    ProcStat = 0,
    MemInfo,
    Battery,
    ThermalTemps,
    ThermalTypes,
    AppPid,
    AppStat,
    AppStatus,
    Count,
};

constexpr std::size_t probeIndex(Probe p) { return static_cast<std::size_t>(p); }

std::vector<std::string> buildProbeCommands(std::string_view packageName) {
    std::vector<std::string> commands(probeIndex(Probe::Count));
    commands[probeIndex(Probe::ProcStat)] = "cat /proc/stat";
    commands[probeIndex(Probe::MemInfo)] = "cat /proc/meminfo";
    commands[probeIndex(Probe::Battery)] = "dumpsys battery";
    commands[probeIndex(Probe::ThermalTemps)] = "cat /sys/class/thermal/thermal_zone*/temp";
    commands[probeIndex(Probe::ThermalTypes)] = "cat /sys/class/thermal/thermal_zone*/type";

    if (packageName.empty()) {
        // `true` keeps the sentinel count stable so indices never shift.
        commands[probeIndex(Probe::AppPid)] = "true";
        commands[probeIndex(Probe::AppStat)] = "true";
        commands[probeIndex(Probe::AppStatus)] = "true";
    } else {
        const std::string quoted = shellQuote(packageName);
        // Resolve the pid inside the same shell so the stat reads cannot race
        // against a restart between round trips.
        commands[probeIndex(Probe::AppPid)] = "pidof " + quoted;
        commands[probeIndex(Probe::AppStat)] =
            "P=$(pidof " + quoted + " | tr ' ' '\\n' | head -1); cat /proc/$P/stat";
        commands[probeIndex(Probe::AppStatus)] =
            "P=$(pidof " + quoted + " | tr ' ' '\\n' | head -1); cat /proc/$P/status";
    }
    return commands;
}

double percentBusy(const CpuTimes& previous, const CpuTimes& current) {
    const std::uint64_t totalNow = current.total();
    const std::uint64_t totalBefore = previous.total();
    if (totalNow <= totalBefore) return -1.0;

    const auto deltaTotal = static_cast<double>(totalNow - totalBefore);
    const std::uint64_t busyNow = current.busy();
    const std::uint64_t busyBefore = previous.busy();
    const double deltaBusy =
        busyNow >= busyBefore ? static_cast<double>(busyNow - busyBefore) : 0.0;
    return std::clamp(100.0 * deltaBusy / deltaTotal, 0.0, 100.0);
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------

std::optional<CpuTimes> parseCpuLine(std::string_view line) {
    const auto fields = tokenize(line);
    if (fields.size() < 5) return std::nullopt;  // need at least through idle
    if (!fields[0].starts_with("cpu")) return std::nullopt;

    CpuTimes times;
    std::uint64_t* const slots[] = {&times.user,  &times.nice, &times.system,
                                    &times.idle,  &times.iowait, &times.irq,
                                    &times.softirq, &times.steal};
    constexpr std::size_t kSlotCount = sizeof(slots) / sizeof(slots[0]);

    for (std::size_t i = 0; i < kSlotCount && i + 1 < fields.size(); ++i) {
        const auto value = parseNumber<std::uint64_t>(fields[i + 1]);
        if (!value) return std::nullopt;
        *slots[i] = *value;
    }
    return times;
}

std::optional<CpuSnapshot> parseProcStat(std::string_view text) {
    CpuSnapshot snapshot;
    bool haveAggregate = false;

    for (const auto line : splitLines(text)) {
        if (!line.starts_with("cpu")) {
            if (haveAggregate) break;  // cpu lines are contiguous and come first
            continue;
        }
        const auto times = parseCpuLine(line);
        if (!times) continue;

        // "cpu " is the aggregate; "cpu0", "cpu1", ... are the cores.
        const bool isAggregate = line.size() > 3 && (line[3] == ' ' || line[3] == '\t');
        if (isAggregate) {
            snapshot.aggregate = *times;
            haveAggregate = true;
        } else {
            snapshot.cores.push_back(*times);
        }
    }

    if (!haveAggregate && snapshot.cores.empty()) return std::nullopt;
    return snapshot;
}

std::optional<MemInfo> parseMemInfo(std::string_view text) {
    MemInfo info;
    bool any = false;

    for (const auto line : splitLines(text)) {
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, colon));
        const auto kib = parseNumber<double>(trim(line.substr(colon + 1)));
        if (!kib) continue;

        const double mib = *kib * kKibToMib;
        if (key == "MemTotal")          { info.totalMiB = mib;     any = true; }
        else if (key == "MemAvailable") { info.availableMiB = mib; any = true; }
        else if (key == "MemFree")      { info.freeMiB = mib;      any = true; }
        else if (key == "SwapTotal")    { info.swapTotalMiB = mib; any = true; }
        else if (key == "SwapFree")     { info.swapFreeMiB = mib;  any = true; }
    }

    if (!any) return std::nullopt;
    // Older kernels lack MemAvailable; MemFree is a usable stand-in.
    if (info.availableMiB < 0.0) info.availableMiB = info.freeMiB;
    return info;
}

std::optional<BatteryInfo> parseBatteryDump(std::string_view text) {
    BatteryInfo info;
    bool any = false;

    for (const auto line : splitLines(text)) {
        const std::string_view t = trim(line);
        const auto colon = t.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view key = trim(t.substr(0, colon));
        const std::string_view value = trim(t.substr(colon + 1));

        if (key == "level") {
            if (const auto v = parseNumber<double>(value)) { info.levelPct = *v; any = true; }
        } else if (key == "temperature") {
            // Reported in tenths of a degree Celsius.
            if (const auto v = parseNumber<double>(value)) { info.tempC = *v / 10.0; any = true; }
        } else if (key == "voltage") {
            if (const auto v = parseNumber<double>(value)) {
                info.voltageV = *v / 1000.0;  // millivolts
                any = true;
            }
        } else if (key == "Charge counter" || key == "current now") {
            if (key == "current now") {
                if (const auto v = parseNumber<double>(value)) {
                    info.currentMa = *v / 1000.0;  // microamps
                    any = true;
                }
            }
        } else if (key == "AC powered" || key == "USB powered" || key == "Wireless powered") {
            if (value == "true") info.charging = true;
        }
    }

    if (!any) return std::nullopt;
    return info;
}

std::vector<double> parseThermalZones(std::string_view text) {
    std::vector<double> out;
    for (const auto line : splitLines(text)) {
        const auto milli = parseNumber<double>(trim(line));
        if (!milli) continue;
        // Most zones report millidegrees; a few report whole degrees.
        const double celsius = std::abs(*milli) > 1000.0 ? *milli / 1000.0 : *milli;
        out.push_back(celsius);
    }
    return out;
}

std::optional<std::uint64_t> parseProcPidStatJiffies(std::string_view text) {
    // Field 2 (comm) can contain spaces and parentheses, so start after the
    // last ')' and count from field 3 (state).
    const auto close = text.rfind(')');
    if (close == std::string_view::npos) return std::nullopt;

    const auto fields = tokenize(text.substr(close + 1));
    // After comm: state(3) ppid(4) pgrp(5) session(6) tty(7) tpgid(8) flags(9)
    // minflt(10) cminflt(11) majflt(12) cmajflt(13) utime(14) stime(15)
    constexpr std::size_t kUtimeOffset = 11;  // zero-based index of field 14
    if (fields.size() <= kUtimeOffset + 1) return std::nullopt;

    const auto utime = parseNumber<std::uint64_t>(fields[kUtimeOffset]);
    const auto stime = parseNumber<std::uint64_t>(fields[kUtimeOffset + 1]);
    if (!utime || !stime) return std::nullopt;
    return *utime + *stime;
}

std::optional<ProcStatus> parseProcPidStatus(std::string_view text) {
    ProcStatus status;
    bool any = false;
    for (const auto line : splitLines(text)) {
        if (const auto rss = fieldAfter(line, "VmRSS")) {
            if (const auto kib = parseNumber<double>(*rss)) {
                status.rssMiB = *kib * kKibToMib;
                any = true;
            }
        } else if (const auto threads = fieldAfter(line, "Threads")) {
            if (const auto n = parseNumber<int>(*threads)) {
                status.threads = *n;
                any = true;
            }
        }
    }
    if (!any) return std::nullopt;
    return status;
}

// ---------------------------------------------------------------------------
// MetricHistory
// ---------------------------------------------------------------------------

void MetricHistory::setCapacity(std::size_t capacity) {
    capacity_ = std::max<std::size_t>(capacity, 16);
    for (RingBuffer<double>* buf : {&t, &cpuTotal, &memUsed, &memAvailable, &batteryTemp,
                                    &batteryCurrent, &batteryLevel, &maxThermal, &appCpu,
                                    &appRss}) {
        buf->reserveCapacity(capacity_);
    }
    for (auto& core : cpuCores) core.reserveCapacity(capacity_);
}

void MetricHistory::push(const MetricSample& sample) {
    if (t.capacity() == 0) setCapacity(capacity_);

    t.push(sample.t);
    cpuTotal.push(sample.cpuTotalPct);
    memUsed.push(sample.memUsedMiB);
    memAvailable.push(sample.memAvailableMiB);
    batteryTemp.push(sample.batteryTempC);
    batteryCurrent.push(sample.batteryCurrentMa);
    batteryLevel.push(sample.batteryLevelPct);
    maxThermal.push(sample.maxThermalC);
    appCpu.push(sample.appCpuPct);
    appRss.push(sample.appRssMiB);

    if (cpuCores.size() != sample.cpuCorePct.size()) {
        cpuCores.resize(sample.cpuCorePct.size());
        for (auto& core : cpuCores) core.reserveCapacity(capacity_);
    }
    for (std::size_t i = 0; i < sample.cpuCorePct.size(); ++i) {
        cpuCores[i].push(sample.cpuCorePct[i]);
    }

    latest = sample;
    ++sampleCount;
}

void MetricHistory::clear() {
    for (RingBuffer<double>* buf : {&t, &cpuTotal, &memUsed, &memAvailable, &batteryTemp,
                                    &batteryCurrent, &batteryLevel, &maxThermal, &appCpu,
                                    &appRss}) {
        buf->clear();
    }
    for (auto& core : cpuCores) core.clear();
    latest = MetricSample{};
    sampleCount = 0;
    lastError.clear();
}

// ---------------------------------------------------------------------------
// MetricSampler
// ---------------------------------------------------------------------------

MetricSampler::MetricSampler(DeviceManager& devices) : devices_(devices) {
    history_.mutate([](MetricHistory& h) { h.setCapacity(1800); });
}

MetricSampler::~MetricSampler() { stop(); }

void MetricSampler::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    startTime_ = std::chrono::steady_clock::now();
    thread_ = std::thread([this] { sampleLoop(); });
}

void MetricSampler::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    sleepCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void MetricSampler::setInterval(std::chrono::milliseconds newInterval) {
    const auto ms = std::clamp<std::int64_t>(newInterval.count(), 100, 10'000);
    intervalMs_.store(ms, std::memory_order_release);

    const auto samples = static_cast<std::size_t>(
        std::max<std::int64_t>(historyMs_.load(std::memory_order_acquire) / ms, 16));
    history_.mutate([samples](MetricHistory& h) { h.setCapacity(samples); });
    sleepCv_.notify_all();
}

std::chrono::milliseconds MetricSampler::interval() const {
    return std::chrono::milliseconds{intervalMs_.load(std::memory_order_acquire)};
}

void MetricSampler::setHistorySeconds(double seconds) {
    const auto ms = std::clamp<std::int64_t>(static_cast<std::int64_t>(seconds * 1000.0),
                                             10'000, 3'600'000);
    historyMs_.store(ms, std::memory_order_release);
    setInterval(interval());  // recomputes capacity
}

double MetricSampler::historySeconds() const {
    return static_cast<double>(historyMs_.load(std::memory_order_acquire)) / 1000.0;
}

void MetricSampler::reset() {
    history_.mutate([](MetricHistory& h) { h.clear(); });
    hasPreviousCpu_ = false;
    hasPreviousApp_ = false;
    cachedPid_ = -1;
    cachedPidPackage_.clear();
    startTime_ = std::chrono::steady_clock::now();
}

MetricSample MetricSampler::latest() const {
    MetricSample out;
    history_.read([&out](const MetricHistory& h) { out = h.latest; });
    return out;
}

std::optional<MetricSample> MetricSampler::collectOnce(std::string_view serial,
                                                       std::string_view packageName) {
    const auto commands = buildProbeCommands(packageName);
    auto batch = devices_.adb().shellBatch(serial, commands, kAdbNormalTimeout);
    if (!batch) {
        history_.mutate([msg = batch.error().message](MetricHistory& h) { h.lastError = msg; });
        return std::nullopt;
    }
    const std::vector<std::string> parts = std::move(batch).value();

    MetricSample sample;
    sample.t = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime_)
                   .count();
    const double elapsed = sample.t - previousSampleTime_;

    ProbeSupport support = support_.get();

    // --- CPU ---------------------------------------------------------------
    if (const auto snapshot = parseProcStat(parts[probeIndex(Probe::ProcStat)])) {
        support.procStat = true;
        support.coreCount = static_cast<int>(snapshot->cores.size());

        if (hasPreviousCpu_) {
            sample.cpuTotalPct = percentBusy(previousCpu_.aggregate, snapshot->aggregate);
            const std::size_t cores =
                std::min(previousCpu_.cores.size(), snapshot->cores.size());
            sample.cpuCorePct.reserve(cores);
            for (std::size_t i = 0; i < cores; ++i) {
                sample.cpuCorePct.push_back(
                    percentBusy(previousCpu_.cores[i], snapshot->cores[i]));
            }
        }
        previousCpu_ = *snapshot;
        hasPreviousCpu_ = true;
    }

    // --- memory ------------------------------------------------------------
    if (const auto mem = parseMemInfo(parts[probeIndex(Probe::MemInfo)])) {
        support.meminfo = true;
        sample.memTotalMiB = mem->totalMiB;
        sample.memAvailableMiB = mem->availableMiB;
        if (mem->totalMiB > 0.0 && mem->availableMiB >= 0.0) {
            sample.memUsedMiB = mem->totalMiB - mem->availableMiB;
        }
        if (mem->swapTotalMiB > 0.0 && mem->swapFreeMiB >= 0.0) {
            sample.swapUsedMiB = mem->swapTotalMiB - mem->swapFreeMiB;
        }
    }

    // --- battery -----------------------------------------------------------
    if (const auto battery = parseBatteryDump(parts[probeIndex(Probe::Battery)])) {
        support.battery = true;
        sample.batteryLevelPct = battery->levelPct;
        sample.batteryTempC = battery->tempC;
        sample.batteryCurrentMa = battery->currentMa;
        sample.batteryVoltageV = battery->voltageV;
    }

    // --- thermal -----------------------------------------------------------
    sample.thermalC = parseThermalZones(parts[probeIndex(Probe::ThermalTemps)]);
    if (!sample.thermalC.empty()) {
        support.thermal = true;
        sample.maxThermalC = *std::max_element(sample.thermalC.begin(), sample.thermalC.end());

        if (support.thermalZoneNames.size() != sample.thermalC.size()) {
            support.thermalZoneNames.clear();
            for (const auto name : splitLines(parts[probeIndex(Probe::ThermalTypes)])) {
                support.thermalZoneNames.emplace_back(trim(name));
            }
            support.thermalZoneNames.resize(sample.thermalC.size());
        }
    }

    // --- per-app -----------------------------------------------------------
    if (!packageName.empty()) {
        const auto pidTokens = tokenize(parts[probeIndex(Probe::AppPid)]);
        if (!pidTokens.empty()) {
            if (const auto pid = parseNumber<int>(pidTokens.front())) sample.appPid = *pid;
        }

        if (sample.appPid > 0) {
            support.perApp = true;
            if (cachedPid_ != sample.appPid) {
                // Process restarted: the jiffy baseline is meaningless now.
                hasPreviousApp_ = false;
                cachedPid_ = sample.appPid;
                cachedPidPackage_ = std::string{packageName};
            }

            if (const auto jiffies =
                    parseProcPidStatJiffies(parts[probeIndex(Probe::AppStat)])) {
                if (hasPreviousApp_ && elapsed > 0.0 && *jiffies >= previousAppJiffies_) {
                    const auto delta = static_cast<double>(*jiffies - previousAppJiffies_);
                    const double seconds =
                        delta / static_cast<double>(clockTicksPerSecond_);
                    sample.appCpuPct = std::clamp(100.0 * seconds / elapsed, 0.0,
                                                  100.0 * std::max(support.coreCount, 1));
                }
                previousAppJiffies_ = *jiffies;
                hasPreviousApp_ = true;
            }

            if (const auto status = parseProcPidStatus(parts[probeIndex(Probe::AppStatus)])) {
                sample.appRssMiB = status->rssMiB;
                sample.appThreads = status->threads;
            }
        } else {
            hasPreviousApp_ = false;
            cachedPid_ = -1;
        }
    }

    support_.set(std::move(support));
    previousSampleTime_ = sample.t;
    return sample;
}

void MetricSampler::sampleLoop() {
    while (running_.load(std::memory_order_acquire)) {
        const std::string serial = devices_.selectedSerial();
        const std::string packageName = devices_.selectedPackage();

        if (serial.empty() || !devices_.hasUsableSelection()) {
            hasPreviousCpu_ = false;
            hasPreviousApp_ = false;
        } else {
            if (auto sample = collectOnce(serial, packageName)) {
                // The first tick after (re)connecting has no CPU delta yet.
                if (sample->cpuTotalPct >= 0.0 || sample->memUsedMiB >= 0.0) {
                    history_.mutate([&sample](MetricHistory& h) {
                        h.push(*sample);
                        h.lastError.clear();
                    });
                }
            }
        }

        std::unique_lock lock{sleepMutex_};
        sleepCv_.wait_for(lock, interval(), [this] {
            return !running_.load(std::memory_order_acquire);
        });
    }
    EM_LOG_DEBUG(kCategory, "sampler thread exiting");
}

}  // namespace em
