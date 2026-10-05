#include "ui/panels/CrashLogPanel.h"

#include <chrono>
#include <ctime>
#include <system_error>

#include <imgui.h>

#include "app/Theme.h"
#include "collect/FileSystemBrowser.h"
#include "core/StringUtil.h"
#include "ui/Widgets.h"

namespace em {
namespace {

std::string timeText(std::filesystem::file_time_type when) {
    // file_time_type's clock is not system_clock, and libc++'s clock_cast is
    // not available on every toolchain this project builds with yet, so this
    // uses the older now()-anchored conversion instead.
    const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        when - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const auto timeT = std::chrono::system_clock::to_time_t(sctp);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &timeT);
#else
    localtime_r(&timeT, &parts);
#endif
    return format("%04d-%02d-%02d %02d:%02d:%02d", parts.tm_year + 1900, parts.tm_mon + 1,
                 parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
}

}  // namespace

void CrashLogPanel::refresh() {
    reports_ = crash::listReports();
    // Open the newest report straight away: it is almost always the one
    // being looked for.
    selected_ = reports_.empty() ? -1 : 0;
    selectedText_ = reports_.empty() ? std::string{} : crash::readReport(reports_.front().path);
}

void CrashLogPanel::draw(AppContext& context) {
    if (!ImGui::Begin(windowTitle(), visibleFlag())) {
        ImGui::End();
        return;
    }

    if (!loaded_) {
        loaded_ = true;
        refresh();
    }

    const auto& palette = theme::palette();

    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Refresh")) refresh();
    widgets::termTooltip("Refresh", "Look for new crash reports in the crash-logs folder.");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open folder")) {
        FileSystemBrowser::revealOnHost(context.pool, crash::logDirectory());
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(reports_.empty());
    if (ImGui::Button(ICON_FA_TRASH_CAN " Clear all")) confirmClearAll_ = true;
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("%s", crash::logDirectory().string().c_str());

    if (confirmClearAll_) {
        ImGui::OpenPopup("Clear crash reports");
        confirmClearAll_ = false;
    }
    if (ImGui::BeginPopupModal("Clear crash reports", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete all %zu crash report(s)? This cannot be undone.", reports_.size());
        if (ImGui::Button("Delete", ImVec2{120.0F, 0.0F})) {
            std::error_code ec;
            for (const auto& report : reports_) std::filesystem::remove(report.path, ec);
            refresh();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2{120.0F, 0.0F})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Separator();

    if (reports_.empty()) {
        ImGui::TextColored(palette.good, ICON_FA_CIRCLE_CHECK "  No crash reports.");
        ImGui::TextDisabled(
            "If emeron ever crashes -- a segfault, an abort, anything fatal -- a report with "
            "a backtrace and what the UI was doing at the time lands here automatically, "
            "ready to attach to a bug report.");
        ImGui::End();
        return;
    }

    ImGui::BeginChild("##list", ImVec2{280.0F, 0.0F}, ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(reports_.size()); ++i) {
        const auto& report = reports_[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        const bool isSelected = selected_ == i;
        if (ImGui::Selectable("##row", isSelected, 0, ImVec2{0.0F, 36.0F})) {
            selected_ = i;
            selectedText_ = crash::readReport(report.path);
        }
        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (ImGui::MenuItem(ICON_FA_COPY " Copy report")) {
                const std::string text =
                    isSelected ? selectedText_ : crash::readReport(report.path);
                ImGui::SetClipboardText(text.c_str());
            }
            if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Delete")) {
                std::error_code ec;
                std::filesystem::remove(report.path, ec);
                refresh();
                ImGui::EndPopup();
                ImGui::PopID();
                continue;
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(4.0F);
        ImGui::BeginGroup();
        ImGui::TextColored(palette.bad, "%s %s", ICON_FA_BOMB, report.fileName.c_str());
        ImGui::TextDisabled("%s  |  %s", timeText(report.modified).c_str(),
                            humanBytes(static_cast<double>(report.sizeBytes)).c_str());
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##detail", ImVec2{0.0F, 0.0F}, ImGuiChildFlags_Borders);
    if (selected_ < 0) {
        ImGui::TextDisabled("Select a report to view it.");
    } else {
        if (ImGui::Button(ICON_FA_COPY " Copy")) ImGui::SetClipboardText(selectedText_.c_str());
        ImGui::Separator();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0.0F, 0.0F});
        ImGui::TextUnformatted(selectedText_.c_str());
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace em
