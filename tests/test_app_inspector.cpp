#include "TestHarness.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "collect/AppInspector.h"

using namespace em;

namespace {

// Lines starting with '#' are the fixture's own notes, not device output.
std::string loadFixture(const char* name) {
    std::ifstream in{std::string{EMERON_TEST_DATA_DIR} + "/" + name};
    std::string out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with('#')) continue;
        out += line;
        out += '\n';
    }
    return out;
}

const AppComponent* findComponent(const AppDetails& d, AppComponent::Kind kind,
                                  std::string_view cls) {
    for (const auto& c : d.components) {
        if (c.kind == kind && c.className == cls) return &c;
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

TEST_CASE("parsePackageList reads the Android 14 -f -U -i --show-versioncode format") {
    const auto apps = parsePackageList(
        "package:/system/priv-app/Shell/Shell.apk=com.android.shell versionCode:34  "
        "installer=null uid:2000\n"
        "package:/data/app/~~kOIGpXoYPtm22B_lxcqjCg==/com.android.chrome-XVd3nwkxTcK52jBCLOFMzQ==/"
        "base.apk=com.android.chrome versionCode:803709404  installer=com.android.vending uid:10131\n");

    CHECK_EQ(apps.size(), std::size_t{2});
    if (apps.size() != 2) return;
    // Sorted by package name.
    const InstalledApp& chrome = apps[0];
    CHECK_EQ(chrome.packageName, std::string{"com.android.chrome"});
    // The path itself contains '=': the package starts after the LAST one.
    CHECK_EQ(chrome.apkPath, std::string{"/data/app/~~kOIGpXoYPtm22B_lxcqjCg==/"
                                         "com.android.chrome-XVd3nwkxTcK52jBCLOFMzQ==/base.apk"});
    CHECK_EQ(chrome.versionCode, std::int64_t{803709404});
    CHECK_EQ(chrome.installer, std::string{"com.android.vending"});
    CHECK_EQ(chrome.uid, 10131);

    CHECK_EQ(apps[1].packageName, std::string{"com.android.shell"});
    CHECK_EQ(apps[1].uid, 2000);
}

TEST_CASE("parsePackageList accepts the bare format of older releases") {
    const auto apps = parsePackageList("package:com.b\npackage:/system/app/A.apk=com.a\n\n");
    CHECK_EQ(apps.size(), std::size_t{2});
    if (apps.size() != 2) return;
    CHECK_EQ(apps[0].packageName, std::string{"com.a"});
    CHECK_EQ(apps[0].apkPath, std::string{"/system/app/A.apk"});
    CHECK_EQ(apps[0].versionCode, std::int64_t{-1});
    CHECK_EQ(apps[1].packageName, std::string{"com.b"});
}

TEST_CASE("parsePackageList drops names that are not safe to put in a shell command") {
    const auto apps = parsePackageList("package:com.ok\npackage:com.bad;rm\npackage:$(x)\n");
    CHECK_EQ(apps.size(), std::size_t{1});
}

TEST_CASE("isSafeAndroidName") {
    CHECK(isSafeAndroidName("com.example.app"));
    CHECK(isSafeAndroidName("android.permission.CAMERA"));
    CHECK(!isSafeAndroidName(""));
    CHECK(!isSafeAndroidName("com.a com.b"));
    CHECK(!isSafeAndroidName("com.a'"));
    CHECK(!isSafeAndroidName("com.a/b"));
}

TEST_CASE("parseDiskStats lines up the parallel arrays by index") {
    const auto stats = parseDiskStats(
        "Package Names: [\"com.a\",\"com.b\"]\n"
        "App Sizes: [100,200]\n"
        "App Data Sizes: [10,20]\n"
        "Cache Sizes: [1,2]\n"
        "App Size: 300\n");  // the device-wide total, not an array
    CHECK_EQ(stats.size(), std::size_t{2});
    CHECK_EQ(stats.at("com.b").appBytes, std::int64_t{200});
    CHECK_EQ(stats.at("com.b").dataBytes, std::int64_t{20});
    CHECK_EQ(stats.at("com.b").cacheBytes, std::int64_t{2});
}

// ---------------------------------------------------------------------------
// The real Chrome dump
// ---------------------------------------------------------------------------

TEST_CASE("parsePackageDump reads Chrome's package block from a real Android 14 dump") {
    const std::string dump = loadFixture("dumpsys_package_chrome.txt");
    const AppDetails d = parsePackageDump(dump, "com.android.chrome");

    CHECK(d.found);
    CHECK(d.error.empty());
    CHECK_EQ(d.versionName, std::string{"154.0.8037.94"});
    CHECK_EQ(d.versionCode, std::int64_t{803709404});
    CHECK_EQ(d.minSdk, 32);
    CHECK_EQ(d.targetSdk, 36);
    CHECK_EQ(d.installer, std::string{"com.android.vending"});
    CHECK_EQ(d.signingVersion, 3);
    CHECK_EQ(d.signatures.size(), std::size_t{1});
    if (!d.signatures.empty()) CHECK_EQ(d.signatures[0], std::string{"e3ca78d8"});
    CHECK_EQ(d.lastUpdateTime, std::string{"2026-10-04 01:03:07"});
    CHECK_EQ(d.primaryCpuAbi, std::string{"arm64-v8a"});
    CHECK(std::find(d.splits.begin(), d.splits.end(), "base") != d.splits.end());
    CHECK(std::find(d.splits.begin(), d.splits.end(), "stack_unwinder") != d.splits.end());
    CHECK(d.isSystem());  // an updated system app keeps its SYSTEM flag

    // The factory copy lives in "Hidden system packages", and must not
    // overwrite the installed version.
    CHECK(d.updatedSystemApp);
    CHECK_EQ(d.factoryVersionName, std::string{"121.0.6167.178"});
}

TEST_CASE("parsePackageDump reads user 0 state and permissions") {
    const AppDetails d = parsePackageDump(loadFixture("dumpsys_package_chrome.txt"),
                                          "com.android.chrome");
    CHECK(d.installed);
    CHECK(!d.stopped);
    CHECK_EQ(d.firstInstallTime, std::string{"2009-01-01 07:00:00"});

    CHECK_EQ(d.requestedPermissions.size(), std::size_t{4});  // the fixture keeps four
    CHECK_EQ(d.installPermissions.size(), std::size_t{4});
    if (!d.installPermissions.empty()) CHECK(d.installPermissions[0].granted);

    CHECK_EQ(d.runtimePermissions.size(), std::size_t{4});
    if (d.runtimePermissions.size() == 4) {
        CHECK_EQ(d.runtimePermissions[0].name, std::string{"android.permission.POST_NOTIFICATIONS"});
        CHECK(d.runtimePermissions[0].granted);
        CHECK(d.runtimePermissions[0].flags.find("USER_SET") != std::string::npos);
        CHECK(!d.runtimePermissions[2].granted);
    }

    CHECK_EQ(d.disabledComponents.size(), std::size_t{2});
    CHECK_EQ(d.enabledComponents.size(), std::size_t{3});
    // Lists with no typed field are still kept.
    CHECK(d.lists.contains("overlay paths"));
}

TEST_CASE("parsePackageDump collects components from the resolver tables") {
    const AppDetails d = parsePackageDump(loadFixture("dumpsys_package_chrome.txt"),
                                          "com.android.chrome");

    const auto* dispatcher = findComponent(d, AppComponent::Kind::Activity,
                                           "com.google.android.apps.chrome.IntentDispatcher");
    CHECK(dispatcher != nullptr);
    if (dispatcher != nullptr) {
        CHECK(std::find(dispatcher->actions.begin(), dispatcher->actions.end(),
                        "android.intent.action.VIEW") != dispatcher->actions.end());
    }

    const auto* payments = findComponent(
        d, AppComponent::Kind::Service,
        "org.chromium.components.payments.GooglePayDataCallbacksService");
    CHECK(payments != nullptr);
    if (payments != nullptr) {
        CHECK_EQ(payments->permission,
                 std::string{"com.google.android.gms.permission.BIND_PAYMENTS_CALLBACK_SERVICE"});
    }

    const auto* receiver = findComponent(
        d, AppComponent::Kind::Receiver,
        "org.chromium.chrome.browser.browserservices.InstalledWebappBroadcastReceiver");
    CHECK(receiver != nullptr);
    if (receiver != nullptr) CHECK_EQ(receiver->actions.size(), std::size_t{2});

    const auto* screenshot = findComponent(
        d, AppComponent::Kind::Provider,
        "org.chromium.chrome.browser.screenshotprovider.ScreenshotContentProvider");
    CHECK(screenshot != nullptr);
    if (screenshot != nullptr) {
        CHECK_EQ(screenshot->authorities.size(), std::size_t{1});
    }
    CHECK(findComponent(d, AppComponent::Kind::Provider,
                        "org.chromium.chrome.browser.provider.ChromeBrowserProvider") != nullptr);
}

TEST_CASE("parsePackageDump reads the dexopt state of the package only") {
    const AppDetails d = parsePackageDump(loadFixture("dumpsys_package_chrome.txt"),
                                          "com.android.chrome");
    CHECK(d.dexopt.size() >= 2);
    if (d.dexopt.empty()) return;
    CHECK_EQ(d.dexopt[0].apk, std::string{"base.apk"});
    CHECK_EQ(d.dexopt[0].isa, std::string{"arm64"});
    CHECK_EQ(d.dexopt[0].status, std::string{"speed-profile"});
    CHECK_EQ(d.dexopt[0].reason, std::string{"cmdline"});
}

TEST_CASE("parsePackageDump reports a package the dump does not contain") {
    const AppDetails d = parsePackageDump(loadFixture("dumpsys_package_chrome.txt"),
                                          "com.example.missing");
    CHECK(!d.found);
    CHECK(!d.error.empty());
    CHECK(d.components.empty());  // Chrome's components must not leak in
}

TEST_CASE("parsePackageDump skips users other than 0") {
    const AppDetails d = parsePackageDump(
        "Packages:\n"
        "  Package [com.a] (1):\n"
        "    versionName=1.0\n"
        "    User 0: installed=true stopped=false\n"
        "      runtime permissions:\n"
        "        android.permission.CAMERA: granted=true, flags=[ USER_SET]\n"
        "    User 10: installed=false stopped=true\n"
        "      runtime permissions:\n"
        "        android.permission.CAMERA: granted=false, flags=[ ]\n",
        "com.a");
    CHECK(d.installed);
    CHECK(!d.stopped);
    CHECK_EQ(d.runtimePermissions.size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// The extra commands
// ---------------------------------------------------------------------------

TEST_CASE("parseApkSizes puts base.apk first") {
    const auto apks = parseApkSizes(
        "8220879 /data/app/x/split_chrome.apk\n"
        "225032136 /data/app/x/base.apk\n");
    CHECK_EQ(apks.size(), std::size_t{2});
    if (apks.size() != 2) return;
    CHECK_EQ(apks[0].path, std::string{"/data/app/x/base.apk"});
    CHECK_EQ(apks[0].sizeBytes, std::int64_t{225032136});
}

TEST_CASE("parseLauncherActivity") {
    CHECK_EQ(parseLauncherActivity(
                 "priority=0 preferredOrder=0 match=0x108000 specificIndex=-1 isDefault=true\n"
                 "com.android.chrome/com.google.android.apps.chrome.Main\n"),
             std::string{"com.android.chrome/com.google.android.apps.chrome.Main"});
    CHECK(parseLauncherActivity("No activity found\n").empty());
}

TEST_CASE("parseAppOps marks the uid-wide modes") {
    const auto ops = parseAppOps(
        "Uid mode: COARSE_LOCATION: foreground\n"
        "FINE_LOCATION: foreground\n"
        "READ_CONTACTS: ignore\n"
        "  some unrelated note\n");
    CHECK_EQ(ops.size(), std::size_t{3});
    if (ops.size() != 3) return;
    CHECK_EQ(ops[0].first, std::string{"COARSE_LOCATION (whole app)"});
    CHECK_EQ(ops[0].second, std::string{"foreground"});
    CHECK_EQ(ops[2].second, std::string{"ignore"});
}

TEST_CASE("parseMemorySummary tells the PSS column from the RSS column") {
    const auto rows = parseMemorySummary(
        " App Summary\n"
        "                       Pss(KB)                        Rss(KB)\n"
        "                        ------                         ------\n"
        "           Java Heap:    40080                          55020\n"
        "       Private Other:    22532\n"
        "             Unknown:                                    8152\n"
        " \n"
        "           TOTAL PSS:   477443            TOTAL RSS:   423416       TOTAL SWAP PSS:   149829\n");
    CHECK_EQ(rows.size(), std::size_t{4});
    if (rows.size() != 4) return;
    CHECK_EQ(rows[0].label, std::string{"Java Heap"});
    CHECK_EQ(rows[0].pssKb, std::int64_t{40080});
    CHECK_EQ(rows[0].rssKb, std::int64_t{55020});
    CHECK_EQ(rows[1].pssKb, std::int64_t{22532});
    CHECK_EQ(rows[1].rssKb, std::int64_t{-1});
    CHECK_EQ(rows[2].pssKb, std::int64_t{-1});
    CHECK_EQ(rows[2].rssKb, std::int64_t{8152});
    CHECK_EQ(rows[3].label, std::string{"Total"});
    CHECK_EQ(rows[3].pssKb, std::int64_t{477443});
    CHECK_EQ(rows[3].rssKb, std::int64_t{423416});
}

// ---------------------------------------------------------------------------
// Actions, through a recording fake transport
// ---------------------------------------------------------------------------

#include <chrono>
#include <mutex>
#include <thread>

#include "adb/AdbClient.h"
#include "adb/DeviceManager.h"
#include "core/TaskQueue.h"

namespace {

class RecordingTransport final : public IAdbTransport {
public:
    std::string reply;  // stdout for every shell command

    [[nodiscard]] bool available() const override { return true; }
    [[nodiscard]] std::string describe() const override { return "fake"; }
    Result<CommandOutput> host(std::span<const std::string>, std::chrono::milliseconds) override {
        return CommandOutput{};
    }
    Result<CommandOutput> device(std::string_view, std::span<const std::string>,
                                 std::chrono::milliseconds) override {
        return CommandOutput{};
    }
    Result<CommandOutput> shell(std::string_view, std::string_view command,
                                std::chrono::milliseconds) override {
        const std::lock_guard lock{mutex};
        commands.emplace_back(command);
        CommandOutput out;
        out.out = reply;
        return out;
    }
    Result<std::unique_ptr<Subprocess>> stream(std::string_view, std::span<const std::string>,
                                               bool) override {
        return makeError(ErrorKind::Unsupported, "fake");
    }

    std::vector<std::string> recorded() {
        const std::lock_guard lock{mutex};
        return commands;
    }

private:
    std::mutex mutex;
    std::vector<std::string> commands;
};

struct ActionRig {
    std::shared_ptr<RecordingTransport> transport = std::make_shared<RecordingTransport>();
    DeviceManager devices{std::make_shared<AdbClient>(transport)};
    ThreadPool pool{1};
    Dispatcher dispatcher;
    AppInspector apps{devices};

    ActionRig() { devices.selectSerial("FAKE01"); }

    AppActionResult run(AppAction action, std::string pkg, std::string argument = {}) {
        std::optional<AppActionResult> result;
        apps.runAction(pool, dispatcher, action, std::move(pkg), std::move(argument),
                       [&result](AppActionResult r) { result = std::move(r); });
        for (int i = 0; i < 500 && !result; ++i) {
            dispatcher.drain();
            if (!result) std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return result.value_or(AppActionResult{false, "timed out"});
    }
};

}  // namespace

TEST_CASE("AppInspector actions send the expected device commands") {
    ActionRig rig;
    rig.transport->reply = "Success\n";

    CHECK(rig.run(AppAction::ForceStop, "com.example.app").ok);
    CHECK(rig.run(AppAction::ClearData, "com.example.app").ok);
    CHECK(rig.run(AppAction::Uninstall, "com.example.app").ok);
    rig.transport->reply.clear();
    CHECK(rig.run(AppAction::GrantPermission, "com.example.app", "android.permission.CAMERA").ok);
    CHECK(rig.run(AppAction::RevokePermission, "com.example.app", "android.permission.CAMERA").ok);
    CHECK(rig.run(AppAction::Launch, "com.example.app", "com.example.app/.Main$Inner").ok);
    CHECK(rig.run(AppAction::Launch, "com.example.app").ok);

    const auto commands = rig.transport->recorded();
    CHECK_EQ(commands.size(), std::size_t{7});
    if (commands.size() != 7) return;
    CHECK_EQ(commands[0], std::string{"am force-stop com.example.app"});
    CHECK_EQ(commands[1], std::string{"pm clear com.example.app"});
    CHECK_EQ(commands[2], std::string{"pm uninstall com.example.app"});
    CHECK_EQ(commands[3], std::string{"pm grant com.example.app android.permission.CAMERA"});
    CHECK_EQ(commands[4], std::string{"pm revoke com.example.app android.permission.CAMERA"});
    // '$' in an inner-class name must reach am literally, not be expanded.
    CHECK_EQ(commands[5], std::string{"am start -n 'com.example.app/.Main$Inner'"});
    // No launcher activity known: fall back to monkey.
    CHECK_EQ(commands[6],
             std::string{"monkey -p com.example.app -c android.intent.category.LAUNCHER 1"});
}

TEST_CASE("AppInspector refuses unsafe names without running anything") {
    ActionRig rig;
    CHECK(!rig.run(AppAction::Uninstall, "com.a; rm -rf /sdcard").ok);
    CHECK(!rig.run(AppAction::GrantPermission, "com.example.app", "x; reboot").ok);
    CHECK(!rig.run(AppAction::Launch, "com.example.app'").ok);
    CHECK(rig.transport->recorded().empty());
}

TEST_CASE("AppInspector reports pm's stdout failures as failures") {
    // pm prints most failures on stdout and still exits 0.
    ActionRig rig;
    rig.transport->reply = "Failure [DELETE_FAILED_INTERNAL_ERROR]\n";
    const auto uninstall = rig.run(AppAction::Uninstall, "com.example.app");
    CHECK(!uninstall.ok);
    CHECK_EQ(uninstall.message, std::string{"Failure [DELETE_FAILED_INTERNAL_ERROR]"});

    rig.transport->reply =
        "Exception occurred while executing 'grant':\n"
        "java.lang.SecurityException: Permission android.permission.INTERNET requested by "
        "com.example.app is not a changeable permission type\n";
    const auto grant = rig.run(AppAction::GrantPermission, "com.example.app",
                               "android.permission.INTERNET");
    CHECK(!grant.ok);
    CHECK_EQ(grant.message, std::string{"Permission android.permission.INTERNET requested by "
                                         "com.example.app is not a changeable permission type"});
}
