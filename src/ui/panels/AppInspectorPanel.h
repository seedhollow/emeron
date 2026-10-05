#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <IconsFontAwesome6.h>

#include "collect/AppInspector.h"
#include "ui/Panel.h"

namespace em {

// Every installed app on the device, and everything the device will say about
// one of them: version, SDK levels, installer, signing, permissions and their
// grant state, components, APK files, storage, memory, compilation state, and
// the raw `dumpsys package` it was all read from.
class AppInspectorPanel final : public Panel {
public:
    static constexpr const char* kId = "Apps";

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_CUBES; }
    void draw(AppContext& context) override;
    void onDeviceChanged(AppContext& context) override;

private:
    enum class Kind : int { User = 0, System, All };
    enum class Confirm { None, ClearData, Uninstall };

    void refreshList(AppContext& context);
    void select(AppContext& context, const std::string& packageName);
    void reloadDetails(AppContext& context);
    void ensureApkContents(AppContext& context);
    void runAction(AppContext& context, AppAction action, std::string argument = {});
    void pullApks(AppContext& context);
    void rebuildVisible();

    void drawList(AppContext& context);
    void drawDetails(AppContext& context);
    void drawHeader(AppContext& context);
    void drawActions(AppContext& context);
    void drawOverviewTab();
    void drawPermissionsTab(AppContext& context);
    void drawComponentsTab();
    void drawStorageTab();
    void drawBuildTab();
    void drawRawTab();
    void drawNativeTab(AppContext& context);
    void drawSignatureTab(AppContext& context);
    bool drawApkContentsState(AppContext& context);
    void drawConfirmModals(AppContext& context);

    // Results from a previous device must not land on the current one.
    std::uint64_t generation_ = 0;

    std::vector<InstalledApp> apps_;
    std::vector<std::size_t> visible_;
    std::string listError_;
    bool listLoading_ = false;
    bool listRequested_ = false;
    bool visibleDirty_ = true;
    std::string filter_;
    int kind_ = static_cast<int>(Kind::User);
    int sortColumn_ = 0;
    bool sortAscending_ = true;

    std::string selected_;
    bool autoSelected_ = false;  // opened once on the toolbar's target package
    AppDetails details_;
    // Read from the APK files on demand, when its tab is first opened.
    ApkContents apk_;
    std::string apkFor_;
    bool apkLoading_ = false;
    bool detailsLoading_ = false;
    std::string detailFilter_;

    bool actionBusy_ = false;
    AppActionResult lastAction_;
    Confirm confirm_ = Confirm::None;
};

}  // namespace em
