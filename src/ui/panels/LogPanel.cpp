#include "ui/panels/LogPanel.h"

#include <array>
#include <chrono>
#include <ctime>
#include <string>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/Theme.h"
#include <IconsFontAwesome6.h>
#include "core/StringUtil.h"

namespace em {
namespace {

ImVec4 levelColor(LogLevel level) {
    const auto& palette = theme::palette();
    switch (level) {
        case LogLevel::Error: return palette.critical;
        case LogLevel::Warn:  return palette.warning;
        case LogLevel::Info:  return ImGui::GetStyleColorVec4(ImGuiCol_Text);
        case LogLevel::Debug:
        case LogLevel::Trace: break;
    }
    return palette.muted;
}

const char* levelIcon(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return ICON_FA_CIRCLE_XMARK;
        case LogLevel::Warn:  return ICON_FA_TRIANGLE_EXCLAMATION;
        case LogLevel::Info:  return ICON_FA_CIRCLE_INFO;
        case LogLevel::Debug: return ICON_FA_BUG;
        case LogLevel::Trace: break;
    }
    return ICON_FA_ELLIPSIS;
}

std::string clockText(std::chrono::system_clock::time_point when) {
    const auto timeT = std::chrono::system_clock::to_time_t(when);
    const auto fractional = std::chrono::duration_cast<std::chrono::milliseconds>(
                                when.time_since_epoch()) %
                            std::chrono::seconds{1};
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &timeT);
#else
    localtime_r(&timeT, &parts);
#endif
    return format("%02d:%02d:%02d.%03d", parts.tm_hour, parts.tm_min, parts.tm_sec,
                  static_cast<int>(fractional.count()));
}

}  // namespace

void LogPanel::draw(AppContext& context) {
    (void)context;

    if (!ImGui::Begin(windowTitle(), visibleFlag())) {
        ImGui::End();
        return;
    }

    static constexpr std::array<const char*, 5> kLevelNames{"Trace", "Debug", "Info", "Warn",
                                                            "Error"};

    ImGui::SetNextItemWidth(110.0F);
    ImGui::Combo("##level", &minLevel_, kLevelNames.data(),
                 static_cast<int>(kLevelNames.size()));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimum level to display");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0F);
    ImGui::InputTextWithHint("##logfilter", "filter...", &filter_);

    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll_);

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ERASER " Clear")) {
        Logger::instance().clear();
        cache_.clear();
        lastRevision_ = 0;
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_COPY " Copy all")) {
        std::string blob;
        for (const auto& entry : cache_) {
            blob += clockText(entry.when);
            blob += ' ';
            blob += toString(entry.level);
            blob += " [";
            blob += entry.category;
            blob += "] ";
            blob += entry.message;
            blob += '\n';
        }
        ImGui::SetClipboardText(blob.c_str());
    }

    ImGui::Separator();

    // The logger is written from worker threads, so only re-snapshot when its
    // revision moved rather than copying the ring every frame.
    const std::uint64_t revision = Logger::instance().revision();
    if (revision != lastRevision_) {
        cache_ = Logger::instance().snapshot();
        lastRevision_ = revision;
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##log", 4, kFlags)) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 74.0F);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        // An explicit weight keeps ImGui from deriving one from the content
        // width, which divided 0 by 0 when the panel was first laid out hidden.
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const auto& entry : cache_) {
            if (static_cast<int>(entry.level) < minLevel_) continue;
            if (!filter_.empty() && !containsIgnoreCase(entry.message, filter_) &&
                !containsIgnoreCase(entry.category, filter_)) {
                continue;
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", clockText(entry.when).c_str());

            ImGui::TableNextColumn();
            ImGui::TextColored(levelColor(entry.level), "%s %s", levelIcon(entry.level),
                               std::string{toString(entry.level)}.c_str());

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", entry.category.c_str());

            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", entry.message.c_str());
        }

        if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0F) {
            ImGui::SetScrollHereY(1.0F);
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace em
