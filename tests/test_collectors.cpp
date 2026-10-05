#include "TestHarness.h"

#include "collect/DeviceInfo.h"
#include "collect/FileSystemBrowser.h"
#include "collect/MetricSampler.h"
#include "collect/PerfettoCapture.h"
#include "collect/SensorReader.h"
#include "collect/TraceProcessor.h"

using namespace em;

// ---------------------------------------------------------------------------
// MetricSampler parsers
// ---------------------------------------------------------------------------

TEST_CASE("parseProcStat separates the aggregate from the cores") {
    constexpr const char* kStat =
        "cpu  1000 20 300 5000 40 5 2 0 0 0\n"
        "cpu0 500 10 150 2500 20 3 1 0 0 0\n"
        "cpu1 500 10 150 2500 20 2 1 0 0 0\n"
        "intr 1234 5 6\n"
        "ctxt 98765\n";

    const auto snapshot = parseProcStat(kStat);
    CHECK(snapshot.has_value());
    if (!snapshot) return;

    CHECK_EQ(snapshot->cores.size(), std::size_t{2});
    CHECK_EQ(snapshot->aggregate.user, std::uint64_t{1000});
    CHECK_EQ(snapshot->aggregate.idle, std::uint64_t{5000});
    CHECK_EQ(snapshot->aggregate.total(), std::uint64_t{1000 + 20 + 300 + 5000 + 40 + 5 + 2});
    CHECK_EQ(snapshot->aggregate.busy(), snapshot->aggregate.total() - 5000 - 40);
    CHECK_EQ(snapshot->cores[0].user, std::uint64_t{500});
}

TEST_CASE("parseProcStat rejects junk") {
    CHECK(!parseProcStat("intr 1 2 3\nctxt 4\n").has_value());
    CHECK(!parseProcStat("").has_value());
}

TEST_CASE("parseMemInfo converts kB to MiB") {
    constexpr const char* kMemInfo =
        "MemTotal:       16227664 kB\n"
        "MemFree:          982344 kB\n"
        "MemAvailable:    5242880 kB\n"
        "Buffers:           12345 kB\n"
        "SwapTotal:       4194304 kB\n"
        "SwapFree:        3145728 kB\n";

    const auto info = parseMemInfo(kMemInfo);
    CHECK(info.has_value());
    if (!info) return;

    CHECK_NEAR(info->totalMiB, 16227664.0 / 1024.0, 0.01);
    CHECK_NEAR(info->availableMiB, 5120.0, 0.01);
    CHECK_NEAR(info->swapTotalMiB, 4096.0, 0.01);
    CHECK_NEAR(info->swapFreeMiB, 3072.0, 0.01);
}

TEST_CASE("parseMemInfo falls back to MemFree when MemAvailable is absent") {
    const auto info = parseMemInfo("MemTotal: 1024 kB\nMemFree: 512 kB\n");
    CHECK(info.has_value());
    if (info) CHECK_NEAR(info->availableMiB, 0.5, 1e-6);
}

TEST_CASE("parseBatteryDump scales the units dumpsys uses") {
    constexpr const char* kBattery =
        "Current Battery Service state:\n"
        "  AC powered: false\n"
        "  USB powered: true\n"
        "  status: 2\n"
        "  level: 87\n"
        "  voltage: 4215\n"
        "  temperature: 312\n"
        "  current now: -458000\n";

    const auto info = parseBatteryDump(kBattery);
    CHECK(info.has_value());
    if (!info) return;

    CHECK_NEAR(info->levelPct, 87.0, 0.001);
    CHECK_NEAR(info->tempC, 31.2, 0.001);      // tenths of a degree
    CHECK_NEAR(info->voltageV, 4.215, 0.001);  // millivolts
    CHECK_NEAR(info->currentMa, -458.0, 0.001);  // microamps
    CHECK(info->charging);
}

TEST_CASE("parseThermalZones handles millidegrees and degrees") {
    const auto zones = parseThermalZones("45000\n52300\n38\n");
    CHECK_EQ(zones.size(), std::size_t{3});
    CHECK_NEAR(zones[0], 45.0, 0.001);
    CHECK_NEAR(zones[1], 52.3, 0.001);
    CHECK_NEAR(zones[2], 38.0, 0.001);
}

TEST_CASE("parseProcPidStatJiffies survives a comm with spaces") {
    // utime is field 14, stime field 15.
    const std::string stat =
        "1234 (my weird (app) name) S 1 1234 0 0 -1 1077936192 "
        "100 200 0 0 "   // minflt cminflt majflt cmajflt
        "4500 1500 "     // utime stime
        "0 0 20 0 42 0 0";
    const auto jiffies = parseProcPidStatJiffies(stat);
    CHECK(jiffies.has_value());
    if (jiffies) CHECK_EQ(*jiffies, std::uint64_t{6000});
}

TEST_CASE("parseProcPidStatJiffies rejects a truncated line") {
    CHECK(!parseProcPidStatJiffies("1234 (app) S 1").has_value());
    CHECK(!parseProcPidStatJiffies("no parens here").has_value());
}

TEST_CASE("parseProcPidStatus reads VmRSS and Threads") {
    constexpr const char* kStatus =
        "Name:\tcom.example.app\n"
        "State:\tS (sleeping)\n"
        "VmRSS:\t  204800 kB\n"
        "Threads:\t47\n";

    const auto status = parseProcPidStatus(kStatus);
    CHECK(status.has_value());
    if (!status) return;
    CHECK_NEAR(status->rssMiB, 200.0, 0.01);
    CHECK_EQ(status->threads, 47);
}

// ---------------------------------------------------------------------------
// File explorer
// ---------------------------------------------------------------------------

TEST_CASE("parseLsLongOutput reads toybox ls -la") {
    constexpr const char* kLs =
        "total 68\n"
        "drwxrwx--x   4 system system    4096 2024-05-01 09:12 data\n"
        "-rw-r--r--   1 root   root        512 2024-05-01 09:13 build.prop\n"
        "lrwxrwxrwx   1 root   root         21 2024-05-01 09:14 sdcard -> "
        "/storage/self/primary\n"
        "-rwxr-xr-x   1 shell  shell    123456 2024-05-01 09:15 my file with spaces\n"
        "crw-rw-rw-   1 root   root      1,   3 2024-05-01 09:16 null\n";

    const auto entries = parseLsLongOutput(kLs);
    CHECK_EQ(entries.size(), std::size_t{5});

    // Directories sort first.
    CHECK_EQ(entries[0].name, std::string{"data"});
    CHECK(entries[0].isDirectory());
    CHECK_EQ(entries[0].owner, std::string{"system"});
    CHECK_EQ(entries[0].permissions, std::string{"drwxrwx--x"});

    const auto find = [&entries](std::string_view name) -> const FileEntry* {
        for (const auto& entry : entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    };

    const FileEntry* prop = find("build.prop");
    CHECK(prop != nullptr);
    if (prop != nullptr) {
        CHECK_EQ(prop->size, std::uint64_t{512});
        CHECK_EQ(prop->modified, std::string{"2024-05-01 09:13"});
        CHECK(!prop->executable());
    }

    const FileEntry* link = find("sdcard");
    CHECK(link != nullptr);
    if (link != nullptr) {
        CHECK(link->isSymlink());
        CHECK_EQ(link->linkTarget, std::string{"/storage/self/primary"});
    }

    const FileEntry* spaced = find("my file with spaces");
    CHECK(spaced != nullptr);
    if (spaced != nullptr) {
        CHECK_EQ(spaced->size, std::uint64_t{123456});
        CHECK(spaced->executable());
    }

    const FileEntry* device = find("null");
    CHECK(device != nullptr);
    if (device != nullptr) CHECK(device->kind == FileKind::Character);
}

TEST_CASE("parseLsLongOutput handles the month-name date format") {
    constexpr const char* kLs = "-rw-r--r-- 1 root root 100 May  1 09:12 thing\n";
    const auto entries = parseLsLongOutput(kLs);
    CHECK_EQ(entries.size(), std::size_t{1});
    if (!entries.empty()) CHECK_EQ(entries[0].name, std::string{"thing"});
}

TEST_CASE("parseLsLongOutput ignores . and ..") {
    constexpr const char* kLs =
        "drwxr-xr-x 2 root root 4096 2024-05-01 09:12 .\n"
        "drwxr-xr-x 2 root root 4096 2024-05-01 09:12 ..\n"
        "drwxr-xr-x 2 root root 4096 2024-05-01 09:12 .hidden\n";
    const auto entries = parseLsLongOutput(kLs);
    CHECK_EQ(entries.size(), std::size_t{1});
    if (!entries.empty()) CHECK_EQ(entries[0].name, std::string{".hidden"});
}

TEST_CASE("device path join and parent") {
    CHECK_EQ(joinDevicePath("/sdcard", "Download"), std::string{"/sdcard/Download"});
    CHECK_EQ(joinDevicePath("/", "data"), std::string{"/data"});
    CHECK_EQ(joinDevicePath("/sdcard", "/absolute"), std::string{"/absolute"});
    CHECK_EQ(joinDevicePath("/a/b", ".."), std::string{"/a"});

    CHECK_EQ(parentDevicePath("/a/b/c"), std::string{"/a/b"});
    CHECK_EQ(parentDevicePath("/a"), std::string{"/"});
    CHECK_EQ(parentDevicePath("/"), std::string{"/"});
    CHECK_EQ(parentDevicePath("/a/b/"), std::string{"/a"});
}

// ---------------------------------------------------------------------------
// Sensors
// ---------------------------------------------------------------------------

TEST_CASE("parseSensorList reads the handle, type and rates") {
    constexpr const char* kDump =
        "Sensor List:\n"
        "0x00000001) LSM6DSO Accelerometer | STMicro | ver: 1 | type: "
        "android.sensor.accelerometer(1) | perm: n/a | flags: 0x00000000\n"
        "\tcontinuous | minRate=12.50Hz | maxRate=416.00Hz | FIFO (max,reserved) = "
        "(3000, 1200) events | non-wakeUp |\n"
        "0x00000004) LSM6DSO Gyroscope | STMicro | ver: 1 | type: "
        "android.sensor.gyroscope(4) | perm: n/a | flags: 0x00000000\n"
        "\tcontinuous | minRate=12.50Hz | maxRate=416.00Hz | FIFO (max,reserved) = "
        "(3000, 1200) events | non-wakeUp |\n"
        "0x0000000b) Game Rotation Vector | AOSP | ver: 1 | type: "
        "android.sensor.game_rotation_vector(15) | perm: n/a | flags: 0x00000000\n";

    const auto sensors = parseSensorList(kDump);
    CHECK_EQ(sensors.size(), std::size_t{3});
    if (sensors.size() < 3) return;

    CHECK_EQ(sensors[0].handle, std::uint32_t{1});
    CHECK_EQ(sensors[0].name, std::string{"LSM6DSO Accelerometer"});
    CHECK_EQ(sensors[0].vendor, std::string{"STMicro"});
    CHECK_EQ(sensors[0].typeString, std::string{"android.sensor.accelerometer"});
    CHECK_EQ(sensors[0].typeId, 1);
    CHECK_EQ(sensors[0].version, 1);
    CHECK_NEAR(sensors[0].minRateHz, 12.5, 0.01);
    CHECK_NEAR(sensors[0].maxRateHz, 416.0, 0.01);
    CHECK_EQ(sensors[0].fifoMax, 3000);
    CHECK_EQ(sensors[0].fifoReserved, 1200);
    CHECK(!sensors[0].wakeUp);
    CHECK_EQ(sensors[0].reportingMode, std::string{"continuous"});
    CHECK_EQ(sensors[0].axisCount(), 3);
    CHECK_EQ(sensors[0].shortType(), std::string{"Accelerometer"});

    CHECK_EQ(sensors[1].handle, std::uint32_t{4});
    CHECK_EQ(sensors[1].typeId, 4);
    CHECK_EQ(sensors[1].axisCount(), 3);

    CHECK_EQ(sensors[2].handle, std::uint32_t{11});
    CHECK_EQ(sensors[2].typeId, 15);
    CHECK_EQ(sensors[2].axisCount(), 4);
    CHECK_EQ(sensors[2].shortType(), std::string{"Game rotation vector"});
}

TEST_CASE("parseSensorList accepts the decimal handle format") {
    constexpr const char* kDump =
        "0000000000) Accelerometer | Bosch | ver: 1 | type: "
        "android.sensor.accelerometer(1) | perm: n/a\n";
    const auto sensors = parseSensorList(kDump);
    CHECK_EQ(sensors.size(), std::size_t{1});
    if (!sensors.empty()) CHECK_EQ(sensors[0].handle, std::uint32_t{0});
}

TEST_CASE("parseSensorEvents picks the newest sample per handle") {
    constexpr const char* kDump =
        "Gyroscope Sensor (handle=0x00000004, 3-axis) last 10 events:\n"
        "  1 (ts=123.456789, wall=10:11:12.345) -0.0100, 0.0200, 0.0300,\n"
        "  2 (ts=123.446789, wall=10:11:12.335) -0.0110, 0.0210, 0.0310,\n"
        "Light Sensor (handle=0x00000005, 1-axis) last 10 events:\n"
        "  1 (ts=123.450000, wall=10:11:12.340) 128.0000,\n";

    const auto readings = parseSensorEvents(kDump);
    CHECK_EQ(readings.size(), std::size_t{2});
    if (readings.size() < 2) return;

    CHECK_EQ(readings[0].handle, std::uint32_t{4});
    CHECK_EQ(readings[0].values.size(), std::size_t{3});
    CHECK_NEAR(readings[0].timestampSec, 123.456789, 1e-6);
    CHECK_NEAR(readings[0].values[0], -0.01, 1e-6);
    CHECK_NEAR(readings[0].values[2], 0.03, 1e-6);

    CHECK_EQ(readings[1].handle, std::uint32_t{5});
    CHECK_EQ(readings[1].values.size(), std::size_t{1});
    CHECK_NEAR(readings[1].values[0], 128.0, 1e-6);
}

TEST_CASE("parseActiveSensorCount reads the running count") {
    const auto count =
        parseActiveSensorCount("  Total 24 h/w sensors, 3 running:\n  0x1) active-count = 1\n");
    CHECK(count.has_value());
    if (count) CHECK_EQ(*count, 3);
    CHECK(!parseActiveSensorCount("nothing").has_value());
}

// ---------------------------------------------------------------------------
// Device info
// ---------------------------------------------------------------------------

TEST_CASE("buildInfoSections only includes what the device reported") {
    PropertyMap props;
    props["ro.product.manufacturer"] = "Google";
    props["ro.product.model"] = "Pixel 7 Pro";
    props["ro.build.version.sdk"] = "34";
    props["ro.product.cpu.abi"] = "arm64-v8a";
    props["ro.empty"] = "   ";  // blank values must be skipped

    DeviceInfoExtras extras;
    extras.kernel = "Linux localhost 5.10.157";
    extras.screenSize = "Physical size: 1440x3120";
    extras.cpuInfo = "processor\t: 0\nprocessor\t: 1\nHardware\t: Tensor G2\n";
    extras.uptime = "123456.78 98765.43";

    const auto sections = buildInfoSections(props, extras);
    CHECK(!sections.empty());

    const auto findField = [&sections](std::string_view label) -> const InfoField* {
        for (const auto& section : sections) {
            for (const auto& field : section.fields) {
                if (field.label == label) return &field;
            }
        }
        return nullptr;
    };

    const InfoField* model = findField("Model");
    CHECK(model != nullptr);
    if (model != nullptr) CHECK_EQ(model->value, std::string{"Pixel 7 Pro"});

    const InfoField* cores = findField("CPU cores");
    CHECK(cores != nullptr);
    if (cores != nullptr) CHECK_EQ(cores->value, std::string{"2"});

    const InfoField* cpuModel = findField("CPU model");
    CHECK(cpuModel != nullptr);
    if (cpuModel != nullptr) CHECK_EQ(cpuModel->value, std::string{"Tensor G2"});

    const InfoField* uptime = findField("Uptime");
    CHECK(uptime != nullptr);

    // A property with only whitespace must not produce a row.
    CHECK(findField("ro.empty") == nullptr);
}

// ---------------------------------------------------------------------------
// Perfetto
// ---------------------------------------------------------------------------

TEST_CASE("buildTraceConfig emits the requested data sources") {
    PerfettoCaptureOptions options;
    options.durationMs = 5000;
    options.bufferSizeKb = 32768;
    options.atraceCategories = {"gfx", "view"};
    options.atraceApps = {"com.example.app"};
    options.ftraceSchedAndFreq = true;
    options.processStats = true;
    options.frameTimeline = true;
    options.logcat = false;

    const std::string config = buildTraceConfig(options);

    CHECK(config.find("size_kb: 32768") != std::string::npos);
    CHECK(config.find("duration_ms: 5000") != std::string::npos);
    CHECK(config.find("name: \"linux.ftrace\"") != std::string::npos);
    CHECK(config.find("atrace_categories: \"gfx\"") != std::string::npos);
    CHECK(config.find("atrace_categories: \"view\"") != std::string::npos);
    CHECK(config.find("atrace_apps: \"com.example.app\"") != std::string::npos);
    CHECK(config.find("sched/sched_switch") != std::string::npos);
    CHECK(config.find("power/cpu_frequency") != std::string::npos);
    CHECK(config.find("name: \"linux.process_stats\"") != std::string::npos);
    CHECK(config.find("android.surfaceflinger.frametimeline") != std::string::npos);
    CHECK(config.find("android.log") == std::string::npos);
}

TEST_CASE("buildTraceConfig omits ftrace entirely when nothing needs it") {
    PerfettoCaptureOptions options;
    options.atraceCategories.clear();
    options.atraceApps.clear();
    options.ftraceSchedAndFreq = false;
    options.processStats = false;
    options.frameTimeline = false;

    const std::string config = buildTraceConfig(options);
    CHECK(config.find("linux.ftrace") == std::string::npos);
    CHECK(config.find("linux.process_stats") == std::string::npos);
    CHECK(config.find("buffers:") != std::string::npos);
}

TEST_CASE("buildTraceConfig passes a custom config through untouched") {
    PerfettoCaptureOptions options;
    options.customConfig = "buffers: { size_kb: 1024 }\nduration_ms: 1\n";
    CHECK_EQ(buildTraceConfig(options), options.customConfig);
}

TEST_CASE("makeTraceName produces a plausible filename") {
    const std::string name = makeTraceName();
    CHECK(name.starts_with("emeron-"));
    CHECK(name.ends_with(".perfetto-trace"));
}

// ---------------------------------------------------------------------------
// trace_processor output parsing
// ---------------------------------------------------------------------------

TEST_CASE("parseQueryOutput reads quoted CSV") {
    auto parsed = parseQueryOutput("\"name\",\"dur_ms\"\n\"measure\",12.5\n\"draw\",3.25\n");
    CHECK(parsed.hasValue());
    if (!parsed) return;

    const QueryResult& result = parsed.value();
    CHECK_EQ(result.columns.size(), std::size_t{2});
    CHECK_EQ(result.columns[0], std::string{"name"});
    CHECK_EQ(result.rows.size(), std::size_t{2});
    CHECK_EQ(result.rows[0][0], std::string{"measure"});
    CHECK_EQ(result.rows[1][1], std::string{"3.25"});
}

TEST_CASE("parseQueryOutput reads a pipe-delimited table") {
    auto parsed = parseQueryOutput(
        "-------------------------\n"
        "| name     | dur_ms    |\n"
        "-------------------------\n"
        "| measure  | 12.5      |\n"
        "| draw     | 3.25      |\n"
        "-------------------------\n");
    CHECK(parsed.hasValue());
    if (!parsed) return;

    const QueryResult& result = parsed.value();
    CHECK_EQ(result.columns.size(), std::size_t{2});
    CHECK_EQ(result.columns[0], std::string{"name"});
    CHECK_EQ(result.rows.size(), std::size_t{2});
    CHECK_EQ(result.rows[0][1], std::string{"12.5"});
}

TEST_CASE("parseQueryOutput reads tab-separated output") {
    auto parsed = parseQueryOutput("name\tdur_ms\nmeasure\t12.5\n");
    CHECK(parsed.hasValue());
    if (!parsed) return;
    CHECK_EQ(parsed.value().columns.size(), std::size_t{2});
    CHECK_EQ(parsed.value().rows[0][0], std::string{"measure"});
}

TEST_CASE("parseQueryOutput keeps commas inside quoted cells") {
    auto parsed = parseQueryOutput("\"name\",\"n\"\n\"a, b\",2\n");
    CHECK(parsed.hasValue());
    if (!parsed) return;
    CHECK_EQ(parsed.value().rows[0][0], std::string{"a, b"});
}

TEST_CASE("parseQueryOutput surfaces an error line as a failure") {
    auto parsed = parseQueryOutput("Error: no such table: nope\n");
    CHECK(!parsed.hasValue());
    if (!parsed) CHECK(parsed.error().kind == ErrorKind::ParseFailure);
}

TEST_CASE("parseQueryOutput returns empty for a statement with no rows") {
    auto parsed = parseQueryOutput("");
    CHECK(parsed.hasValue());
    if (parsed) CHECK(parsed.value().empty());
}

TEST_CASE("standardQueries are all non-empty") {
    const auto queries = TraceProcessor::standardQueries();
    CHECK(queries.size() >= 5);
    for (const auto& query : queries) {
        CHECK(query.title != nullptr && query.title[0] != '\0');
        CHECK(query.sql != nullptr && query.sql[0] != '\0');
        CHECK(query.note != nullptr && query.note[0] != '\0');
    }
}

// ---------------------------------------------------------------------------
// MetricHistory
// ---------------------------------------------------------------------------

TEST_CASE("MetricHistory grows per-core buffers to match the sample") {
    MetricHistory history;
    history.setCapacity(32);

    MetricSample sample;
    sample.t = 1.0;
    sample.cpuTotalPct = 50.0;
    sample.cpuCorePct = {40.0, 60.0, 20.0, 80.0};
    sample.memUsedMiB = 2048.0;
    history.push(sample);

    CHECK_EQ(history.cpuCores.size(), std::size_t{4});
    CHECK_EQ(history.sampleCount, std::uint64_t{1});
    CHECK_NEAR(history.cpuTotal.back(), 50.0, 1e-9);
    CHECK_NEAR(history.cpuCores[3].back(), 80.0, 1e-9);

    // A device with a different core count mid-session must not corrupt state.
    sample.t = 2.0;
    sample.cpuCorePct = {10.0, 20.0};
    history.push(sample);
    CHECK_EQ(history.cpuCores.size(), std::size_t{2});
    CHECK_EQ(history.t.size(), std::size_t{2});

    history.clear();
    CHECK(history.empty());
    CHECK_EQ(history.sampleCount, std::uint64_t{0});
}

TEST_CASE("cleanTraceProcessorError reduces real stderr to the error and its position") {
    // Captured from trace_processor_shell v58.2 on macOS.
    const std::string stderrText =
        "Loading trace: 0.00 MB\rLoading trace: 0.00 MB"
        "[939.730]     common_flags.cc:525 Trace loaded: 0.00 MB in 0.01s (0.1 MB/s)\n"
        "Traceback (most recent call last):\n"
        "  File \"stdin\" line 1 col 1\n"
        "    SELECT * FROM no_such_table\n"
        "    ^\n"
        "no such table: no_such_table\n";
    CHECK_EQ(cleanTraceProcessorError(stderrText),
             std::string{"line 1 col 1: no such table: no_such_table"});

    const std::string syntax =
        "Traceback (most recent call last):\n"
        "  File \"stdin\" line 1 col 31\n"
        "    SELECT COUNT(*) AS n, NULL AS nothing FROM slice;\n"
        "                                  ^\n"
        "syntax error near 'nothing'\n";
    CHECK_EQ(cleanTraceProcessorError(syntax),
             std::string{"line 1 col 31: syntax error near 'nothing'"});
}

TEST_CASE("cleanTraceProcessorError keeps a message it does not recognise") {
    CHECK_EQ(cleanTraceProcessorError("Could not read trace: file truncated\n"),
             std::string{"Could not read trace: file truncated"});
    CHECK(cleanTraceProcessorError("").empty());
}
