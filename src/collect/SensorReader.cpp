#include "collect/SensorReader.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "core/Log.h"
#include "core/StringUtil.h"

namespace em {
namespace {

constexpr const char* kCategory = "sensors";

// Parses "0x00000001)" or "0000000001)" into a handle.
std::optional<std::uint32_t> parseHandleToken(std::string_view token) {
    if (token.empty() || token.back() != ')') return std::nullopt;
    token.remove_suffix(1);

    if (token.starts_with("0x") || token.starts_with("0X")) {
        std::uint32_t value = 0;
        for (const char c : token.substr(2)) {
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return std::nullopt;
            value = value * 16 + static_cast<std::uint32_t>(digit);
        }
        return value;
    }
    return parseNumber<std::uint32_t>(token);
}

// The sensor list uses ' | ' separated fields. Returns them trimmed.
std::vector<std::string_view> pipeFields(std::string_view line) {
    auto parts = split(line, '|', true);
    for (auto& part : parts) part = trim(part);
    return parts;
}

void parseRateAndFifo(std::string_view line, SensorDescriptor& sensor) {
    if (const auto v = fieldAfter(line, "minRate")) {
        if (const auto hz = parseNumber<double>(*v)) sensor.minRateHz = *hz;
    }
    if (const auto v = fieldAfter(line, "maxRate")) {
        if (const auto hz = parseNumber<double>(*v)) sensor.maxRateHz = *hz;
    }
    // FIFO (max,reserved) = (3000, 1200) events
    if (const auto v = fieldAfter(line, "FIFO (max,reserved) =")) {
        const auto open = v->find('(');
        if (open != std::string_view::npos) {
            const auto numbers = split(v->substr(open + 1), ',', false);
            if (numbers.size() >= 2) {
                if (const auto maxCount = parseNumber<int>(numbers[0])) {
                    sensor.fifoMax = *maxCount;
                }
                if (const auto reserved = parseNumber<int>(numbers[1])) {
                    sensor.fifoReserved = *reserved;
                }
            }
        }
    }
    for (const auto mode : {"continuous", "on-change", "one-shot", "special-trigger"}) {
        if (line.find(mode) != std::string_view::npos) {
            sensor.reportingMode = mode;
            break;
        }
    }
    if (line.find("wakeUp") != std::string_view::npos &&
        line.find("non-wakeUp") == std::string_view::npos) {
        sensor.wakeUp = true;
    }
}

}  // namespace

std::string SensorDescriptor::shortType() const {
    if (const auto stripped = stripPrefix(typeString, "android.sensor.")) {
        std::string out{*stripped};
        if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
        for (char& c : out) {
            if (c == '_') c = ' ';
        }
        return out;
    }
    return typeString.empty() ? name : typeString;
}

int SensorDescriptor::axisCount() const {
    // Android sensor type constants. The three-axis families are the ones a
    // chart needs to split out; everything else is treated as scalar.
    switch (typeId) {
        case 1:   // ACCELEROMETER
        case 2:   // MAGNETIC_FIELD
        case 4:   // GYROSCOPE
        case 9:   // GRAVITY
        case 10:  // LINEAR_ACCELERATION
        case 14:  // MAGNETIC_FIELD_UNCALIBRATED
        case 16:  // GYROSCOPE_UNCALIBRATED
        case 35:  // ACCELEROMETER_UNCALIBRATED
            return 3;
        case 3:   // ORIENTATION (azimuth/pitch/roll)
            return 3;
        case 11:  // ROTATION_VECTOR
        case 15:  // GAME_ROTATION_VECTOR
        case 20:  // GEOMAGNETIC_ROTATION_VECTOR
            return 4;
        default:
            return 1;
    }
}

std::vector<SensorDescriptor> parseSensorList(std::string_view dump) {
    std::vector<SensorDescriptor> sensors;

    const auto lines = splitLines(dump);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = trim(lines[i]);
        if (line.empty()) continue;

        const auto fields = pipeFields(line);
        if (fields.size() < 3) continue;

        // The first field must look like "0x00000001) Accelerometer Sensor".
        const auto firstTokens = tokenize(fields[0]);
        if (firstTokens.empty()) continue;
        const auto handle = parseHandleToken(firstTokens[0]);
        if (!handle) continue;

        SensorDescriptor sensor;
        sensor.handle = *handle;
        sensor.name = std::string{trim(fields[0].substr(firstTokens[0].size()))};
        sensor.vendor = std::string{fields[1]};

        for (std::size_t f = 2; f < fields.size(); ++f) {
            const std::string_view field = fields[f];
            if (const auto v = stripPrefix(field, "ver:")) {
                if (const auto ver = parseNumber<int>(*v)) sensor.version = *ver;
            } else if (const auto typeField = stripPrefix(field, "type:")) {
                // android.sensor.gyroscope(4)
                const std::string_view text = trim(*typeField);
                const auto open = text.find('(');
                sensor.typeString =
                    std::string{open == std::string_view::npos ? text : text.substr(0, open)};
                if (open != std::string_view::npos) {
                    if (const auto id = parseNumber<int>(text.substr(open + 1))) {
                        sensor.typeId = *id;
                    }
                }
            } else if (const auto permField = stripPrefix(field, "perm:")) {
                sensor.permission = std::string{trim(*permField)};
            } else if (const auto flagsField = stripPrefix(field, "flags:")) {
                const std::string_view hex = trim(*flagsField);
                if (hex.starts_with("0x")) {
                    std::uint32_t value = 0;
                    for (const char c : hex.substr(2)) {
                        int digit = -1;
                        if (c >= '0' && c <= '9') digit = c - '0';
                        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                        else break;
                        value = value * 16 + static_cast<std::uint32_t>(digit);
                    }
                    sensor.flags = value;
                }
            }
        }

        // The following indented line carries rate / FIFO / reporting mode.
        // It cannot be recognised by the absence of ')' -- the FIFO field has
        // parentheses of its own -- so test whether it opens a new sensor.
        if (i + 1 < lines.size()) {
            const std::string_view next = lines[i + 1];
            const bool indented =
                !next.empty() && (next.front() == ' ' || next.front() == '\t');
            const auto nextTokens = tokenize(next);
            const bool opensNewSensor =
                !nextTokens.empty() && parseHandleToken(nextTokens.front()).has_value();
            if (indented && !opensNewSensor) parseRateAndFifo(next, sensor);
        }

        sensors.push_back(std::move(sensor));
    }

    std::sort(sensors.begin(), sensors.end(),
              [](const SensorDescriptor& a, const SensorDescriptor& b) {
                  return a.handle < b.handle;
              });
    sensors.erase(std::unique(sensors.begin(), sensors.end(),
                              [](const SensorDescriptor& a, const SensorDescriptor& b) {
                                  return a.handle == b.handle;
                              }),
                  sensors.end());
    return sensors;
}

std::vector<SensorReading> parseSensorEvents(std::string_view dump) {
    // Recent-event blocks look like:
    //   Gyroscope Sensor (handle=0x00000004, 3-axis) last 10 events:
    //     1 (ts=12345.678901, wall=10:11:12.345) -0.01, 0.00, 0.02,
    std::vector<SensorReading> readings;
    std::optional<std::uint32_t> currentHandle;

    for (const auto rawLine : splitLines(dump)) {
        const std::string_view line = trim(rawLine);
        if (line.empty()) continue;

        if (const auto pos = line.find("handle=0x"); pos != std::string_view::npos) {
            const std::string_view hex = line.substr(pos + 9);
            std::uint32_t value = 0;
            bool any = false;
            for (const char c : hex) {
                int digit = -1;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                else break;
                value = value * 16 + static_cast<std::uint32_t>(digit);
                any = true;
            }
            if (any) currentHandle = value;
            continue;
        }

        if (!currentHandle) continue;
        const auto tsPos = line.find("ts=");
        if (tsPos == std::string_view::npos) continue;

        SensorReading reading;
        reading.handle = *currentHandle;
        if (const auto ts = parseNumber<double>(line.substr(tsPos + 3))) {
            reading.timestampSec = *ts;
        }

        // Values follow the closing parenthesis of the (ts=..., wall=...) group.
        const auto close = line.find(')', tsPos);
        if (close == std::string_view::npos) continue;
        for (const auto field : split(line.substr(close + 1), ',', false)) {
            if (const auto v = parseNumber<double>(trim(field))) reading.values.push_back(*v);
        }
        if (reading.values.empty()) continue;

        // Blocks list newest first on most releases; keep the first we see per
        // handle and ignore the rest.
        const bool alreadyHave =
            std::any_of(readings.begin(), readings.end(), [&](const SensorReading& r) {
                return r.handle == reading.handle;
            });
        if (!alreadyHave) readings.push_back(std::move(reading));
    }
    return readings;
}

std::optional<int> parseActiveSensorCount(std::string_view dump) {
    for (const auto line : splitLines(dump)) {
        // "Total 24 h/w sensors, 1 running:"
        const auto pos = line.find("h/w sensors,");
        if (pos == std::string_view::npos) continue;
        const auto tail = line.substr(pos + 12);
        const auto tokens = tokenize(tail);
        if (tokens.empty()) continue;
        if (const auto n = parseNumber<int>(tokens.front())) return *n;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// SensorTrace
// ---------------------------------------------------------------------------

void SensorTrace::setCapacity(std::size_t capacity) {
    capacity_ = std::max<std::size_t>(capacity, 16);
    t.reserveCapacity(capacity_);
    for (auto& axis : axes) axis.reserveCapacity(capacity_);
}

void SensorTrace::push(double time, const std::vector<double>& values) {
    if (t.capacity() == 0) setCapacity(capacity_);
    if (axes.size() != values.size()) {
        axes.resize(values.size());
        for (auto& axis : axes) axis.reserveCapacity(capacity_);
    }
    t.push(time);
    for (std::size_t i = 0; i < values.size(); ++i) axes[i].push(values[i]);
}

void SensorTrace::clear() {
    t.clear();
    for (auto& axis : axes) axis.clear();
}

// ---------------------------------------------------------------------------
// SensorReader
// ---------------------------------------------------------------------------

SensorReader::SensorReader(DeviceManager& devices) : devices_(devices) {
    startTime_ = std::chrono::steady_clock::now();
}

SensorReader::~SensorReader() { stopPolling(); }

void SensorReader::clear() { snapshot_.set(SensorSnapshot{}); }

void SensorReader::enumerate(ThreadPool& pool) {
    if (enumerating_.exchange(true, std::memory_order_acq_rel)) return;

    const std::string serial = devices_.selectedSerial();
    if (serial.empty()) {
        enumerating_.store(false, std::memory_order_release);
        return;
    }

    pool.submit([this, serial] {
        auto dump = devices_.adb().shell(serial, "dumpsys sensorservice", kAdbLongTimeout);
        if (!dump) {
            snapshot_.mutate([msg = dump.error().message](SensorSnapshot& s) {
                s.lastError = msg;
            });
            enumerating_.store(false, std::memory_order_release);
            return;
        }
        const std::string text = std::move(dump).value();
        auto sensors = parseSensorList(text);
        const auto active = parseActiveSensorCount(text);

        EM_LOG_INFO(kCategory, "found " + std::to_string(sensors.size()) + " sensors on " + serial);

        snapshot_.mutate([&](SensorSnapshot& s) {
            s.sensors = std::move(sensors);
            s.activeSensorCount = active.value_or(0);
            s.rawDump = text;
            s.loaded = true;
            s.lastError = s.sensors.empty() ? "no sensors parsed from dumpsys sensorservice" : "";
        });
        enumerating_.store(false, std::memory_order_release);
    });
}

void SensorReader::startPolling() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    startTime_ = std::chrono::steady_clock::now();
    thread_ = std::thread([this] { pollLoop(); });
}

void SensorReader::stopPolling() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    sleepCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SensorReader::setInterval(std::chrono::milliseconds newInterval) {
    intervalMs_.store(std::clamp<std::int64_t>(newInterval.count(), 250, 10'000),
                      std::memory_order_release);
    sleepCv_.notify_all();
}

std::chrono::milliseconds SensorReader::interval() const {
    return std::chrono::milliseconds{intervalMs_.load(std::memory_order_acquire)};
}

void SensorReader::pollOnce(std::string_view serial) {
    auto dump = devices_.adb().shell(serial, "dumpsys sensorservice", kAdbLongTimeout);
    if (!dump) {
        snapshot_.mutate([msg = dump.error().message](SensorSnapshot& s) { s.lastError = msg; });
        return;
    }
    const std::string text = std::move(dump).value();
    auto readings = parseSensorEvents(text);
    const auto active = parseActiveSensorCount(text);
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime_).count();

    snapshot_.mutate([&](SensorSnapshot& s) {
        if (s.sensors.empty()) s.sensors = parseSensorList(text);
        s.rawDump = text;
        s.activeSensorCount = active.value_or(s.activeSensorCount);
        s.lastError.clear();
        ++s.pollCount;

        for (auto& reading : readings) {
            s.traces[reading.handle].push(now, reading.values);
            s.latest[reading.handle] = std::move(reading);
        }
    });
}

void SensorReader::pollLoop() {
    while (running_.load(std::memory_order_acquire)) {
        const std::string serial = devices_.selectedSerial();
        if (!serial.empty() && devices_.hasUsableSelection()) pollOnce(serial);

        std::unique_lock lock{sleepMutex_};
        sleepCv_.wait_for(lock, interval(), [this] {
            return !running_.load(std::memory_order_acquire);
        });
    }
}

}  // namespace em
