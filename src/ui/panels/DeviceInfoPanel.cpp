#include "ui/panels/DeviceInfoPanel.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/Theme.h"
#include "ui/Icons.h"
#include <IconsFontAwesome6.h>
#include "collect/DeviceInfo.h"
#include "core/StringUtil.h"
#include "core/TaskQueue.h"
#include "ui/Widgets.h"

namespace em {

void DeviceInfoPanel::onDeviceChanged(AppContext& context) {
    requested_ = false;
    context.deviceInfo.clear();
}

void DeviceInfoPanel::draw(AppContext& context) {
    if (!ImGui::Begin(windowTitle(), visibleFlag())) {
        ImGui::End();
        return;
    }

    const auto& palette = theme::palette();

    if (!context.hasDevice()) {
        ImGui::TextDisabled(ICON_FA_PLUG "  No device selected.");
        ImGui::End();
        return;
    }

    if (!requested_) {
        requested_ = true;
        context.deviceInfo.refresh(context.pool);
    }

    if (ImGui::Button(ICON_FA_ARROWS_ROTATE)) context.deviceInfo.refresh(context.pool);
    ImGui::SetItemTooltip("Read the device again");
    ImGui::SameLine();
    ImGui::Checkbox(ICON_FA_LIST " Raw properties", &showRawProperties_);

    context.deviceInfo.readSnapshot([&](const DeviceInfoSnapshot& snapshot) {
        if (snapshot.loading) {
            ImGui::SameLine();
            ImGui::TextDisabled("loading...");
        }
        if (!snapshot.lastError.empty()) {
            ImGui::TextColored(palette.bad, "%s", snapshot.lastError.c_str());
        }
        if (!snapshot.loaded) {
            if (!snapshot.loading) ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  No data yet.");
            return;
        }

        ImGui::Separator();

        if (showRawProperties_) {
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputTextWithHint("##propfilter", "filter properties...", &propertyFilter_);

            if (ImGui::BeginTable("##props", 2,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 280.0F);
                ImGui::TableSetupColumn("Value");
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (const auto& [key, value] : snapshot.properties) {
                    if (!propertyFilter_.empty() &&
                        !containsIgnoreCase(key, propertyFilter_) &&
                        !containsIgnoreCase(value, propertyFilter_)) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(key.c_str());
                    if (ImGui::IsItemClicked()) ImGui::SetClipboardText(key.c_str());

                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", value.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("%zu properties (click a key to copy it)",
                                snapshot.properties.size());
            return;
        }

        // --- curated sections ----------------------------------------------
        for (const auto& section : snapshot.sections) {
            const std::string header =
                std::string{icons::deviceInfoSection(section.title)} + "  " + section.title + "###" +
                section.title;
            if (!ImGui::CollapsingHeader(header.c_str(),
                                         ImGuiTreeNodeFlags_DefaultOpen)) {
                continue;
            }

            const std::string tableId = "##tbl_" + section.title;
            if (!ImGui::BeginTable(tableId.c_str(), 2,
                                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_SizingStretchProp)) {
                continue;
            }
            ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 160.0F);
            ImGui::TableSetupColumn("Value");

            for (const auto& field : section.fields) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", field.label.c_str());
                if (!field.sourceKey.empty()) {
                    widgets::termTooltip(field.sourceKey.c_str(), field.explanation.c_str());
                }

                ImGui::TableNextColumn();
                // Multi-line values (dumps) get their own scrolling box so a
                // long display dump cannot push the rest of the panel away.
                if (field.value.find('\n') != std::string::npos) {
                    const std::string childId = "##dump_" + field.label;
                    ImGui::BeginChild(childId.c_str(), ImVec2{-1.0F, 110.0F},
                                      ImGuiChildFlags_Borders);
                    ImGui::TextUnformatted(field.value.c_str());
                    ImGui::EndChild();
                } else {
                    ImGui::TextWrapped("%s", field.value.c_str());
                    if (ImGui::IsItemClicked()) {
                        ImGui::SetClipboardText(field.value.c_str());
                    }
                }
            }
            ImGui::EndTable();
        }
    });

    ImGui::End();
}

}  // namespace em
