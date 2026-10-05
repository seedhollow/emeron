#include "collect/DeviceInfo.h"

#include <array>
#include <utility>

#include "core/Log.h"
#include "core/StringUtil.h"

namespace em {
namespace {

constexpr const char* kCategory = "deviceinfo";

enum class Extra : std::size_t {
    ScreenSize = 0,
    ScreenDensity,
    Display,
    Storage,
    CpuInfo,
    Uptime,
    Network,
    Selinux,
    Kernel,
    SensorCount,
    Count,
};

constexpr std::size_t extraIndex(Extra e) { return static_cast<std::size_t>(e); }

std::vector<std::string> buildExtraCommands() {
    std::vector<std::string> commands(extraIndex(Extra::Count));
    commands[extraIndex(Extra::ScreenSize)] = "wm size";
    commands[extraIndex(Extra::ScreenDensity)] = "wm density";
    commands[extraIndex(Extra::Display)] =
        "dumpsys display | grep -iE 'refreshRate|mDisplayId|density|real [0-9]|FLAG_' | head -30";
    commands[extraIndex(Extra::Storage)] = "df -h /data /storage/emulated 2>/dev/null";
    commands[extraIndex(Extra::CpuInfo)] = "cat /proc/cpuinfo";
    commands[extraIndex(Extra::Uptime)] = "cat /proc/uptime";
    commands[extraIndex(Extra::Network)] = "ip -o addr 2>/dev/null || ifconfig 2>/dev/null";
    commands[extraIndex(Extra::Selinux)] = "getenforce";
    commands[extraIndex(Extra::Kernel)] = "uname -a";
    commands[extraIndex(Extra::SensorCount)] =
        "dumpsys sensorservice | grep -cE '^\\s*0x[0-9a-f]+\\)'";
    return commands;
}

// Appends a field only when the property exists, so sections stay tidy on
// devices that do not report it.
void addProp(InfoSection& section, const PropertyMap& props, std::string_view label,
             std::string_view key, std::string_view explanation = {}) {
    const auto it = props.find(std::string{key});
    if (it == props.end() || trim(it->second).empty()) return;
    section.fields.push_back(InfoField{std::string{label}, it->second, std::string{key},
                                       std::string{explanation}});
}

void addRaw(InfoSection& section, std::string_view label, std::string_view value,
            std::string_view source = {}, std::string_view explanation = {}) {
    const std::string_view t = trim(value);
    if (t.empty()) return;
    section.fields.push_back(InfoField{std::string{label}, std::string{t}, std::string{source},
                                       std::string{explanation}});
}

// Pulls the model name and core count out of /proc/cpuinfo.
std::pair<std::string, int> summarizeCpuInfo(std::string_view cpuInfo) {
    std::string model;
    int processors = 0;
    for (const auto line : splitLines(cpuInfo)) {
        if (line.starts_with("processor")) {
            ++processors;
        } else if (model.empty()) {
            if (const auto v = fieldAfter(line, "Hardware")) {
                model = std::string{*v};
            } else if (const auto v2 = fieldAfter(line, "model name")) {
                model = std::string{*v2};
            } else if (const auto v3 = fieldAfter(line, "CPU part")) {
                model = "ARM part " + std::string{*v3};
            }
        }
    }
    return {model, processors};
}

std::string firstLine(std::string_view text) {
    const auto lines = splitLines(text);
    return lines.empty() ? std::string{} : std::string{trim(lines.front())};
}

}  // namespace

std::vector<InfoSection> buildInfoSections(const PropertyMap& props,
                                           const DeviceInfoExtras& extras) {
    std::vector<InfoSection> sections;

    {
        InfoSection s{"Identity", {}};
        addProp(s, props, "Manufacturer", "ro.product.manufacturer");
        addProp(s, props, "Brand", "ro.product.brand");
        addProp(s, props, "Model", "ro.product.model");
        addProp(s, props, "Marketing name", "ro.product.marketname");
        addProp(s, props, "Device codename", "ro.product.device",
                "Internal name of the hardware design.");
        addProp(s, props, "Product codename", "ro.product.name",
                "Internal name of this product variant (region, carrier...).");
        addProp(s, props, "Serial number", "ro.serialno");
        addProp(s, props, "Hardware platform", "ro.hardware");
        addProp(s, props, "Board", "ro.product.board", "Name of the main circuit board.");
        if (!s.fields.empty()) sections.push_back(std::move(s));
    }
    {
        InfoSection s{"OS and build", {}};
        addProp(s, props, "Android version", "ro.build.version.release");
        addProp(s, props, "API level", "ro.build.version.sdk",
                "The number to compare with minSdk / targetSdk in your build.gradle "
                "(e.g. 34 = Android 14).");
        addProp(s, props, "Security patch", "ro.build.version.security_patch");
        addProp(s, props, "Build ID", "ro.build.id");
        addProp(s, props, "Build fingerprint", "ro.build.fingerprint",
                "Unique id of this exact firmware build. Useful in bug reports.");
        addProp(s, props, "Build type", "ro.build.type",
                "user = normal retail phone; userdebug / eng = developer builds with more "
                "access.");
        addProp(s, props, "Build signing", "ro.build.tags",
                "release-keys on retail firmware; test-keys or dev-keys on custom or "
                "development builds.");
        addProp(s, props, "Build date", "ro.build.date");
        addProp(s, props, "Bootloader version", "ro.bootloader");
        addProp(s, props, "Modem firmware", "gsm.version.baseband",
                "Baseband: firmware of the cellular radio.");
        addRaw(s, "Linux kernel", extras.kernel, "uname -a");
        addRaw(s, "Security policy", extras.selinux, "getenforce",
                "SELinux mode. Enforcing is normal; Permissive means the security "
                "policy is only logging, not blocking.");
        if (!extras.uptime.empty()) {
            if (const auto seconds = parseNumber<double>(tokenize(extras.uptime).empty()
                                                             ? std::string_view{}
                                                             : tokenize(extras.uptime).front())) {
                addRaw(s, "Uptime", humanDuration(*seconds), "/proc/uptime");
            }
        }
        if (!s.fields.empty()) sections.push_back(std::move(s));
    }
    {
        InfoSection s{"SoC and CPU", {}};
        addProp(s, props, "Chip maker", "ro.soc.manufacturer",
                "SoC (system on a chip) manufacturer: the company behind the main "
                "processor.");
        addProp(s, props, "Chip model", "ro.soc.model");
        addProp(s, props, "Chip platform", "ro.board.platform");
        addProp(s, props, "CPU architecture", "ro.product.cpu.abi",
                "Primary ABI. The native-library folder (jniLibs/<abi>) the device "
                "prefers, e.g. arm64-v8a.");
        addProp(s, props, "Supported architectures", "ro.product.cpu.abilist",
                "Every ABI the device can run native code for, in order of preference.");
        const auto [cpuModel, cores] = summarizeCpuInfo(extras.cpuInfo);
        addRaw(s, "CPU model", cpuModel, "/proc/cpuinfo");
        if (cores > 0) addRaw(s, "CPU cores", std::to_string(cores), "/proc/cpuinfo");
        addProp(s, props, "32-bit architectures", "ro.product.cpu.abilist32",
                "ABIs for 32-bit native code. Empty on 64-bit-only devices.");
        addProp(s, props, "64-bit architectures", "ro.product.cpu.abilist64");
        if (!s.fields.empty()) sections.push_back(std::move(s));
    }
    {
        InfoSection s{"Display and graphics", {}};
        addRaw(s, "Resolution", firstLine(extras.screenSize), "wm size");
        addRaw(s, "Pixel density", firstLine(extras.screenDensity), "wm density",
               "Dots per inch (dpi). 160 dpi means 1 dp = 1 px; 480 dpi means 1 dp = 3 px.");
        addProp(s, props, "GPU driver", "ro.gfx.driver.0");
        addProp(s, props, "OpenGL ES version", "ro.opengles.version",
                "Encoded as major << 16 | minor, e.g. 196610 = 3.2.");
        addProp(s, props, "Screen mounted rotated", "ro.sf.hwrotation",
                "How many degrees the panel is physically rotated in the device.");
        addProp(s, props, "Idle refresh timeout", "ro.surface_flinger.set_idle_timer_ms",
                "After this many ms without changes the screen may drop to a lower "
                "refresh rate to save power.");
        // The display dump is multi-line and highly release-specific, so it is
        // surfaced verbatim for the user to read rather than parsed.
        addRaw(s, "Display details", extras.displayDump, "dumpsys display");
        if (!s.fields.empty()) sections.push_back(std::move(s));
    }
    {
        InfoSection s{"Memory and storage", {}};
        addProp(s, props, "App memory limit (largeHeap)", "dalvik.vm.heapsize",
                "Most Java/Kotlin memory an app can use when its manifest sets "
                "android:largeHeap=\"true\".");
        addProp(s, props, "App memory limit", "dalvik.vm.heapgrowthlimit",
                "Most Java/Kotlin memory a normal app can use before OutOfMemoryError. "
                "Same as ActivityManager.getMemoryClass().");
        addProp(s, props, "Low-memory device", "ro.config.low_ram",
                "true on Android Go / low-RAM devices, where the system trims apps "
                "more aggressively.");
        addRaw(s, "Storage", extras.storageDump, "df -h");
        if (!s.fields.empty()) sections.push_back(std::move(s));
    }
    {
        InfoSection s{"Network", {}};
        addProp(s, props, "Carrier", "gsm.operator.alpha");
        addProp(s, props, "Mobile network type", "gsm.network.type");
        addProp(s, props, "SIM state", "gsm.sim.state");
        addRaw(s, "Network addresses", extras.networkDump, "ip -o addr",
               "Every network interface (Wi-Fi, mobile data, ...) and its IP address.");
        if (!s.fields.empty()) sections.push_back(std::move(s));
    }
    {
        InfoSection s{"Debug and tracing", {}};
        addProp(s, props, "USB debugging needs approval", "ro.adb.secure",
                "1 means a computer must be authorised on the phone before adb works.");
        addProp(s, props, "Debug build", "ro.debuggable",
                "1 on userdebug/eng firmware: every app can be debugged and adb can run "
                "as root. 0 on retail phones.");
        addProp(s, props, "Locked down", "ro.secure",
                "1 means adb runs as a normal user, not root.");
        addProp(s, props, "Tracing service", "init.svc.traced",
                "Perfetto's traced daemon. Needs to be \"running\" to record traces.");
        addProp(s, props, "Tracing data sources", "init.svc.traced_probes",
                "Perfetto's traced_probes daemon, which collects system data for traces.");
        addProp(s, props, "Tracing enabled", "persist.traced.enable",
                "1 when Perfetto tracing is switched on on the device.");
        addRaw(s, "Sensors found", trim(extras.sensorCount), "dumpsys sensorservice");
        if (!s.fields.empty()) sections.push_back(std::move(s));
    }
    return sections;
}

DeviceInfoCollector::DeviceInfoCollector(DeviceManager& devices) : devices_(devices) {}

void DeviceInfoCollector::clear() {
    snapshot_.set(DeviceInfoSnapshot{});
}

void DeviceInfoCollector::refresh(ThreadPool& pool) {
    if (inFlight_.exchange(true, std::memory_order_acq_rel)) return;

    const std::string serial = devices_.selectedSerial();
    if (serial.empty()) {
        inFlight_.store(false, std::memory_order_release);
        snapshot_.mutate([](DeviceInfoSnapshot& s) {
            s = DeviceInfoSnapshot{};
            s.lastError = "no device selected";
        });
        return;
    }

    snapshot_.mutate([](DeviceInfoSnapshot& s) { s.loading = true; });

    pool.submit([this, serial] {
        DeviceInfoSnapshot next;
        next.serial = serial;

        auto props = devices_.adb().properties(serial);
        if (!props) {
            next.lastError = props.error().message;
            next.loading = false;
            snapshot_.set(std::move(next));
            inFlight_.store(false, std::memory_order_release);
            return;
        }
        next.properties = std::move(props).value();

        DeviceInfoExtras extras;
        const auto commands = buildExtraCommands();
        if (auto batch = devices_.adb().shellBatch(serial, commands, kAdbLongTimeout)) {
            const auto parts = std::move(batch).value();
            extras.screenSize = parts[extraIndex(Extra::ScreenSize)];
            extras.screenDensity = parts[extraIndex(Extra::ScreenDensity)];
            extras.displayDump = parts[extraIndex(Extra::Display)];
            extras.storageDump = parts[extraIndex(Extra::Storage)];
            extras.cpuInfo = parts[extraIndex(Extra::CpuInfo)];
            extras.uptime = parts[extraIndex(Extra::Uptime)];
            extras.networkDump = parts[extraIndex(Extra::Network)];
            extras.selinux = parts[extraIndex(Extra::Selinux)];
            extras.kernel = parts[extraIndex(Extra::Kernel)];
            extras.sensorCount = parts[extraIndex(Extra::SensorCount)];
        } else {
            next.lastError = "extra probes failed: " + batch.error().message;
        }

        next.cpuInfo = extras.cpuInfo;
        next.sections = buildInfoSections(next.properties, extras);
        next.loaded = true;
        next.loading = false;

        EM_LOG_INFO(kCategory, "collected " + std::to_string(next.properties.size()) +
                                   " properties from " + serial);
        snapshot_.set(std::move(next));
        inFlight_.store(false, std::memory_order_release);
    });
}

}  // namespace em
