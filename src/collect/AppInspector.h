#pragma once

// Installed-app inventory and per-app detail, for the Apps panel.
//
// The list is one batched adb round trip (`pm list packages` in three flavours
// plus `ps` and the storage stats). The detail view is another: the full
// `dumpsys package <pkg>` plus the APK files, launcher activity, app ops,
// memory and storage. Everything is parsed on a worker thread and handed back
// through the Dispatcher, so the UI never blocks on the device.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adb/DeviceManager.h"
#include "collect/ApkAnalysis.h"
#include "core/TaskQueue.h"

namespace em {

// --- the list -------------------------------------------------------------

struct InstalledApp {
    std::string packageName;
    std::string apkPath;      // base APK, from `pm list packages -f`
    std::string installer;    // "null" for preinstalled apps, as pm prints it
    std::int64_t versionCode = -1;
    int uid = -1;
    bool system = false;      // not in `pm list packages -3`
    bool disabled = false;    // in `pm list packages -d`
    bool running = false;     // a process named after the package exists
    // From `dumpsys diskstats`, which the system refreshes about once a day;
    // -1 when the device does not report the package.
    std::int64_t appBytes = -1;
    std::int64_t dataBytes = -1;
    std::int64_t cacheBytes = -1;

    [[nodiscard]] std::int64_t totalBytes() const noexcept {
        if (appBytes < 0) return -1;
        return appBytes + std::max<std::int64_t>(dataBytes, 0) +
               std::max<std::int64_t>(cacheBytes, 0);
    }
};

struct AppListResult {
    std::vector<InstalledApp> apps;
    std::string error;
};

// --- the detail view --------------------------------------------------------

struct PermissionGrant {
    std::string name;
    bool granted = false;
    std::string flags;  // runtime permissions only, e.g. "USER_SET|USER_FIXED"
};

struct AppComponent {
    enum class Kind { Activity, Service, Receiver, Provider };
    Kind kind = Kind::Activity;
    std::string className;             // fully qualified
    std::vector<std::string> actions;  // intent-filter actions, de-duplicated
    std::string permission;            // required to start/bind/send, if any
    std::vector<std::string> authorities;  // providers only
};

struct ApkFile {
    std::string path;
    std::int64_t sizeBytes = -1;
};

struct MemoryRow {
    std::string label;      // "Java Heap", "Native Heap", ..., "TOTAL"
    std::int64_t pssKb = -1;
    std::int64_t rssKb = -1;
};

struct DexoptEntry {
    std::string apk;     // file name, e.g. base.apk
    std::string isa;     // arm64, arm, x86_64...
    std::string status;  // speed-profile, verify, ...
    std::string reason;  // why it was compiled that way
};

struct AppDetails {
    std::string packageName;
    std::string error;
    bool found = false;

    // Every `key=value` the package block printed, in order. The typed fields
    // below are pulled out of this; anything not given a field of its own is
    // still shown from here, so nothing the device reports is dropped.
    std::vector<std::pair<std::string, std::string>> fields;
    // Every list the package block printed ("requested permissions:",
    // "usesLibraries:", "overlay paths:"...), keyed by its header.
    std::map<std::string, std::vector<std::string>> lists;

    std::string versionName;
    std::int64_t versionCode = -1;
    int minSdk = -1;
    int targetSdk = -1;
    int appId = -1;
    std::string sharedUser;
    std::string codePath;
    std::string dataDir;
    std::string primaryCpuAbi;
    std::vector<std::string> splits;
    std::vector<std::string> flags;
    std::vector<std::string> privateFlags;
    int signingVersion = -1;
    std::vector<std::string> signatures;  // certificate digests, short form
    std::string installer;
    std::string firstInstallTime;
    std::string lastUpdateTime;

    // User 0 state.
    bool installed = true;
    bool hidden = false;
    bool suspended = false;
    bool stopped = false;
    bool notLaunched = false;
    int enabledState = 0;  // 0 default, 1 enabled, 2 disabled, 3 disabled by user...

    std::vector<std::string> requestedPermissions;
    std::vector<std::string> declaredPermissions;  // "name: prot=..."
    std::vector<PermissionGrant> installPermissions;
    std::vector<PermissionGrant> runtimePermissions;
    std::vector<std::string> enabledComponents;
    std::vector<std::string> disabledComponents;

    std::vector<AppComponent> components;

    // An updated system app keeps its factory copy as a hidden package.
    bool updatedSystemApp = false;
    std::string factoryVersionName;

    std::vector<DexoptEntry> dexopt;

    // From the extra commands in the same batch.
    std::vector<ApkFile> apks;
    std::vector<int> pids;
    std::string launcherActivity;  // pkg/cls, empty when there is none
    std::vector<std::pair<std::string, std::string>> appOps;
    std::vector<MemoryRow> memory;  // empty when the app is not running
    std::int64_t appBytes = -1;
    std::int64_t dataBytes = -1;
    std::int64_t cacheBytes = -1;

    std::string rawDump;

    [[nodiscard]] bool isSystem() const;
    [[nodiscard]] bool hasFlag(std::string_view flag) const;
};

// --- APK contents (native code and signature) ------------------------------

struct NativeLibrary {
    std::string apk;    // file name of the APK it is in, e.g. split_config.arm64_v8a.apk
    std::string abi;    // folder under lib/, e.g. arm64-v8a
    std::string name;   // libfoo.so
    std::uint64_t sizeBytes = 0;
    bool compressed = false;  // deflated in the APK (extractNativeLibs=true)
    std::int64_t dataOffset = -1;  // where the uncompressed bytes start, stored libs only
    ElfInfo elf;

    [[nodiscard]] bool is64Bit() const { return abi == "arm64-v8a" || abi == "x86_64" || abi == "riscv64"; }
    // ELF segments aligned for 16 KB pages.
    [[nodiscard]] bool elfAligned16k() const { return elf.valid && elf.loadAlignment >= 16384; }
    // Stored libraries are mapped straight out of the APK, so the ZIP data
    // must start on a 16 KB boundary too. Compressed ones are extracted first.
    [[nodiscard]] bool zipAligned16k() const { return compressed || (dataOffset >= 0 && dataOffset % 16384 == 0); }
    [[nodiscard]] bool ready16k() const { return !is64Bit() || (elfAligned16k() && zipAligned16k()); }
};

struct ApkContents {
    std::string packageName;
    std::string error;
    std::vector<NativeLibrary> libraries;

    // From base.apk; every split must be signed with the same key.
    SigningBlock signing;
    bool hasV1Signature = false;  // META-INF/*.SF + .RSA|.DSA|.EC
    std::vector<CertificateInfo> v1Certificates;  // only read when there is no v2/v3
};

// --- actions ----------------------------------------------------------------

enum class AppAction {
    Launch,
    ForceStop,
    OpenSettings,  // the app's page in Android Settings, on the device
    ClearData,
    Uninstall,
    GrantPermission,
    RevokePermission,
};

struct AppActionResult {
    bool ok = false;
    std::string message;
};

class AppInspector {
public:
    explicit AppInspector(DeviceManager& devices);

    void refreshList(ThreadPool& pool, Dispatcher& dispatcher,
                     std::function<void(AppListResult)> onDone);

    void loadDetails(ThreadPool& pool, Dispatcher& dispatcher, std::string packageName,
                     std::function<void(AppDetails)> onDone);

    // `argument` is the permission for Grant/Revoke and the launcher
    // component for Launch (empty falls back to monkey).
    void runAction(ThreadPool& pool, Dispatcher& dispatcher, AppAction action,
                   std::string packageName, std::string argument,
                   std::function<void(AppActionResult)> onDone);

    // Reads the native libraries and signing certificates straight out of the
    // APK files on the device, a few small byte ranges at a time.
    void analyzeApks(ThreadPool& pool, Dispatcher& dispatcher, std::string packageName,
                     std::vector<ApkFile> apks, std::function<void(ApkContents)> onDone);

    // Pulls every APK of a package (base and splits) into `destination`.
    void pullApks(ThreadPool& pool, Dispatcher& dispatcher, std::vector<std::string> remotePaths,
                  std::filesystem::path destination,
                  std::function<void(AppActionResult)> onDone);

private:
    DeviceManager& devices_;
};

// --- parsers, exposed for unit tests ---------------------------------------

// Package names and permission names are interpolated into shell commands, so
// anything outside [A-Za-z0-9._] is rejected rather than quoted.
[[nodiscard]] bool isSafeAndroidName(std::string_view name) noexcept;

// `pm list packages -f -U -i --show-versioncode`, any subset of those flags.
[[nodiscard]] std::vector<InstalledApp> parsePackageList(std::string_view text);
// Plain `pm list packages` output, names only.
[[nodiscard]] std::vector<std::string> parsePackageNames(std::string_view text);

struct DiskStats {
    std::int64_t appBytes = -1;
    std::int64_t dataBytes = -1;
    std::int64_t cacheBytes = -1;
};
// The "Package Names:" / "App Sizes:" / ... arrays of `dumpsys diskstats`.
[[nodiscard]] std::map<std::string, DiskStats> parseDiskStats(std::string_view text);

[[nodiscard]] AppDetails parsePackageDump(std::string_view dump, std::string_view packageName);

[[nodiscard]] std::vector<ApkFile> parseApkSizes(std::string_view statOutput);
[[nodiscard]] std::string parseLauncherActivity(std::string_view resolveOutput);
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parseAppOps(std::string_view text);
[[nodiscard]] std::vector<MemoryRow> parseMemorySummary(std::string_view text);

}  // namespace em
