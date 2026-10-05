#include "ui/panels/DeviceToolbar.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>
#include <imgui_stdlib.h>

#include "adb/DeviceManager.h"
#include "app/Theme.h"
#include "collect/FrameStats.h"
#include "collect/MetricSampler.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "core/TaskQueue.h"
#include "ui/Widgets.h"

namespace em {
namespace {
constexpr const char* kCategory = "toolbar";
}

void DeviceToolbar::onDeviceChanged() {
    packages_.clear();
    packageError_.clear();
    packagesRequested_ = false;
    packagesLoading_ = false;
}

void DeviceToolbar::refreshPackages(AppContext& context) {
    if (packagesLoading_ || !context.hasDevice()) return;

    packagesLoading_ = true;
    packageError_.clear();
    const std::string serial = context.selectedSerial;
    const bool thirdParty = thirdPartyOnly_;

    context.pool.submit([this, &context, serial, thirdParty] {
        auto result = context.devices.adb().packages(serial, thirdParty);
        if (!result) {
            const std::string message = result.error().message;
            context.dispatcher.post([this, message] {
                packageError_ = message;
                packagesLoading_ = false;
            });
            return;
        }
        auto list = std::move(result).value();
        context.dispatcher.post([this, list = std::move(list)]() mutable {
            packages_ = std::move(list);
            packagesLoading_ = false;
        });
    });
}

void DeviceToolbar::drawDevicePicker(AppContext& context) {
    const DeviceList list = context.devices.devices();
    const auto& palette = theme::palette();

    std::string preview = "No device";
    if (const auto selected = context.devices.selectedDevice()) {
        preview = selected->displayName();
    } else if (!context.selectedSerial.empty()) {
        preview = context.selectedSerial + " (gone)";
    }

    ImGui::SetNextItemWidth(320.0F);
    if (ImGui::BeginCombo("##device", preview.c_str())) {
        if (list.devices.empty()) {
            ImGui::TextDisabled("no devices attached");
            if (!list.serverReachable) {
                ImGui::TextColored(palette.bad, "adb server unreachable");
            }
        }
        for (const auto& device : list.devices) {
            const bool selected = device.serial == context.selectedSerial;
            const std::string label = device.displayName();

            if (!device.usable()) ImGui::BeginDisabled();
            if (ImGui::Selectable(label.c_str(), selected)) {
                context.devices.selectSerial(device.serial);
            }
            if (!device.usable()) ImGui::EndDisabled();

            ImGui::SameLine();
            const ImVec4 stateColor = device.usable() ? palette.good
                                      : device.state == DeviceState::Unauthorized ? palette.warning
                                                                                  : palette.bad;
            const char* stateIcon = device.usable() ? ICON_FA_CIRCLE_CHECK
                                    : device.state == DeviceState::Unauthorized ? ICON_FA_LOCK
                                    : device.state == DeviceState::Offline      ? ICON_FA_PLUG_CIRCLE_XMARK
                                                                                : ICON_FA_CIRCLE_QUESTION;
            ImGui::TextColored(stateColor, "%s %s", stateIcon,
                               std::string{toString(device.state)}.c_str());
            if (device.isNetworkDevice()) {
                ImGui::SameLine();
                ImGui::TextDisabled(ICON_FA_WIFI);
                ImGui::SetItemTooltip("Connected over the network");
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE)) {
        context.devices.requestRefresh();
        onDeviceChanged();
    }
    ImGui::SetItemTooltip("Refresh the device list (Ctrl+R)");

    // An unauthorized device is the single most common first-run problem, so
    // it gets called out rather than left as a greyed combo entry.
    for (const auto& device : list.devices) {
        if (device.state == DeviceState::Unauthorized) {
            ImGui::SameLine();
            ImGui::TextColored(palette.warning, ICON_FA_LOCK " accept the USB debugging prompt on %s",
                               device.serial.c_str());
            break;
        }
    }
}

void DeviceToolbar::drawPackagePicker(AppContext& context) {
    const auto& palette = theme::palette();

    if (!context.hasDevice()) {
        ImGui::TextDisabled("select a device to pick a package");
        return;
    }

    if (!packagesRequested_) {
        packagesRequested_ = true;
        refreshPackages(context);
    }

    std::string preview = context.hasPackage() ? context.selectedPackage : "No package";
    ImGui::SetNextItemWidth(300.0F);
    if (ImGui::BeginCombo("##package", preview.c_str())) {
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##pkgfilter", ICON_FA_MAGNIFYING_GLASS " filter", &packageFilter_);

        if (ImGui::Checkbox("third-party only", &thirdPartyOnly_)) refreshPackages(context);
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_ARROWS_ROTATE)) refreshPackages(context);
        ImGui::SetItemTooltip("Reload the package list");

        ImGui::Separator();

        if (packagesLoading_) {
            ImGui::TextDisabled("loading...");
        } else if (!packageError_.empty()) {
            ImGui::TextColored(palette.bad, "%s", packageError_.c_str());
        } else if (packages_.empty()) {
            ImGui::TextDisabled("no packages");
        }

        if (ImGui::BeginChild("##pkglist", ImVec2{360.0F, 320.0F})) {
            for (const auto& name : packages_) {
                if (!packageFilter_.empty() && !containsIgnoreCase(name, packageFilter_)) {
                    continue;
                }
                if (ImGui::Selectable(name.c_str(), name == context.selectedPackage)) {
                    // No frame-counter reset here: that wiped the app's recent
                    // frames, so a static screen showed nothing at all. The
                    // Frame Time panel's Reset button does it on purpose.
                    context.devices.selectPackage(name);
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CROSSHAIRS)) {
        const std::string serial = context.selectedSerial;
        context.pool.submit([&context, serial] {
            auto result = context.devices.adb().foregroundPackage(serial);
            if (!result) {
                EM_LOG_WARN(kCategory, "foreground lookup failed: " + result.describe());
                return;
            }
            auto name = std::move(result).value();
            context.dispatcher.post([&context, name = std::move(name)] {
                EM_LOG_INFO(kCategory, "foreground package: " + name);
                context.devices.selectPackage(name);
            });
        });
    }
    ImGui::SetItemTooltip("Use the app that is on screen right now");

    if (context.hasPackage()) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_XMARK)) {
            context.devices.selectPackage({});
        }
        ImGui::SetItemTooltip("Stop targeting %s", context.selectedPackage.c_str());
    }
}

void DeviceToolbar::draw(AppContext& context) {
    if (!ImGui::BeginTable("##toolbar", 3,
                           ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
        return;
    }

    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(ICON_FA_MOBILE_SCREEN);
    ImGui::SetItemTooltip("Device");
    ImGui::SameLine();
    drawDevicePicker(context);

    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(ICON_FA_CUBE);
    ImGui::SetItemTooltip("Package: the app the per-app panels target");
    ImGui::SameLine();
    drawPackagePicker(context);

    ImGui::TableNextColumn();
    bool sampling = context.metrics.running();
    if (widgets::toggleButton(ICON_FA_CHART_LINE, &sampling, "Sample device metrics")) {
        if (sampling) {
            context.metrics.start();
        } else {
            context.metrics.stop();
        }
    }

    ImGui::EndTable();
}

}  // namespace em
