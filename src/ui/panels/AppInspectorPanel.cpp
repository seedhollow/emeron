#include "ui/panels/AppInspectorPanel.h"

#include <algorithm>
#include <climits>
#include <ctime>
#include <map>
#include <set>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "adb/DeviceManager.h"
#include "app/Theme.h"
#include "collect/FileSystemBrowser.h"
#include "core/CrashHandler.h"
#include "core/StringUtil.h"
#include "core/TaskQueue.h"
#include "ui/Widgets.h"

namespace em {
namespace {

const char* androidVersion(int sdk) {
    switch (sdk) {
        case 21: return "Android 5.0";
        case 22: return "Android 5.1";
        case 23: return "Android 6";
        case 24: return "Android 7.0";
        case 25: return "Android 7.1";
        case 26: return "Android 8.0";
        case 27: return "Android 8.1";
        case 28: return "Android 9";
        case 29: return "Android 10";
        case 30: return "Android 11";
        case 31: return "Android 12";
        case 32: return "Android 12L";
        case 33: return "Android 13";
        case 34: return "Android 14";
        case 35: return "Android 15";
        case 36: return "Android 16";
        default: return sdk > 36 ? "newer than Android 16" : "older than Android 5";
    }
}

std::string sdkText(int sdk) {
    if (sdk < 0) return "--";
    return std::to_string(sdk) + "  (" + androidVersion(sdk) + ")";
}

std::string installerName(const std::string& installer) {
    if (installer.empty() || installer == "null") return "None -- preinstalled, or adb install";
    if (installer == "com.android.vending") return "Google Play Store";
    if (installer == "com.google.android.packageinstaller" ||
        installer == "com.android.packageinstaller") {
        return "Package installer (APK opened on the phone)";
    }
    if (installer == "com.android.shell") return "adb install";
    return installer;
}

std::string bytesText(std::int64_t bytes) {
    return bytes < 0 ? std::string{"--"} : humanBytes(static_cast<double>(bytes));
}

std::string kbText(std::int64_t kb) {
    return kb < 0 ? std::string{"--"} : humanBytes(static_cast<double>(kb) * 1024.0);
}

// "android.permission.CAMERA" -> "CAMERA": the common prefix is noise in a
// long list. The full name is in the tooltip.
std::string_view shortPermission(std::string_view name) {
    constexpr std::string_view kPrefix = "android.permission.";
    return name.starts_with(kPrefix) ? name.substr(kPrefix.size()) : name;
}

// "com.app.ui.Main" in package com.app -> ".ui.Main", the manifest shorthand.
std::string shortClass(const std::string& cls, const std::string& packageName) {
    if (cls.size() > packageName.size() && cls.starts_with(packageName) &&
        cls[packageName.size()] == '.') {
        return cls.substr(packageName.size());
    }
    return cls;
}

// Runtime-permission flags in words. The noise flags (USER_SENSITIVE_*, the
// restriction exemptions) are dropped; the raw list stays in the tooltip.
std::string describePermissionFlags(std::string_view flags) {
    static constexpr std::pair<std::string_view, std::string_view> kWords[] = {
        {"USER_FIXED", "you chose \"don't ask again\""},
        {"USER_SET", "you chose"},
        {"GRANTED_BY_DEFAULT", "granted by default"},
        {"GRANTED_BY_ROLE", "granted for a role (e.g. default browser)"},
        {"POLICY_FIXED", "locked by device policy"},
        {"SYSTEM_FIXED", "locked by the system"},
        {"ONE_TIME", "one-time only"},
        {"REVIEW_REQUIRED", "needs review"},
        {"REVOKED_COMPAT", "revoked (app targets an old Android)"},
        {"AUTO_REVOKED", "auto-revoked: app unused for months"},
        {"APPLY_RESTRICTION", "restricted permission"},
    };
    std::string out;
    for (const auto part : split(flags, '|', false)) {
        const std::string_view token = trim(part);
        for (const auto& [flag, words] : kWords) {
            if (token == flag) {
                if (!out.empty()) out += ", ";
                out += words;
            }
        }
    }
    return out.empty() ? std::string{"not decided yet"} : out;
}

const char* kindLabel(AppComponent::Kind kind) {
    switch (kind) {
        case AppComponent::Kind::Activity: return "Screens (activities)";
        case AppComponent::Kind::Service:  return "Background services";
        case AppComponent::Kind::Receiver: return "Broadcast receivers";
        case AppComponent::Kind::Provider: return "Content providers";
    }
    return "";
}

const char* kindExplanation(AppComponent::Kind kind) {
    switch (kind) {
        case AppComponent::Kind::Activity:
            return "Screens the app shows. dumpsys only lists those with an intent filter -- the "
                   "ones other apps or the launcher can open.";
        case AppComponent::Kind::Service:
            return "Work that runs without a screen. Only services with an intent filter are "
                   "listed.";
        case AppComponent::Kind::Receiver:
            return "Code that wakes up for system or app broadcasts (boot completed, network "
                   "change...). Only manifest receivers with an intent filter are listed.";
        case AppComponent::Kind::Provider:
            return "Data the app shares with other apps through a content:// URI.";
    }
    return "";
}

// One label/value row of a two-column info table. Clicking the value copies it.
void infoRow(const char* label, const char* term, const char* explanation,
             const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    if (term != nullptr) widgets::termTooltip(term, explanation);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.empty() ? "--" : value.c_str());
    if (ImGui::IsItemClicked()) ImGui::SetClipboardText(value.c_str());
}

bool beginInfoTable(const char* id) {
    if (!ImGui::BeginTable(id, 2,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 190.0F);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void badge(const char* text, ImVec4 color, const char* term, const char* explanation) {
    widgets::sameLineOrWrap(ImGui::CalcTextSize(text).x);
    ImGui::TextColored(color, "%s", text);
    widgets::termTooltip(term, explanation);
}

// Fingerprints are conventionally shown upper-case ("F0:FD:6C:...").
std::string toUpper(std::string text) {
    for (char& ch : text) {
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    }
    return text;
}

bool matches(std::string_view text, const std::string& filter) {
    return filter.empty() || containsIgnoreCase(text, filter);
}

}  // namespace

// ---------------------------------------------------------------------------
// State changes
// ---------------------------------------------------------------------------

void AppInspectorPanel::onDeviceChanged(AppContext& context) {
    (void)context;
    ++generation_;
    apps_.clear();
    visible_.clear();
    listError_.clear();
    listLoading_ = false;
    listRequested_ = false;
    selected_.clear();
    autoSelected_ = false;
    details_ = AppDetails{};
    apk_ = ApkContents{};
    apkFor_.clear();
    apkLoading_ = false;
    detailsLoading_ = false;
    actionBusy_ = false;
    lastAction_ = AppActionResult{};
    confirm_ = Confirm::None;
}

void AppInspectorPanel::refreshList(AppContext& context) {
    listRequested_ = true;
    listLoading_ = true;
    listError_.clear();
    const std::uint64_t generation = generation_;
    context.apps.refreshList(context.pool, context.dispatcher,
                             [this, generation](AppListResult result) {
                                 if (generation != generation_) return;
                                 listLoading_ = false;
                                 listError_ = std::move(result.error);
                                 apps_ = std::move(result.apps);
                                 visibleDirty_ = true;
                             });
}

void AppInspectorPanel::select(AppContext& context, const std::string& packageName) {
    if (packageName == selected_) return;
    selected_ = packageName;
    details_ = AppDetails{};
    apk_ = ApkContents{};
    apkFor_.clear();
    lastAction_ = AppActionResult{};
    detailFilter_.clear();
    reloadDetails(context);
}

void AppInspectorPanel::reloadDetails(AppContext& context) {
    if (selected_.empty()) return;
    crash::setBreadcrumb("Apps: reading " + selected_);
    detailsLoading_ = true;
    const std::uint64_t generation = generation_;
    context.apps.loadDetails(context.pool, context.dispatcher, selected_,
                             [this, generation](AppDetails details) {
                                 // A slow dump can land after another app was picked.
                                 if (generation != generation_ ||
                                     details.packageName != selected_) {
                                     return;
                                 }
                                 detailsLoading_ = false;
                                 details_ = std::move(details);
                             });
}

void AppInspectorPanel::runAction(AppContext& context, AppAction action, std::string argument) {
    if (selected_.empty() || actionBusy_) return;
    actionBusy_ = true;
    const std::uint64_t generation = generation_;
    const std::string target = selected_;
    context.apps.runAction(
        context.pool, context.dispatcher, action, target, std::move(argument),
        [this, &context, generation, action, target](AppActionResult result) {
            if (generation != generation_) return;
            actionBusy_ = false;
            lastAction_ = std::move(result);
            if (action == AppAction::Uninstall && lastAction_.ok) {
                if (selected_ == target) {
                    selected_.clear();
                    details_ = AppDetails{};
                }
                refreshList(context);
                return;
            }
            if (selected_ == target) reloadDetails(context);
        });
}

void AppInspectorPanel::pullApks(AppContext& context) {
    if (details_.apks.empty() || actionBusy_) return;
    std::vector<std::string> paths;
    for (const auto& apk : details_.apks) paths.push_back(apk.path);

    std::string folder = details_.packageName;
    if (!details_.versionName.empty()) folder += "-" + details_.versionName;
    const auto destination = context.workspaceDir / "apks" / folder;

    actionBusy_ = true;
    const std::uint64_t generation = generation_;
    context.apps.pullApks(context.pool, context.dispatcher, std::move(paths), destination,
                          [this, &context, generation, destination](AppActionResult result) {
                              if (generation != generation_) return;
                              actionBusy_ = false;
                              lastAction_ = std::move(result);
                              if (lastAction_.ok) {
                                  FileSystemBrowser::revealOnHost(context.pool, destination);
                              }
                          });
}

void AppInspectorPanel::rebuildVisible() {
    visible_.clear();
    const auto kind = static_cast<Kind>(kind_);
    for (std::size_t i = 0; i < apps_.size(); ++i) {
        const InstalledApp& app = apps_[i];
        if (kind == Kind::User && app.system) continue;
        if (kind == Kind::System && !app.system) continue;
        if (!matches(app.packageName, filter_)) continue;
        visible_.push_back(i);
    }
    const int column = sortColumn_;
    const bool ascending = sortAscending_;
    std::stable_sort(visible_.begin(), visible_.end(), [&](std::size_t a, std::size_t b) {
        const InstalledApp& x = apps_[a];
        const InstalledApp& y = apps_[b];
        bool less = false;
        bool greater = false;
        if (column == 1) {
            less = x.totalBytes() < y.totalBytes();
            greater = x.totalBytes() > y.totalBytes();
        } else {
            less = x.packageName < y.packageName;
            greater = x.packageName > y.packageName;
        }
        return ascending ? less : greater;
    });
    visibleDirty_ = false;
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

void AppInspectorPanel::drawList(AppContext& context) {
    const auto& palette = theme::palette();

    ImGui::BeginDisabled(listLoading_);
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE)) refreshList(context);
    ImGui::EndDisabled();
    widgets::termTooltip("Reload  (pm list packages)", "Read the list of installed apps again.");

    ImGui::SameLine();
    static constexpr const char* kKinds[] = {"Installed by you", "Came with the phone", "All apps"};
    ImGui::SetNextItemWidth(160.0F);
    if (ImGui::Combo("##kind", &kind_, kKinds, 3)) visibleDirty_ = true;
    widgets::termTooltip("User / system apps  (pm list packages -3)",
                         "\"Installed by you\" is everything installed after the phone left the "
                         "factory -- Play Store, sideloaded APKs, adb install. \"Came with the "
                         "phone\" is the system image, including system apps that have since "
                         "been updated.");

    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::InputTextWithHint("##appfilter", ICON_FA_FILTER " package name", &filter_)) {
        visibleDirty_ = true;
    }

    if (visibleDirty_) rebuildVisible();

    if (listLoading_ && apps_.empty()) {
        ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  Reading the installed apps...");
        return;
    }
    if (!listError_.empty()) ImGui::TextColored(palette.bad, "%s", listError_.c_str());

    const auto userCount =
        std::count_if(apps_.begin(), apps_.end(), [](const InstalledApp& a) { return !a.system; });
    ImGui::TextDisabled("%zu shown  |  %lld installed by you, %zu in total", visible_.size(),
                        static_cast<long long>(userCount), apps_.size());

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Sortable |
                                       ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##apps", 2, kFlags)) return;
    ImGui::TableSetupColumn("App", ImGuiTableColumnFlags_WidthStretch |
                                       ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_PreferSortDescending,
                            70.0F);
    ImGui::TableSetupScrollFreeze(0, 1);
    static constexpr const char* kTerms[] = {"Package name", "Size  (dumpsys diskstats)"};
    static constexpr const char* kExplanations[] = {
        "The app's unique id on the device. A green dot means it is running right now.",
        ("App + data + cache, as Android last measured it. The system refreshes these numbers "
         "about once a day, so they can lag behind.")};
    widgets::headersRowWithTooltips(kTerms, kExplanations, 2);

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
        specs != nullptr && specs->SpecsDirty && specs->SpecsCount > 0) {
        sortColumn_ = specs->Specs[0].ColumnIndex;
        sortAscending_ = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
        specs->SpecsDirty = false;
        rebuildVisible();
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible_.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const InstalledApp& app = apps_[visible_[static_cast<std::size_t>(row)]];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(app.packageName.c_str());

            const bool isSelected = app.packageName == selected_;
            const bool isTarget = app.packageName == context.selectedPackage;
            const ImVec2 cellStart = ImGui::GetCursorPos();
            if (ImGui::Selectable("##row", isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                select(context, app.packageName);
            }
            if (ImGui::BeginItemTooltip()) {
                ImGui::TextUnformatted(app.packageName.c_str());
                ImGui::TextDisabled("version code %lld  |  uid %d",
                                    static_cast<long long>(app.versionCode), app.uid);
                ImGui::TextDisabled("installed by: %s", installerName(app.installer).c_str());
                ImGui::TextDisabled("%s", app.apkPath.c_str());
                if (app.appBytes >= 0) {
                    ImGui::TextDisabled("app %s  |  data %s  |  cache %s",
                                        bytesText(app.appBytes).c_str(),
                                        bytesText(app.dataBytes).c_str(),
                                        bytesText(app.cacheBytes).c_str());
                }
                ImGui::EndTooltip();
            }

            // Drawn over the label-less selectable, like Device Files, so the
            // dot and the name can have their own colours.
            ImGui::SetCursorPos(cellStart);
            ImGui::TextColored(app.running ? palette.good : ImVec4{0, 0, 0, 0}, ICON_FA_CIRCLE);
            ImGui::SameLine();
            if (isTarget) {
                ImGui::TextColored(palette.accent, ICON_FA_CROSSHAIRS);
                ImGui::SameLine();
            }
            if (app.disabled) {
                ImGui::TextColored(palette.muted, "%s (disabled)", app.packageName.c_str());
            } else {
                ImGui::TextUnformatted(app.packageName.c_str());
            }

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", bytesText(app.totalBytes()).c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// The detail view
// ---------------------------------------------------------------------------

void AppInspectorPanel::drawHeader(AppContext& context) {
    const auto& palette = theme::palette();
    const AppDetails& d = details_;

    ImGui::TextColored(palette.accent, ICON_FA_CUBE);
    ImGui::SameLine();
    ImGui::TextUnformatted(d.packageName.c_str());
    if (ImGui::IsItemClicked()) ImGui::SetClipboardText(d.packageName.c_str());
    ImGui::SetItemTooltip("Click to copy");
    ImGui::SameLine();
    ImGui::TextDisabled("%s  (%lld)", d.versionName.empty() ? "--" : d.versionName.c_str(),
                        static_cast<long long>(d.versionCode));

    // Status badges.
    ImGui::TextDisabled(" ");
    if (d.updatedSystemApp) {
        const std::string term = "Updated system app -- factory version " + d.factoryVersionName;
        badge(ICON_FA_CLOCK_ROTATE_LEFT " Updated system app", palette.accent, term.c_str(),
              "Came with the phone and has since been updated. Uninstalling removes only the "
              "update and goes back to the factory version.");
    } else if (d.isSystem()) {
        badge(ICON_FA_LOCK " Came with the phone", palette.muted, "System app  (FLAG_SYSTEM)",
              "Part of the system image. It can be disabled, but not uninstalled.");
    } else {
        badge(ICON_FA_DOWNLOAD " Installed by you", palette.good, "User app",
              "Installed after the phone left the factory.");
    }
    if (!d.pids.empty()) {
        std::string pids;
        for (const int pid : d.pids) pids += (pids.empty() ? "" : ", ") + std::to_string(pid);
        const std::string term = "Running  (pid " + pids + ")";
        badge(ICON_FA_CIRCLE " Running", palette.good, term.c_str(),
              "The app has a live process right now.");
    }
    if (d.hasFlag("DEBUGGABLE")) {
        badge(ICON_FA_BUG " Debuggable", palette.warning, "android:debuggable=\"true\"",
              "A debug build: debuggers can attach, and run-as gives shell access to its data. "
              "Release builds should never have this.");
    }
    if (d.stopped) {
        badge(ICON_FA_BAN " Force-stopped", palette.warning, "stopped=true",
              "Stopped by the user or by Force stop. It gets no broadcasts, alarms or jobs until "
              "something opens it again.");
    }
    if (d.enabledState >= 2) {
        badge(ICON_FA_BAN " Disabled", palette.bad, "enabled=2/3  (COMPONENT_ENABLED_STATE_DISABLED)",
              "Turned off: it cannot run and is hidden from the launcher.");
    }
    if (d.suspended) {
        badge(ICON_FA_BAN " Suspended", palette.bad, "suspended=true",
              "Paused by the system, e.g. by Digital Wellbeing or a device policy.");
    }
    if (apkFor_ == d.packageName && !apkLoading_) {
        const bool debugSigned = std::any_of(
            apk_.signing.signers.begin(), apk_.signing.signers.end(), [](const ApkSigner& s) {
                return !s.certificates.empty() && s.certificates[0].isDebugCertificate();
            }) || (!apk_.v1Certificates.empty() && apk_.v1Certificates[0].isDebugCertificate());
        if (debugSigned) {
            badge(ICON_FA_KEY " Debug key", palette.warning, "Signed with the Android debug key",
                  "CN=Android Debug: the key Android Studio makes for debug builds. Play and "
                  "most stores reject it, and anyone can sign an update with the same key.");
        }
        const auto notReady = std::count_if(apk_.libraries.begin(), apk_.libraries.end(),
                                            [](const NativeLibrary& l) { return !l.ready16k(); });
        if (notReady > 0) {
            badge(ICON_FA_TRIANGLE_EXCLAMATION " Not 16 KB ready", palette.warning,
                  "16 KB page size",
                  "Some 64-bit native libraries cannot load on devices with 16 KB memory pages. "
                  "See the Native code tab.");
        }
    }
    if (d.packageName == context.selectedPackage) {
        badge(ICON_FA_CROSSHAIRS " Target", palette.accent, "Selected package",
              "The Dashboard, Frame Time and Logcat panels are following this app.");
    }
}

void AppInspectorPanel::drawActions(AppContext& context) {
    const auto& palette = theme::palette();
    const AppDetails& d = details_;
    ImGui::BeginDisabled(actionBusy_);

    ImGui::BeginDisabled(d.packageName == context.selectedPackage);
    if (ImGui::Button(ICON_FA_CROSSHAIRS " Inspect this app")) {
        context.devices.selectPackage(d.packageName);
    }
    ImGui::EndDisabled();
    widgets::termTooltip("Select as target package",
                         "Make the Dashboard, Frame Time and Logcat panels follow this app.");

    widgets::sameLineOrWrap(90.0F);
    if (ImGui::Button(ICON_FA_PLAY " Open")) runAction(context, AppAction::Launch, d.launcherActivity);
    widgets::termTooltip(d.launcherActivity.empty() ? "monkey -p <pkg> -c LAUNCHER 1"
                                                    : ("am start -n " + d.launcherActivity).c_str(),
                         "Start the app on the phone, as tapping its icon would.");

    widgets::sameLineOrWrap(90.0F);
    if (ImGui::Button(ICON_FA_STOP " Force stop")) runAction(context, AppAction::ForceStop);
    widgets::termTooltip("am force-stop", "Kill every process of the app right away.");

    widgets::sameLineOrWrap(150.0F);
    if (ImGui::Button(ICON_FA_GEAR " Settings on phone")) {
        runAction(context, AppAction::OpenSettings);
    }
    widgets::termTooltip("APPLICATION_DETAILS_SETTINGS",
                         "Open this app's page in Android Settings, on the phone.");

    widgets::sameLineOrWrap(130.0F);
    ImGui::BeginDisabled(d.apks.empty());
    if (ImGui::Button(ICON_FA_DOWNLOAD " Save APK")) pullApks(context);
    ImGui::EndDisabled();
    {
        const std::string term = d.apks.size() > 1
                                     ? "adb pull  (" + std::to_string(d.apks.size()) +
                                           " files: base + splits)"
                                     : std::string{"adb pull"};
        widgets::termTooltip(term.c_str(),
                             "Copy the app's APK files to this computer, into "
                             "<workspace>/apks/<package>-<version>/.");
    }

    widgets::sameLineOrWrap(120.0F);
    ImGui::PushStyleColor(ImGuiCol_Text, palette.warning);
    if (ImGui::Button(ICON_FA_ERASER " Clear data")) confirm_ = Confirm::ClearData;
    ImGui::PopStyleColor();
    widgets::termTooltip("pm clear",
                         "Delete everything the app has stored -- logins, settings, databases, "
                         "cache -- as if it were freshly installed. Asks first.");

    if (!d.isSystem()) {
        widgets::sameLineOrWrap(110.0F);
        ImGui::PushStyleColor(ImGuiCol_Text, palette.bad);
        if (ImGui::Button(ICON_FA_TRASH_CAN " Uninstall")) confirm_ = Confirm::Uninstall;
        ImGui::PopStyleColor();
        widgets::termTooltip("pm uninstall", "Remove the app and its data. Asks first.");
    }

    widgets::sameLineOrWrap(40.0F);
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE)) reloadDetails(context);
    widgets::termTooltip("Reload  (dumpsys package)", "Read this app's details again.");

    ImGui::EndDisabled();

    if (actionBusy_) {
        ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  working...");
    } else if (!lastAction_.message.empty()) {
        ImGui::TextColored(lastAction_.ok ? palette.good : palette.bad, "%s %s",
                           lastAction_.ok ? ICON_FA_CIRCLE_CHECK : ICON_FA_CIRCLE_XMARK,
                           lastAction_.message.c_str());
    }
}

void AppInspectorPanel::drawOverviewTab() {
    const AppDetails& d = details_;
    if (!beginInfoTable("##overview")) return;

    infoRow("Version", "versionName", "The version users see, from build.gradle.", d.versionName);
    infoRow("Version code", "versionCode",
            "The internal build number. Each update must have a higher one.",
            d.versionCode < 0 ? std::string{} : std::to_string(d.versionCode));
    infoRow("Runs on at least", "minSdk",
            "The oldest Android version the app can be installed on.", sdkText(d.minSdk));
    infoRow("Built for", "targetSdk",
            "The Android version the app was tested against. Android applies newer behaviour "
            "changes only to apps that target it.",
            sdkText(d.targetSdk));
    infoRow("Installed by", "installerPackageName", nullptr, installerName(d.installer));
    infoRow("First installed", "firstInstallTime", nullptr, d.firstInstallTime);
    infoRow("Last updated", "lastUpdateTime", nullptr, d.lastUpdateTime);
    infoRow("Opens with", "Launcher activity  (MAIN / LAUNCHER)",
            "The screen that opens when the app's icon is tapped. Empty for apps without an "
            "icon, such as services and plug-ins.",
            d.launcherActivity.empty() ? std::string{"no launcher icon"}
                                       : shortClass(d.launcherActivity.substr(
                                                        d.launcherActivity.find('/') + 1),
                                                    d.packageName));
    infoRow("User ID", "appId / uid",
            "The Linux user the app runs as. Files and permissions are checked against it.",
            d.appId < 0 ? std::string{} : std::to_string(d.appId));
    if (!d.sharedUser.empty()) {
        infoRow("Shares its user ID with", "sharedUserId",
                "Apps with the same sharedUserId run as the same user and can read each "
                "other's data.",
                d.sharedUser);
    }
    infoRow("CPU architecture", "primaryCpuAbi",
            "Which native-library folder the app runs from. Empty for apps without native "
            "(C/C++) code.",
            d.primaryCpuAbi.empty() ? std::string{"none (no native code)"} : d.primaryCpuAbi);
    infoRow("Data folder", "dataDir", "Where the app's private files live on the device.",
            d.dataDir);
    infoRow("Code folder", "codePath", "Where the APK files are installed.", d.codePath);
    infoRow("Signing scheme", "apkSigningVersion",
            "APK Signature Scheme version. v2+ signs the whole file; v3 adds key rotation.",
            d.signingVersion < 0 ? std::string{} : "v" + std::to_string(d.signingVersion));
    {
        std::string sigs;
        for (const auto& s : d.signatures) sigs += (sigs.empty() ? "" : ", ") + s;
        infoRow("Signing certificate", "PackageSignatures",
                "A short fingerprint of the signing certificate as dumpsys prints it. Two "
                "builds with the same value were signed with the same key.",
                sigs);
    }
    {
        std::string flags;
        for (const auto& f : d.flags) flags += (flags.empty() ? "" : "  ") + f;
        infoRow("Flags", "ApplicationInfo.flags",
                "Manifest and system flags, e.g. DEBUGGABLE, ALLOW_BACKUP, LARGE_HEAP, "
                "HAS_CODE.",
                flags);
    }
    {
        std::string flags;
        for (const auto& f : d.privateFlags) flags += (flags.empty() ? "" : "  ") + f;
        infoRow("System flags", "privateFlags", "Flags only the system sets.", flags);
    }
    if (d.updatedSystemApp) {
        infoRow("Factory version", "Hidden system package",
                "The version in the system image, which Uninstall would go back to.",
                d.factoryVersionName);
    }
    ImGui::EndTable();
}

void AppInspectorPanel::drawPermissionsTab(AppContext& context) {
    const auto& palette = theme::palette();
    const AppDetails& d = details_;

    ImGui::SetNextItemWidth(260.0F);
    ImGui::InputTextWithHint("##permfilter", ICON_FA_FILTER " permission", &detailFilter_);

    // --- runtime ------------------------------------------------------------
    const auto granted = std::count_if(d.runtimePermissions.begin(), d.runtimePermissions.end(),
                                       [](const PermissionGrant& g) { return g.granted; });
    const std::string runtimeTitle = "Asked at runtime  (" + std::to_string(granted) + " of " +
                                     std::to_string(d.runtimePermissions.size()) + " allowed)";
    ImGui::SeparatorText(runtimeTitle.c_str());
    widgets::termTooltip("Runtime (dangerous) permissions",
                         "The ones the user is asked about: camera, location, contacts... Tick "
                         "or untick to grant or revoke on the device (pm grant / pm revoke).");
    if (d.runtimePermissions.empty()) {
        ImGui::TextDisabled("None.");
    } else if (ImGui::BeginTable("##runtime", 2,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Permission", ImGuiTableColumnFlags_WidthStretch, 1.3F);
        ImGui::TableSetupColumn("How it was set", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        for (const auto& g : d.runtimePermissions) {
            if (!matches(g.name, detailFilter_)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(g.name.c_str());
            bool value = g.granted;
            ImGui::BeginDisabled(actionBusy_);
            const std::string label = std::string{shortPermission(g.name)};
            if (ImGui::Checkbox(label.c_str(), &value)) {
                runAction(context, value ? AppAction::GrantPermission : AppAction::RevokePermission,
                          g.name);
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("%s\n%s", g.name.c_str(),
                                  g.granted ? "Allowed. Untick to revoke." : "Denied. Tick to grant.");
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
            ImGui::TextWrapped("%s", describePermissionFlags(g.flags).c_str());
            ImGui::PopStyleColor();
            if (!g.flags.empty()) widgets::termTooltip("Permission flags", g.flags.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // --- install time -------------------------------------------------------
    const std::string installTitle =
        "Granted at install  (" + std::to_string(d.installPermissions.size()) + ")";
    ImGui::SeparatorText(installTitle.c_str());
    widgets::termTooltip("Install-time (normal / signature) permissions",
                         "Granted automatically when the app is installed -- internet access, "
                         "vibration... The user is never asked about these.");
    for (const auto& g : d.installPermissions) {
        if (!matches(g.name, detailFilter_)) continue;
        ImGui::TextColored(g.granted ? palette.good : palette.muted, "%s",
                           g.granted ? ICON_FA_LOCK_OPEN : ICON_FA_LOCK);
        ImGui::SameLine();
        ImGui::TextUnformatted(std::string{shortPermission(g.name)}.c_str());
        ImGui::SetItemTooltip("%s\n%s", g.name.c_str(), g.granted ? "granted" : "not granted");
    }

    // --- requested ------------------------------------------------------------
    const std::string requestedTitle =
        "Listed in the manifest  (" + std::to_string(d.requestedPermissions.size()) + ")";
    ImGui::SeparatorText(requestedTitle.c_str());
    widgets::termTooltip("requested permissions  (<uses-permission>)",
                         "Everything the app's manifest asks for, granted or not.");
    for (const auto& name : d.requestedPermissions) {
        if (!matches(name, detailFilter_)) continue;
        ImGui::BulletText("%s", std::string{shortPermission(name)}.c_str());
        ImGui::SetItemTooltip("%s", name.c_str());
    }

    // --- declared -------------------------------------------------------------
    if (!d.declaredPermissions.empty()) {
        const std::string declaredTitle =
            "Defined by this app  (" + std::to_string(d.declaredPermissions.size()) + ")";
        ImGui::SeparatorText(declaredTitle.c_str());
        widgets::termTooltip("declared permissions  (<permission>)",
                             "New permissions the app creates, which other apps must hold to "
                             "use its components. prot= is who may be granted it.");
        for (const auto& item : d.declaredPermissions) {
            if (!matches(item, detailFilter_)) continue;
            ImGui::BulletText("%s", item.c_str());
        }
    }

    // --- app ops --------------------------------------------------------------
    if (!d.appOps.empty()) {
        const std::string opsTitle = "Operation modes  (" + std::to_string(d.appOps.size()) + ")";
        ImGui::SeparatorText(opsTitle.c_str());
        widgets::termTooltip("App ops  (cmd appops get)",
                             "Finer switches behind the permissions, e.g. location only in the "
                             "foreground, or running in the background. allow / ignore / "
                             "foreground / default.");
        if (ImGui::BeginTable("##appops", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& [op, mode] : d.appOps) {
                if (!matches(op, detailFilter_)) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(op.c_str());
                ImGui::TableNextColumn();
                const bool denied = mode.starts_with("ignore") || mode.starts_with("deny");
                ImGui::TextColored(denied ? palette.muted : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                                   "%s", mode.c_str());
            }
            ImGui::EndTable();
        }
    }
}

void AppInspectorPanel::drawComponentsTab() {
    const auto& palette = theme::palette();
    const AppDetails& d = details_;

    ImGui::SetNextItemWidth(260.0F);
    ImGui::InputTextWithHint("##compfilter", ICON_FA_FILTER " class or action", &detailFilter_);

    const std::set<std::string> disabled{d.disabledComponents.begin(), d.disabledComponents.end()};
    const std::string launcherClass =
        d.launcherActivity.empty() ? std::string{}
                                   : shortClass(d.launcherActivity.substr(d.launcherActivity.find('/') + 1),
                                                d.packageName);

    for (const auto kind : {AppComponent::Kind::Activity, AppComponent::Kind::Service,
                            AppComponent::Kind::Receiver, AppComponent::Kind::Provider}) {
        std::vector<const AppComponent*> rows;
        for (const auto& c : d.components) {
            if (c.kind != kind) continue;
            bool hit = matches(c.className, detailFilter_);
            for (const auto& a : c.actions) hit = hit || matches(a, detailFilter_);
            if (hit) rows.push_back(&c);
        }
        const std::string title =
            std::string{kindLabel(kind)} + "  (" + std::to_string(rows.size()) + ")###" +
            std::to_string(static_cast<int>(kind));
        const bool open = ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        widgets::termTooltip(kindLabel(kind), kindExplanation(kind));
        if (!open) continue;

        for (const AppComponent* c : rows) {
            const std::string name = shortClass(c->className, d.packageName);
            const bool isDisabled = disabled.contains(c->className);
            const bool isLauncher = kind == AppComponent::Kind::Activity && name == launcherClass;
            ImGui::PushID(c->className.c_str());
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
            if (c->actions.empty() && c->authorities.empty() && c->permission.empty()) {
                flags |= ImGuiTreeNodeFlags_Leaf;
            }
            if (isDisabled) ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
            const bool node = ImGui::TreeNodeEx("##c", flags, "%s%s%s", isLauncher ? ICON_FA_STAR " " : "",
                                                name.c_str(), isDisabled ? "  (disabled)" : "");
            if (isDisabled) ImGui::PopStyleColor();
            if (ImGui::BeginItemTooltip()) {
                ImGui::TextUnformatted(c->className.c_str());
                if (isLauncher) ImGui::TextColored(palette.accent, "Opens when the app icon is tapped");
                ImGui::EndTooltip();
            }
            if (node) {
                if (!c->permission.empty()) {
                    ImGui::TextColored(palette.warning, ICON_FA_LOCK " needs %s", c->permission.c_str());
                }
                for (const auto& authority : c->authorities) {
                    ImGui::TextDisabled("content://%s", authority.c_str());
                }
                for (const auto& action : c->actions) ImGui::BulletText("%s", action.c_str());
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
}

void AppInspectorPanel::drawStorageTab() {
    const auto& palette = theme::palette();
    const AppDetails& d = details_;

    ImGui::SeparatorText(ICON_FA_HARD_DRIVE "  Space used");
    widgets::termTooltip("dumpsys diskstats",
                         "As Android last measured it. The system refreshes these numbers about "
                         "once a day, so they can lag behind.");
    if (beginInfoTable("##space")) {
        infoRow("App", "App size", "The APK files and the compiled code.", bytesText(d.appBytes));
        infoRow("Data", "Data size", "Files, databases and settings the app saved.",
                bytesText(d.dataBytes));
        infoRow("Cache", "Cache size", "Temporary files Android may delete when space runs low.",
                bytesText(d.cacheBytes));
        ImGui::EndTable();
    }

    std::int64_t apkTotal = 0;
    for (const auto& apk : d.apks) apkTotal += std::max<std::int64_t>(apk.sizeBytes, 0);
    const std::string apkTitle = std::string{ICON_FA_BOX_ARCHIVE "  APK files  ("} +
                                 std::to_string(d.apks.size()) + ", " + bytesText(apkTotal) + ")";
    ImGui::SeparatorText(apkTitle.c_str());
    widgets::termTooltip("pm path",
                         "base.apk plus any split APKs (per-language, per-screen-density, "
                         "dynamic features) the app was installed with.");
    if (ImGui::BeginTable("##apks", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        for (const auto& apk : d.apks) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const auto slash = apk.path.rfind('/');
            ImGui::TextUnformatted(slash == std::string::npos ? apk.path.c_str()
                                                              : apk.path.c_str() + slash + 1);
            ImGui::SetItemTooltip("%s", apk.path.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", bytesText(apk.sizeBytes).c_str());
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText(ICON_FA_MEMORY "  Memory right now");
    widgets::termTooltip("dumpsys meminfo <pkg>  (App Summary)",
                         "PSS counts shared memory divided among the processes sharing it -- "
                         "the fairest \"how much is this app using\". RSS counts all of it.");
    if (d.memory.empty()) {
        ImGui::TextDisabled(d.pids.empty() ? "Not running. Open the app to measure its memory."
                                           : "The device did not report a memory summary.");
    } else if (ImGui::BeginTable("##mem", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("PSS", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        ImGui::TableSetupColumn("RSS", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        static constexpr const char* kTerms[] = {"Memory kind", "PSS -- proportional set size",
                                                 "RSS -- resident set size"};
        static constexpr const char* kExplanations[] = {
            ("Java Heap: Kotlin/Java objects. Native Heap: C/C++ allocations. Graphics: GPU "
             "textures and buffers. Code: loaded APK, dex and .so files."),
            "Memory in RAM, with shared pages split among the processes using them.",
            "Memory in RAM, counting shared pages in full."};
        widgets::headersRowWithTooltips(kTerms, kExplanations, 3);
        for (const auto& row : d.memory) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool total = row.label == "Total";
            if (total) {
                ImGui::TextColored(palette.accent, "%s", row.label.c_str());
            } else {
                ImGui::TextUnformatted(row.label.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kbText(row.pssKb).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", kbText(row.rssKb).c_str());
        }
        ImGui::EndTable();
    }
}

void AppInspectorPanel::drawBuildTab() {
    const AppDetails& d = details_;

    ImGui::SeparatorText(ICON_FA_HAMMER "  Compilation");
    widgets::termTooltip("Dexopt state",
                         "How ART compiled each APK. speed-profile: the hot code is compiled "
                         "ahead of time from a usage profile. verify: interpreted/JIT only, "
                         "so it starts slower. speed: everything compiled.");
    if (d.dexopt.empty()) {
        ImGui::TextDisabled("Not reported.");
    } else if (ImGui::BeginTable("##dexopt", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("APK");
        ImGui::TableSetupColumn("CPU");
        ImGui::TableSetupColumn("Compiled as");
        ImGui::TableSetupColumn("Why");
        ImGui::TableHeadersRow();
        for (const auto& e : d.dexopt) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.apk.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", e.isa.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.status.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", e.reason.c_str());
        }
        ImGui::EndTable();
    }

    if (!d.splits.empty()) {
        ImGui::SeparatorText("Split APKs");
        widgets::termTooltip("splits", "The parts the app was installed as, base first.");
        std::string text;
        for (const auto& s : d.splits) text += (text.empty() ? "" : ",  ") + s;
        ImGui::TextWrapped("%s", text.c_str());
    }

    // Every other list dumpsys printed, so nothing is hidden.
    static const std::set<std::string> kShownElsewhere{
        "requested permissions", "declared permissions", "install permissions",
        "runtime permissions",   "enabledComponents",    "disabledComponents"};
    for (const auto& [name, items] : d.lists) {
        if (kShownElsewhere.contains(name) || items.empty()) continue;
        const std::string title = name + "  (" + std::to_string(items.size()) + ")";
        if (ImGui::CollapsingHeader(title.c_str())) {
            for (const auto& item : items) ImGui::BulletText("%s", item.c_str());
        }
    }
}

void AppInspectorPanel::ensureApkContents(AppContext& context) {
    if (apkFor_ == selected_ || details_.packageName != selected_ || details_.apks.empty()) return;
    apkFor_ = selected_;
    apkLoading_ = true;
    crash::setBreadcrumb("Apps: reading the APK files of " + selected_);
    const std::uint64_t generation = generation_;
    context.apps.analyzeApks(context.pool, context.dispatcher, selected_, details_.apks,
                             [this, generation](ApkContents contents) {
                                 if (generation != generation_ ||
                                     contents.packageName != apkFor_) {
                                     return;
                                 }
                                 apkLoading_ = false;
                                 apk_ = std::move(contents);
                             });
}

// Shared top of the Native code and Signature tabs. False while there is
// nothing to show yet.
bool AppInspectorPanel::drawApkContentsState(AppContext& context) {
    ensureApkContents(context);
    if (details_.apks.empty()) {
        ImGui::TextDisabled("The device listed no APK files for this app.");
        return false;
    }
    if (apkLoading_) {
        ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  Reading the APK files on the device...");
        return false;
    }
    if (!apk_.error.empty()) {
        ImGui::TextColored(theme::palette().bad, "%s", apk_.error.c_str());
        if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Try again")) {
            apkFor_.clear();
            ensureApkContents(context);
        }
        return false;
    }
    return true;
}

void AppInspectorPanel::drawNativeTab(AppContext& context) {
    if (!drawApkContentsState(context)) return;
    const auto& palette = theme::palette();
    const AppDetails& d = details_;
    const auto& libs = apk_.libraries;

    if (libs.empty()) {
        ImGui::TextColored(palette.good, ICON_FA_CIRCLE_CHECK "  No native libraries.");
        ImGui::TextDisabled(d.primaryCpuAbi.empty()
                                ? "The app is pure Java/Kotlin: there is no .so file in any of "
                                  "its APKs."
                                : "There is no .so file inside its APKs; its native code comes "
                                  "from the system image.");
        return;
    }

    // Summary: ABIs and total size.
    std::map<std::string, int> perAbi;
    std::uint64_t total = 0;
    for (const auto& l : libs) {
        ++perAbi[l.abi];
        total += l.sizeBytes;
    }
    std::string abis;
    for (const auto& [abi, n] : perAbi) {
        abis += (abis.empty() ? "" : ",  ") + abi + " (" + std::to_string(n) + ")";
    }
    ImGui::Text("%zu libraries, %s", libs.size(), humanBytes(static_cast<double>(total)).c_str());
    ImGui::TextDisabled("%s", abis.c_str());
    widgets::termTooltip("ABIs  (lib/<abi>/)",
                         "One folder per CPU architecture the app ships native code for. "
                         "arm64-v8a is what almost every current phone runs.");

    // 16 KB page size.
    const auto sixtyFour = std::count_if(libs.begin(), libs.end(),
                                         [](const NativeLibrary& l) { return l.is64Bit(); });
    const auto notReady = std::count_if(libs.begin(), libs.end(),
                                        [](const NativeLibrary& l) { return !l.ready16k(); });
    ImGui::Spacing();
    if (sixtyFour == 0) {
        ImGui::TextDisabled(ICON_FA_CIRCLE_INFO "  16 KB page size: not applicable (no 64-bit libraries)");
    } else if (notReady == 0) {
        ImGui::TextColored(palette.good, ICON_FA_CIRCLE_CHECK "  Ready for 16 KB page size");
    } else {
        ImGui::TextColored(palette.warning,
                           ICON_FA_TRIANGLE_EXCLAMATION "  %lld of %lld 64-bit libraries are not ready "
                                                        "for 16 KB page size",
                           static_cast<long long>(notReady), static_cast<long long>(sixtyFour));
    }
    widgets::termTooltip(
        "16 KB page size  (Android 15+)",
        "Newer devices use 16 KB memory pages. A 64-bit library loads on them only if its ELF "
        "LOAD segments are aligned to 16 KB, and -- when it is stored uncompressed in the APK -- "
        "its data starts on a 16 KB boundary in the ZIP. Google Play requires this for apps "
        "targeting Android 15 and higher. Fix: NDK r28+, or -Wl,-z,max-page-size=16384, and "
        "AGP 8.5.1+ for the ZIP alignment.");

    ImGui::Spacing();
    ImGui::SetNextItemWidth(260.0F);
    ImGui::InputTextWithHint("##libfilter", ICON_FA_FILTER " library", &detailFilter_);

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##libs", 5, kFlags)) return;
    ImGui::TableSetupColumn("Library", ImGuiTableColumnFlags_WidthStretch, 2.2F);
    ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthStretch, 1.0F);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.8F);
    ImGui::TableSetupColumn("In the APK", ImGuiTableColumnFlags_WidthStretch, 1.0F);
    ImGui::TableSetupColumn("16 KB pages", ImGuiTableColumnFlags_WidthStretch, 1.1F);
    static constexpr const char* kTerms[] = {
        "Library", "ABI", "Uncompressed size", "Stored / compressed  (extractNativeLibs)",
        "16 KB page size"};
    static constexpr const char* kExplanations[] = {
        "Hover a row for which APK it is in and its ELF details.",
        "The CPU architecture folder it ships in.",
        "Size of the .so file itself.",
        ("Uncompressed: loaded straight from the APK, which saves space on the device "
         "(extractNativeLibs=false, the default). Compressed: unpacked to the data folder at "
         "install time."),
        ("Whether this library can load on a device with 16 KB memory pages. 32-bit libraries "
         "are not affected.")};
    widgets::headersRowWithTooltips(kTerms, kExplanations, 5);

    for (const auto& l : libs) {
        if (!matches(l.name, detailFilter_) && !matches(l.abi, detailFilter_)) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(l.name.c_str());
        if (ImGui::BeginItemTooltip()) {
            ImGui::Text("lib/%s/%s", l.abi.c_str(), l.name.c_str());
            ImGui::TextDisabled("in %s", l.apk.c_str());
            if (l.elf.valid) {
                ImGui::TextDisabled("ELF %d-bit %s  |  LOAD segments aligned to %s", l.elf.bits,
                                    l.elf.machine.c_str(),
                                    humanBytes(static_cast<double>(l.elf.loadAlignment)).c_str());
            } else {
                ImGui::TextColored(palette.bad, "ELF: %s", l.elf.error.c_str());
            }
            if (l.dataOffset >= 0) {
                ImGui::TextDisabled("data starts at byte %lld of the APK",
                                    static_cast<long long>(l.dataOffset));
            }
            ImGui::EndTooltip();
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", l.abi.c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", humanBytes(static_cast<double>(l.sizeBytes)).c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", l.compressed ? "compressed" : "uncompressed");
        ImGui::TableNextColumn();
        if (!l.is64Bit()) {
            ImGui::TextDisabled("n/a (32-bit)");
        } else if (l.ready16k()) {
            ImGui::TextColored(palette.good, ICON_FA_CHECK " ready");
        } else if (!l.elfAligned16k()) {
            if (l.elf.valid) {
                ImGui::TextColored(palette.warning, ICON_FA_XMARK " ELF %llu KB",
                                   static_cast<unsigned long long>(l.elf.loadAlignment / 1024));
            } else {
                ImGui::TextColored(palette.warning, ICON_FA_XMARK " unreadable");
            }
            widgets::termTooltip("ELF LOAD alignment",
                                 "Rebuild with NDK r28+ or link with -Wl,-z,max-page-size=16384.");
        } else {
            ImGui::TextColored(palette.warning, ICON_FA_XMARK " ZIP offset");
            widgets::termTooltip("ZIP alignment",
                                 "The library's data in the APK does not start on a 16 KB "
                                 "boundary. Package with AGP 8.5.1+ (or zipalign -P 16).");
        }
    }
    ImGui::EndTable();
}

void AppInspectorPanel::drawSignatureTab(AppContext& context) {
    if (!drawApkContentsState(context)) return;
    const auto& palette = theme::palette();
    const SigningBlock& block = apk_.signing;

    // Which schemes are present.
    bool v2 = false;
    bool v3 = false;
    bool v31 = false;
    for (const auto& s : block.signers) {
        v2 = v2 || s.scheme == 2;
        v3 = v3 || s.scheme == 3;
        v31 = v31 || s.scheme == 31;
    }
    ImGui::SeparatorText(ICON_FA_SIGNATURE "  Signature schemes");
    widgets::termTooltip("APK Signature Schemes",
                         "How the APK is signed. Newer schemes protect the whole file and allow "
                         "key rotation; an APK usually carries several for older Android versions.");
    const auto scheme = [&](const char* label, bool present, const char* term, const char* text) {
        ImGui::TextColored(present ? palette.good : palette.muted, "%s %s",
                           present ? ICON_FA_CHECK : ICON_FA_MINUS, label);
        widgets::termTooltip(term, text);
        ImGui::SameLine(0.0F, 24.0F);
    };
    scheme("v1 (JAR)", apk_.hasV1Signature, "v1 -- JAR signing",
           "Signs each file inside the APK. Only Android 6 and older need it.");
    scheme("v2", v2, "APK Signature Scheme v2  (Android 7.0+)",
           "Signs the APK as a whole, so nothing in it can be changed.");
    scheme("v3", v3, "APK Signature Scheme v3  (Android 9+)",
           "Like v2, plus key rotation: an app can move to a new signing key.");
    scheme("v3.1", v31, "APK Signature Scheme v3.1  (Android 13+)",
           "Lets a rotated key apply only to newer Android versions.");
    ImGui::NewLine();
    if (!block.error.empty()) ImGui::TextColored(palette.bad, "%s", block.error.c_str());

    // The certificates, de-duplicated: v2 and v3 almost always carry the same one.
    struct Shown {
        const CertificateInfo* cert;
        std::string usedBy;
    };
    std::vector<Shown> certs;
    const auto add = [&certs](const CertificateInfo& c, const std::string& scheme) {
        for (auto& s : certs) {
            if (s.cert->sha256 == c.sha256) {
                s.usedBy += ", " + scheme;
                return;
            }
        }
        certs.push_back({&c, scheme});
    };
    for (const auto& s : block.signers) {
        std::string name = s.scheme == 31 ? "v3.1" : "v" + std::to_string(s.scheme);
        if (s.minSdk > 0) {
            name += " (Android API " + std::to_string(s.minSdk) +
                    (s.maxSdk == INT_MAX ? "+" : " to " + std::to_string(s.maxSdk)) + ")";
        }
        for (const auto& c : s.certificates) add(c, name);
    }
    for (const auto& c : apk_.v1Certificates) add(c, "v1");

    if (certs.empty()) {
        ImGui::TextColored(palette.bad, "No signing certificate found in %s.",
                           details_.apks.front().path.c_str());
        return;
    }

    // Today's date as "YYYY-MM-DD", to flag an expired certificate.
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    const std::string today = format("%04d-%02d-%02d", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);

    for (std::size_t i = 0; i < certs.size(); ++i) {
        const CertificateInfo& c = *certs[i].cert;
        const std::string title = std::string{ICON_FA_CERTIFICATE "  Signing certificate"} +
                                  (certs.size() > 1 ? " " + std::to_string(i + 1) : "") +
                                  "###cert" + std::to_string(i);
        ImGui::SeparatorText(title.c_str());
        if (!c.error.empty()) ImGui::TextColored(palette.bad, "%s", c.error.c_str());
        if (c.isDebugCertificate()) {
            ImGui::PushStyleColor(ImGuiCol_Text, palette.warning);
            ImGui::TextWrapped(ICON_FA_KEY "  This is the Android debug key -- fine for "
                               "development, never for a release.");
            ImGui::PopStyleColor();
        }

        ImGui::PushID(static_cast<int>(i));
        if (beginInfoTable("##cert")) {
            infoRow("Owner", "Subject", "Who the certificate was issued to.", c.subject);
            infoRow("Issued by", "Issuer",
                    "App signing certificates are almost always self-signed: issuer and owner "
                    "are the same.",
                    c.issuer == c.subject ? std::string{"itself (self-signed)"} : c.issuer);
            infoRow("Valid from", "notBefore", nullptr, c.validFrom);
            infoRow("Valid until", "notAfter",
                    "Android does not check expiry when installing, but Google Play requires "
                    "keys valid until at least 2033.",
                    c.validUntil);
            if (!c.validUntil.empty() && c.validUntil.substr(0, 10) < today) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextColored(palette.warning, ICON_FA_TRIANGLE_EXCLAMATION " expired");
            }
            infoRow("Signature algorithm", "Certificate signature algorithm",
                    "How the certificate itself was signed. MD5 and SHA1 are weak, but old "
                    "certificates keep them for life: the app's key cannot change without "
                    "rotation.",
                    c.signatureAlgorithm);
            infoRow("Key", "Public key", "The key type and size the APK is signed with.", c.publicKey);
            infoRow("Serial number", "serialNumber", nullptr, c.serialNumber);
            infoRow("Used by", "Signature schemes", "The schemes that carry this certificate.",
                    certs[i].usedBy);
            ImGui::EndTable();
        }

        const auto fingerprint = [&palette](const char* label, const char* term,
                                            const std::string& value) {
            ImGui::TextDisabled("%s", label);
            widgets::termTooltip(term,
                                 "Paste this into Firebase, Google Cloud (Maps, Sign-In), "
                                 "Facebook Login and similar consoles to register the app. "
                                 "Two APKs with the same fingerprint were signed with the same "
                                 "key.");
            ImGui::SameLine(190.0F);
            ImGui::PushStyleColor(ImGuiCol_Text, palette.accent);
            ImGui::TextWrapped("%s", toUpper(value).c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemClicked()) ImGui::SetClipboardText(toUpper(value).c_str());
            ImGui::SetItemTooltip("Click to copy");
        };
        fingerprint("SHA-256 fingerprint", "SHA-256 certificate fingerprint", c.sha256);
        fingerprint("SHA-1 fingerprint", "SHA-1 certificate fingerprint", c.sha1);
        ImGui::PopID();
    }

    if (!block.otherBlocks.empty()) {
        ImGui::SeparatorText("Other data in the signing block");
        widgets::termTooltip("APK Signing Block",
                             "The area before the ZIP index that holds the signatures; tools "
                             "and stores also put their own data there.");
        for (const auto& name : block.otherBlocks) ImGui::BulletText("%s", name.c_str());
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Read from %s. emeron shows the certificates; it does not verify the "
                        "signatures -- apksigner verify does.",
                        details_.apks.front().path.substr(details_.apks.front().path.rfind('/') + 1).c_str());
}

void AppInspectorPanel::drawRawTab() {
    const AppDetails& d = details_;

    if (ImGui::Button(ICON_FA_COPY " Copy dump")) ImGui::SetClipboardText(d.rawDump.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0F);
    ImGui::InputTextWithHint("##rawfilter", ICON_FA_FILTER " field", &detailFilter_);

    if (ImGui::CollapsingHeader("Every field", ImGuiTreeNodeFlags_DefaultOpen) &&
        beginInfoTable("##fields")) {
        for (const auto& [key, value] : d.fields) {
            if (!matches(key, detailFilter_) && !matches(value, detailFilter_)) continue;
            infoRow(key.c_str(), nullptr, nullptr, value);
        }
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("dumpsys package output")) {
        ImGui::BeginChild("##raw", ImVec2{-1.0F, 400.0F}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(d.rawDump.c_str(), d.rawDump.c_str() + d.rawDump.size());
        ImGui::EndChild();
    }
}

void AppInspectorPanel::drawConfirmModals(AppContext& context) {
    const char* const kClear = "Clear app data?";
    const char* const kUninstall = "Uninstall app?";
    if (confirm_ == Confirm::ClearData) ImGui::OpenPopup(kClear);
    if (confirm_ == Confirm::Uninstall) ImGui::OpenPopup(kUninstall);
    confirm_ = Confirm::None;

    const auto& palette = theme::palette();
    if (ImGui::BeginPopupModal(kClear, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete all data of %s?", selected_.c_str());
        ImGui::TextColored(palette.warning,
                           "Logins, settings, databases and files are gone for good.");
        if (ImGui::Button("Clear data", ImVec2{120.0F, 0.0F})) {
            runAction(context, AppAction::ClearData);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2{120.0F, 0.0F}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(kUninstall, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Uninstall %s from the device?", selected_.c_str());
        ImGui::TextColored(palette.warning, "Its data is deleted too.");
        if (ImGui::Button("Uninstall", ImVec2{120.0F, 0.0F})) {
            runAction(context, AppAction::Uninstall);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2{120.0F, 0.0F}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void AppInspectorPanel::drawDetails(AppContext& context) {
    if (selected_.empty()) {
        ImGui::TextDisabled(ICON_FA_HAND_POINTER "  Pick an app on the left to see everything "
                            "about it.");
        return;
    }
    if (detailsLoading_ && details_.packageName != selected_) {
        ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  Reading %s...", selected_.c_str());
        return;
    }
    if (!details_.error.empty() && !details_.found) {
        ImGui::TextColored(theme::palette().bad, "%s", details_.error.c_str());
        if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Try again")) reloadDetails(context);
        return;
    }

    drawHeader(context);
    drawActions(context);
    ImGui::Separator();

    if (!ImGui::BeginTabBar("##apptabs", ImGuiTabBarFlags_FittingPolicyScroll)) return;
    const auto tab = [](const char* label, auto&& body) {
        if (ImGui::BeginTabItem(label)) {
            ImGui::BeginChild("##tabbody");
            body();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    };
    tab(ICON_FA_CIRCLE_INFO " Overview", [&] { drawOverviewTab(); });
    const std::string permissions =
        std::string{ICON_FA_SHIELD_HALVED " Permissions ("} +
        std::to_string(details_.requestedPermissions.size()) + ")###perms";
    tab(permissions.c_str(), [&] { drawPermissionsTab(context); });
    const std::string components = std::string{ICON_FA_PUZZLE_PIECE " Components ("} +
                                   std::to_string(details_.components.size()) + ")###comps";
    tab(components.c_str(), [&] { drawComponentsTab(); });
    tab(ICON_FA_HARD_DRIVE " Storage & memory", [&] { drawStorageTab(); });
    tab(ICON_FA_MICROCHIP " Native code", [&] { drawNativeTab(context); });
    tab(ICON_FA_CERTIFICATE " Signature", [&] { drawSignatureTab(context); });
    tab(ICON_FA_HAMMER " Build", [&] { drawBuildTab(); });
    tab(ICON_FA_FILE_CODE " Raw", [&] { drawRawTab(); });
    ImGui::EndTabBar();
}

void AppInspectorPanel::draw(AppContext& context) {
    if (!ImGui::Begin(windowTitle(), visibleFlag())) {
        ImGui::End();
        return;
    }
    if (!context.hasDevice()) {
        ImGui::TextDisabled(ICON_FA_PLUG "  No device selected.");
        ImGui::End();
        return;
    }
    if (!listRequested_) refreshList(context);

    // First time the list is in: start on the app the toolbar targets, which
    // is usually the one being worked on.
    if (!autoSelected_ && !apps_.empty() && selected_.empty()) {
        autoSelected_ = true;
        const bool known = std::any_of(apps_.begin(), apps_.end(), [&](const InstalledApp& a) {
            return a.packageName == context.selectedPackage;
        });
        if (context.hasPackage() && known) select(context, context.selectedPackage);
    }

    if (ImGui::BeginTable("##split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImVec2{0.0F, -1.0F})) {
        ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthFixed, 330.0F);
        ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("##listpane");
        drawList(context);
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("##detailpane");
        drawDetails(context);
        ImGui::EndChild();
        ImGui::EndTable();
    }

    drawConfirmModals(context);
    ImGui::End();
}

}  // namespace em
