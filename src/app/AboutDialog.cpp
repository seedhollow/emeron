#include "app/AboutDialog.h"

#include <string>

#include <imgui.h>

#include <IconsFontAwesome6.h>

#include "app/Theme.h"
#include "core/StringUtil.h"

#ifndef EMERON_VERSION
#define EMERON_VERSION "0.0.0"
#endif

namespace em {
namespace {

// Edit these when the project has settled them.
constexpr const char* kCreator = "rrosetta";
constexpr const char* kLicense = "MIT License (see LICENSE.txt)";
constexpr const char* kRepository = "https://github.com/seedhollow/emeron";

struct Credit {
    const char* name;
    const char* license;
    const char* url;
};

constexpr Credit kCredits[] = {
    {"Dear ImGui (docking)", "MIT", "https://github.com/ocornut/imgui"},
    {"ImPlot", "MIT", "https://github.com/epezent/implot"},
    {"GLFW", "Zlib/libpng", "https://github.com/glfw/glfw"},
    {"nativefiledialog-extended", "Zlib", "https://github.com/btzy/nativefiledialog-extended"},
    {"ImGuiNotify", "MIT", "https://github.com/TyomaVader/ImGuiNotify"},
    {"IconFontCppHeaders", "Zlib", "https://github.com/juliettef/IconFontCppHeaders"},
    {"miniz", "MIT", "https://github.com/richgel999/miniz"},
    {"scrcpy server", "Apache-2.0", "https://github.com/Genymobile/scrcpy"},
};

std::string versionInfo() {
    return format("emeron %s\nCreator: %s\nLicense: %s\nRepository: %s", EMERON_VERSION,
                  kCreator, kLicense, kRepository);
}

}  // namespace

void drawAboutDialog(bool& open) {
    constexpr const char* kTitle = "About emeron";
    if (open && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, &open, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const auto& palette = theme::palette();
    ImGui::Text("emeron");
    ImGui::SameLine();
    ImGui::TextDisabled("version %s", EMERON_VERSION);
    ImGui::TextDisabled(
        "Host-side Android inspector: live metrics, jank analysis, file "
        "explorer, Perfetto.");
    ImGui::Spacing();

    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::BeginTable("##about", 2, ImGuiTableFlags_SizingFixedFit)) {
        const auto row = [](const char* key) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", key);
            ImGui::TableNextColumn();
        };
        row("Creator");
        ImGui::Text("%s", kCreator);
        row("License");
        ImGui::TextWrapped("%s", kLicense);
        row("Repository");
        ImGui::TextLinkOpenURL(kRepository, kRepository);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("Built with");
    ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
    for (const Credit& credit : kCredits) {
        ImGui::BulletText("%s, %s", credit.name, credit.license);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", credit.url);
    }
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Icons by Font Awesome (https://fontawesome.com), CC BY 4.0. Fonts: "
        "Font Awesome Free, SIL OFL 1.1.");
    ImGui::TextDisabled(
        "adb, trace_processor_shell and jadx (Apache-2.0) are not bundled; they are "
        "found on this machine.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_COPY "  Copy version info")) {
        ImGui::SetClipboardText(versionInfo().c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        open = false;
    }
    ImGui::EndPopup();
}

}  // namespace em
