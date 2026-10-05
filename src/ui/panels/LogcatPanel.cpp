#include "ui/panels/LogcatPanel.h"

#include <algorithm>
#include <array>

#include <imgui.h>
#include <IconsFontAwesome6.h>
#include <imgui_stdlib.h>

#include "adb/DeviceManager.h"
#include "app/Theme.h"
#include "core/StringUtil.h"
#include "core/TaskQueue.h"
#include "ui/Widgets.h"

namespace em {
namespace {

constexpr std::array<const char*, 7> kLevelNames{"Verbose", "Debug", "Info",
                                                 "Warn",    "Error", "Fatal",
                                                 "Silent"};

// -b selections offered in the UI, indexed by LogcatPanel::bufferChoice_.
struct BufferChoice {
    const char* label;
    const char* description;
    std::vector<std::string> buffers;
};

const std::vector<BufferChoice>& bufferChoices() {
    static const std::vector<BufferChoice> choices{
        {"Standard", "main, system and crash buffers -- what logcat picks on its own", {}},
        {"App only", "main buffer: app output only", {"main"}},
        {"App + crashes", "main and crash buffers: app output plus crash stack traces",
         {"main", "crash"}},
        {"Everything", "all buffers, including events and radio: very chatty",
         {"all"}},
    };
    return choices;
}

ImVec4 levelColor(LogcatLevel level) {
    const auto& palette = theme::palette();
    switch (level) {
        case LogcatLevel::Fatal:   return palette.critical;
        case LogcatLevel::Error:   return palette.bad;
        case LogcatLevel::Warn:    return palette.warning;
        case LogcatLevel::Info:    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
        case LogcatLevel::Debug:   return palette.accent;
        case LogcatLevel::Verbose:
        case LogcatLevel::Silent:
        case LogcatLevel::Unknown: break;
    }
    return palette.muted;
}

std::string formatEntry(const LogcatEntry& entry) {
    if (entry.raw) return entry.message;
    return entry.deviceTime + "  " + std::to_string(entry.pid) + "  " +
           std::to_string(entry.tid) + " " + toChar(entry.level) + " " + entry.tag + ": " +
           entry.message;
}

}  // namespace

void LogcatPanel::onDeviceChanged(AppContext& context) {
    (void)context;
    cacheValid_ = false;
    filtered_.clear();
}

void LogcatPanel::onShutdown(AppContext& context) { context.logcat.stop(); }

void LogcatPanel::rebuildFilter(const LogcatSnapshot& snapshot) {
    filtered_.clear();
    filtered_.reserve(snapshot.entries.size());
    for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
        if (filter_.accepts(snapshot.entries[i], snapshot)) {
            filtered_.push_back(static_cast<std::uint32_t>(i));
        }
    }
}

void LogcatPanel::drawToolbar(AppContext& context) {
    using widgets::sameLineOrWrap;

    // One row of actions and filters. The device-side stream settings, which
    // are set once and rarely touched, live behind the sliders button: as
    // separate rows they took six lines and left the log table itself with no
    // visible rows in the default layout.
    const bool streaming = context.logcat.running();
    if (ImGui::Button(streaming ? ICON_FA_PAUSE : ICON_FA_PLAY)) {
        if (streaming) {
            context.logcat.stop();
        } else {
            context.logcat.start();
        }
    }
    widgets::termTooltip(streaming ? "Pause" : "Start  (adb logcat)",
                         streaming ? "Stop receiving new log lines."
                                   : "Start showing the device's log as it is written.");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ERASER)) {
        context.logcat.clearBuffer();
        cacheValid_ = false;
    }
    ImGui::SetItemTooltip("Clear the lines held here");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH_CAN)) {
        context.logcat.clearDeviceBuffer(context.pool);
        cacheValid_ = false;
    }
    widgets::termTooltip("Clear the device's log  (adb logcat -c)",
                         "Wipes the log on the device itself. Do this right before "
                         "reproducing a bug so only the relevant lines are left.");

    ImGui::SameLine();
    widgets::toggleButton(ICON_FA_ARROW_DOWN_LONG, &autoScroll_, "Follow new lines");

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_SLIDERS)) ImGui::OpenPopup("##streamoptions");
    ImGui::SetItemTooltip("Stream and display options");
    drawOptionsPopup(context);

    // --- host-side filters --------------------------------------------------
    sameLineOrWrap(100.0F, 16.0F);
    ImGui::SetNextItemWidth(100.0F);
    if (ImGui::Combo("##level", &minLevel_, kLevelNames.data(),
                     static_cast<int>(kLevelNames.size()))) {
        filter_.minLevel = static_cast<LogcatLevel>(minLevel_);
        cacheValid_ = false;
    }
    widgets::termTooltip("Minimum level",
                         "Show this level and anything more severe. Verbose shows "
                         "everything; Error shows only errors and crashes.");

    sameLineOrWrap(140.0F);
    ImGui::SetNextItemWidth(140.0F);
    if (ImGui::InputTextWithHint("##tag", ICON_FA_TAG " tag", &filter_.tag)) cacheValid_ = false;
    widgets::termTooltip("Tag",
                         "The label code passes to Log.d(TAG, ...), usually the class name. "
                         "Matches part of the tag, ignoring case.");

    const float checkboxes = ImGui::CalcTextSize("Hide matches My app only").x + 90.0F;
    sameLineOrWrap(180.0F + checkboxes);
    ImGui::SetNextItemWidth(std::max(180.0F, ImGui::GetContentRegionAvail().x - checkboxes));
    if (ImGui::InputTextWithHint("##text", ICON_FA_MAGNIFYING_GLASS " search messages",
                                 &filter_.text)) {
        cacheValid_ = false;
    }

    ImGui::SameLine();
    if (ImGui::Checkbox("Hide matches", &filter_.excludeText)) cacheValid_ = false;
    widgets::termTooltip("Exclude", "Hide lines that match the search instead of showing "
                                    "only those lines.");

    ImGui::SameLine();
    ImGui::BeginDisabled(scope_ != static_cast<int>(LogcatScope::SelectedProcess));
    if (ImGui::Checkbox("My app only", &filter_.onlyTrackedPids)) cacheValid_ = false;
    ImGui::EndDisabled();
    widgets::termTooltip("Filter by PID",
                         "Only show lines written by the selected app's process (its "
                         "process id, PID).");
}

void LogcatPanel::drawOptionsPopup(AppContext& context) {
    if (!ImGui::BeginPopup("##streamoptions")) return;

    LogcatOptions options = context.logcat.options();
    bool optionsChanged = false;

    ImGui::SeparatorText("What the device sends");

    ImGui::SetNextItemWidth(170.0F);
    static constexpr std::array<const char*, 2> kScopeNames{"Every app",
                                                            "Selected app"};
    if (ImGui::Combo("Logs from", &scope_, kScopeNames.data(),
                     static_cast<int>(kScopeNames.size()))) {
        options.scope = static_cast<LogcatScope>(scope_);
        filter_.scope = options.scope;
        optionsChanged = true;
        cacheValid_ = false;
    }
    widgets::termTooltip("Scope", "Whose log lines to collect: every process on the "
                                  "device, or just the app picked in the toolbar.");

    ImGui::BeginDisabled(scope_ != static_cast<int>(LogcatScope::SelectedProcess));
    if (ImGui::Checkbox("Filter on the device", &deviceSideFilter_)) {
        options.deviceSideFilter = deviceSideFilter_;
        optionsChanged = true;
    }
    widgets::termTooltip(
        "Device-side filter  (logcat --pid)",
        "Let the phone drop other apps' lines before sending them, which saves USB "
        "traffic. Only works when the app runs as a single process. Turn it off to "
        "keep other apps' lines around for context.");
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(170.0F);
    if (ImGui::Combo("Lowest level sent", &deviceMinLevel_, kLevelNames.data(),
                     static_cast<int>(kLevelNames.size()))) {
        options.deviceMinLevel = static_cast<LogcatLevel>(deviceMinLevel_);
        optionsChanged = true;
    }
    widgets::termTooltip(
        "Device minimum priority  (logcat *:LEVEL)",
        "Lines below this level never leave the device, so the level filter in the "
        "toolbar cannot bring them back. Leave on Verbose unless the log is too busy.");

    ImGui::SetNextItemWidth(170.0F);
    const auto& choices = bufferChoices();
    if (ImGui::BeginCombo("Log types", choices[static_cast<std::size_t>(bufferChoice_)].label)) {
        for (std::size_t i = 0; i < choices.size(); ++i) {
            if (ImGui::Selectable(choices[i].label, bufferChoice_ == static_cast<int>(i))) {
                bufferChoice_ = static_cast<int>(i);
                options.buffers = choices[i].buffers;
                optionsChanged = true;
            }
            ImGui::SetItemTooltip("%s", choices[i].description);
        }
        ImGui::EndCombo();
    }
    widgets::termTooltip("Buffers  (logcat -b)",
                         "Android keeps separate logs: main (apps), system, crash, events "
                         "and radio. Pick which ones to read.");

    ImGui::SetNextItemWidth(170.0F);
    if (ImGui::DragInt("Earlier lines", &backlogLines_, 10.0F, 0, 20'000)) {
        options.backlogLines = backlogLines_;
        optionsChanged = true;
    }
    widgets::termTooltip("Backlog  (logcat -T N)",
                         "How many lines written before you pressed start to show as well.");

    ImGui::SeparatorText("What emeron shows");

    ImGui::SetNextItemWidth(170.0F);
    if (ImGui::DragInt("Keep up to", &capacityLines_, 100.0F, 500, 500'000, "%d lines")) {
        context.logcat.setCapacity(static_cast<std::size_t>(capacityLines_));
    }
    widgets::termTooltip("Capacity", "How many lines emeron holds in memory. The oldest "
                                     "ones are dropped past this.");
    ImGui::Checkbox("Wrap long messages", &wrapMessages_);
    ImGui::Checkbox("Show thread column", &showTid_);
    widgets::termTooltip("TID -- thread id",
                         "Add a column with the thread that wrote each line, to tell the "
                         "main thread apart from background work.");

    if (optionsChanged) context.logcat.setOptions(options);
    ImGui::EndPopup();
}

void LogcatPanel::drawTable(AppContext& context, const LogcatSnapshot& snapshot) {
    const auto& palette = theme::palette();

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_SizingFixedFit;

    const int columns = showTid_ ? 6 : 5;
    if (!ImGui::BeginTable("##logcat", columns, kFlags, ImVec2{-1.0F, -1.0F})) return;

    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 110.0F);
    ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthFixed, 60.0F);
    if (showTid_) ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, 56.0F);
    ImGui::TableSetupColumn("Lvl", ImGuiTableColumnFlags_WidthFixed, 28.0F);
    ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthFixed, 150.0F);
    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupScrollFreeze(0, 1);
    {
        static constexpr const char* kTime = "Device time";
        static constexpr const char* kPid = "PID -- process id";
        static constexpr const char* kTid = "TID -- thread id";
        static constexpr const char* kLevel = "Level  (V D I W E F)";
        static constexpr const char* kTag = "Tag";
        static constexpr const char* kMessage = "Message";
        const char* terms[] = {kTime, kPid, kTid, kLevel, kTag, kMessage};
        const char* explanations[] = {
            "When the line was written, by the device's clock.",
            "Number of the process that wrote the line. Each running app has its own.",
            "Number of the thread that wrote the line. Equal to the PID on the main thread.",
            "How serious the line is: Verbose, Debug, Info, Warn, Error, Fatal.",
            "The label passed to Log.d(TAG, ...), usually the class name.",
            "The text that was logged."};
        if (!showTid_) {
            std::rotate(terms + 2, terms + 3, terms + 6);
            std::rotate(explanations + 2, explanations + 3, explanations + 6);
        }
        widgets::headersRowWithTooltips(terms, explanations, columns);
    }

    // Clipper: only the visible rows are laid out, so a 20k-line buffer costs
    // the same to draw as a 50-line one.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(filtered_.size()));

    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const LogcatEntry& entry = snapshot.entries[filtered_[static_cast<std::size_t>(row)]];
            const ImVec4 color = levelColor(entry.level);

            ImGui::TableNextRow();
            ImGui::PushID(row);

            if (entry.raw) {
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                if (showTid_) ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextColored(palette.muted, "%s", entry.message.c_str());
                ImGui::PopID();
                continue;
            }

            ImGui::TableNextColumn();
            ImGui::TextColored(palette.muted, "%s", entry.deviceTime.c_str());

            ImGui::TableNextColumn();
            ImGui::TextColored(palette.muted, "%d", entry.pid);

            if (showTid_) {
                ImGui::TableNextColumn();
                ImGui::TextColored(palette.muted, "%d", entry.tid);
            }

            ImGui::TableNextColumn();
            ImGui::TextColored(color, "%c", toChar(entry.level));

            ImGui::TableNextColumn();
            ImGui::TextColored(color, "%s", entry.tag.c_str());

            ImGui::TableNextColumn();
            if (wrapMessages_) {
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextWrapped("%s", entry.message.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::TextColored(color, "%s", entry.message.c_str());
            }

            if (ImGui::BeginPopupContextItem("##row")) {
                if (ImGui::MenuItem("Copy line")) {
                    ImGui::SetClipboardText(formatEntry(entry).c_str());
                }
                if (ImGui::MenuItem("Copy message")) {
                    ImGui::SetClipboardText(entry.message.c_str());
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Filter by this tag")) {
                    filter_.tag = entry.tag;
                    cacheValid_ = false;
                }
                if (ImGui::MenuItem("Clear filters")) {
                    filter_.tag.clear();
                    filter_.text.clear();
                    cacheValid_ = false;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    clipper.End();

    // Only stick to the bottom while the user is already there, so scrolling
    // back to read something is not fought by the incoming stream.
    if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0F) {
        ImGui::SetScrollHereY(1.0F);
    }

    ImGui::EndTable();
    (void)context;
}

void LogcatPanel::draw(AppContext& context) {
    if (!ImGui::Begin(windowTitle(), visibleFlag())) {
        ImGui::End();
        return;
    }

    const auto& palette = theme::palette();

    if (!context.hasDevice()) {
        ImGui::TextDisabled("No device selected.");
        ImGui::End();
        return;
    }

    // Start streaming the first time the panel is looked at with a device
    // attached, rather than burning a logcat process from app launch.
    if (!startedOnce_) {
        startedOnce_ = true;
        context.logcat.start();
    }

    drawToolbar(context);

    const std::uint64_t revision = context.logcat.revision();

    context.logcat.readSnapshot([&](const LogcatSnapshot& snapshot) {
        const std::string signature = filter_.signature(snapshot);
        if (!cacheValid_ || revision != cachedRevision_ || signature != cachedSignature_) {
            rebuildFilter(snapshot);
            cachedRevision_ = revision;
            cachedSignature_ = signature;
            cacheValid_ = true;
        }

        // --- status line ----------------------------------------------------
        if (ImGui::SmallButton(ICON_FA_COPY)) {
            std::string blob;
            blob.reserve(filtered_.size() * 96);
            for (const std::uint32_t index : filtered_) {
                blob += formatEntry(snapshot.entries[index]);
                blob += '\n';
            }
            ImGui::SetClipboardText(blob.c_str());
        }
        ImGui::SetItemTooltip("Copy the lines shown");
        ImGui::SameLine();

        if (snapshot.streaming) {
            ImGui::TextColored(palette.good, "live");
        } else if (context.logcat.running()) {
            ImGui::TextColored(palette.warning, "connecting...");
        } else {
            ImGui::TextDisabled("stopped");
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%zu shown of %zu held", filtered_.size(), snapshot.entries.size());

        if (snapshot.droppedToCap > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::TextColored(palette.muted, "%llu oldest dropped",
                               static_cast<unsigned long long>(snapshot.droppedToCap));
            widgets::termTooltip("Aged out", "Lines removed to stay under the \"Keep up "
                                             "to\" limit in the options.");
        }

        if (!snapshot.trackedPids.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            std::string pids;
            for (const int pid : snapshot.trackedPids) {
                if (!pids.empty()) pids += ", ";
                pids += std::to_string(pid);
            }
            ImGui::TextColored(palette.accent, "app process %s", pids.c_str());
            widgets::termTooltip("PID -- process id", snapshot.commandLine.c_str());
        } else if (context.hasPackage() &&
                   filter_.scope == LogcatScope::SelectedProcess) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::TextColored(palette.warning, "%s is not running",
                               context.selectedPackage.c_str());
        }

        if (snapshot.usingDeviceSideFilter) {
            ImGui::SameLine();
            ImGui::TextDisabled("| filtered on the device");
            widgets::termTooltip("logcat --pid", "The device only sends the app's own lines.");
        }

        if (!context.hasPackage() && filter_.scope == LogcatScope::SelectedProcess) {
            ImGui::SameLine();
            ImGui::TextColored(palette.warning, "| no package selected");
            ImGui::SetItemTooltip(
                "Logs are limited to the selected app. Pick an app in the toolbar, or set\n"
                "\"Logs from\" to Every app under the sliders button.");
        }

        if (!snapshot.lastError.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(palette.bad, "| %s", snapshot.lastError.c_str());
        }

        ImGui::Separator();
        drawTable(context, snapshot);
    });

    ImGui::End();
}

}  // namespace em
