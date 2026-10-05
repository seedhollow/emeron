#include "ui/panels/PerfettoPanel.h"

#include <algorithm>
#include <string>
#include <system_error>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/Theme.h"
#include <IconsFontAwesome6.h>
#include "core/CrashHandler.h"
#include "core/Log.h"
#include "core/StringUtil.h"
#include "core/TaskQueue.h"
#include "ui/Notifications.h"
#include "ui/Widgets.h"

namespace em {
namespace {

constexpr const char* kCategory = "perfetto-ui";

// The atrace categories worth offering as checkboxes. The full list is long
// and mostly irrelevant to app performance work.
// `label` is what the checkbox says; `name` is the atrace category id that
// goes into the config, shown in the tooltip.
struct CategoryOption {
    const char* name;
    const char* label;
    const char* description;
};

constexpr CategoryOption kCategories[] = {
    {"gfx", "Drawing & rendering", "RenderThread, DrawFrame and buffer queues."},
    {"view", "Views (layout)", "View hierarchy: measure, layout and draw."},
    {"input", "Touch & input", "How touch and key events reach the app."},
    {"wm", "Window manager", "Window add/remove, focus and layout by the system."},
    {"am", "App launches", "ActivityManager: app and activity starts, transitions."},
    {"sched", "Thread scheduling", "Which thread ran when. Also needs \"Which thread ran "
                                   "on which core\" above."},
    {"freq", "CPU speed", "CPU clock frequency changes."},
    {"idle", "CPU sleep", "When CPU cores go idle and wake up."},
    {"binder_driver", "Calls to system services",
     "Binder transactions between processes -- shows the app waiting on the system."},
    {"hal", "Hardware drivers", "Hardware abstraction layer calls."},
    {"res", "Resource loading", "Loading layouts, drawables and other resources."},
    {"dalvik", "Garbage collection & JIT",
     "ART runtime: GC pauses, JIT compilation, class loading."},
    {"camera", "Camera", "Camera subsystem."},
    {"aidl", "AIDL interface calls", "Calls through AIDL-generated interfaces."},
    {"memory", "Memory events", "Memory pressure and allocation events."},
    {"disk", "Disk reads & writes", "Block I/O."},
};

bool hasCategory(const std::vector<std::string>& list, const char* name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

void toggleCategory(std::vector<std::string>& list, const char* name, bool enabled) {
    const auto it = std::find(list.begin(), list.end(), name);
    if (enabled && it == list.end()) {
        list.emplace_back(name);
    } else if (!enabled && it != list.end()) {
        list.erase(it);
    }
}

}  // namespace

PerfettoPanel::PerfettoPanel() {
    sql_ = "SELECT name, dur / 1e6 AS dur_ms\n"
           "FROM slice\n"
           "ORDER BY dur DESC\n"
           "LIMIT 50;";
}

void PerfettoPanel::onDeviceChanged(AppContext& context) {
    (void)context;
    supportProbed_ = false;
}

void PerfettoPanel::refreshTraceList(AppContext& context) {
    traceFiles_.clear();
    const std::filesystem::path dir = context.workspaceDir / "traces";

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
        if (!entry.is_regular_file(ec)) continue;
        traceFiles_.push_back(entry.path());
    }
    // Newest first: the trace just captured is the one wanted.
    std::sort(traceFiles_.begin(), traceFiles_.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) {
                  std::error_code inner;
                  return std::filesystem::last_write_time(a, inner) >
                         std::filesystem::last_write_time(b, inner);
              });
    traceListLoaded_ = true;
}

void PerfettoPanel::drawCaptureTab(AppContext& context) {
    const auto& palette = theme::palette();

    if (!context.hasDevice()) {
        ImGui::TextDisabled(ICON_FA_PLUG "  No device selected.");
        return;
    }

    if (!supportProbed_) {
        supportProbed_ = true;
        context.perfetto.probeSupport(context.pool);
    }

    if (context.perfetto.supportKnown()) {
        const bool ok = context.perfetto.supported();
        ImGui::TextColored(ok ? palette.good : palette.warning, "%s",
                           context.perfetto.supportMessage().c_str());
    } else {
        ImGui::TextDisabled("checking tracing support...");
    }

    ImGui::Separator();

    // --- options ------------------------------------------------------------
    int durationSec = options_.durationMs / 1000;
    ImGui::SetNextItemWidth(220.0F);
    if (ImGui::SliderInt("Record for", &durationSec, 1, 120, "%d s")) {
        options_.durationMs = durationSec * 1000;
    }
    widgets::termTooltip("Duration", "How long to record before the trace is pulled back.");

    int bufferMb = options_.bufferSizeKb / 1024;
    ImGui::SetNextItemWidth(220.0F);
    if (ImGui::SliderInt("Memory for the trace", &bufferMb, 4, 512, "%d MiB")) {
        options_.bufferSizeKb = bufferMb * 1024;
    }
    widgets::termTooltip("Buffer size  (ring buffer)",
                         "Device memory the trace is recorded into. When it fills up the "
                         "oldest data is overwritten, so if the start of a trace is "
                         "missing, raise this.");

    ImGui::Checkbox("Which thread ran on which core", &options_.ftraceSchedAndFreq);
    widgets::termTooltip("ftrace sched + cpufreq",
                         "sched_switch, sched_waking, cpu_frequency, cpu_idle. Shows which "
                         "thread ran on which CPU core, at what clock speed, and when a "
                         "thread was ready but had to wait.");
    ImGui::SameLine();
    ImGui::Checkbox("Frame timing", &options_.frameTimeline);
    widgets::termTooltip("Frame timeline  (android.surfaceflinger.frametimeline)",
                         "Per-frame expected vs. actual timing from the display system. "
                         "Perfetto uses it to say why each late frame was late.");

    ImGui::Checkbox("Memory & CPU per process", &options_.processStats);
    widgets::termTooltip("Process stats  (linux.process_stats)",
                         "Process names, threads and periodic memory counters.");
    ImGui::SameLine();
    ImGui::Checkbox("Log lines", &options_.logcat);
    widgets::termTooltip("logcat  (android.log)",
                         "Include logcat messages on the trace timeline.");
    ImGui::SameLine();
    ImGui::Checkbox("Java heap snapshot", &options_.javaHeapGraph);
    widgets::termTooltip("Java heap graph  (android.java_hprof)",
                         "Dump every object on the app's Java heap and who holds it, to "
                         "find leaks. Large, and pauses the app while it is taken.");

    ImGui::SeparatorText(ICON_FA_TAGS "  System events to record");
    widgets::termTooltip("atrace categories",
                         "Groups of trace points built into Android. Hover each one to see "
                         "its category id.");

    const float columnWidth = 230.0F;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x /
                                                     columnWidth));
    if (ImGui::BeginTable("##categories", columns, ImGuiTableFlags_NoSavedSettings)) {
        for (const auto& category : kCategories) {
            ImGui::TableNextColumn();
            bool enabled = hasCategory(options_.atraceCategories, category.name);
            ImGui::PushID(category.name);
            if (ImGui::Checkbox(category.label, &enabled)) {
                toggleCategory(options_.atraceCategories, category.name, enabled);
            }
            ImGui::PopID();
            const std::string term = std::string{"atrace category: "} + category.name;
            widgets::termTooltip(term.c_str(), category.description);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText(ICON_FA_CUBE "  Apps with custom trace sections");
    widgets::termTooltip("atrace apps",
                         "Trace.beginSection / endSection calls in your own code only show "
                         "up for apps listed here.");
    ImGui::TextDisabled(
        "Your own Trace.beginSection spans are only recorded for the apps listed here.");

    ImGui::SetNextItemWidth(280.0F);
    ImGui::InputTextWithHint("##atraceapp", "com.example.app", atraceAppBuffer_,
                             sizeof(atraceAppBuffer_));
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS " Add") && atraceAppBuffer_[0] != '\0') {
        options_.atraceApps.emplace_back(atraceAppBuffer_);
        atraceAppBuffer_[0] = '\0';
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CROSSHAIRS " Add selected") && context.hasPackage()) {
        if (!hasCategory(options_.atraceApps, context.selectedPackage.c_str())) {
            options_.atraceApps.push_back(context.selectedPackage);
        }
    }

    for (std::size_t i = 0; i < options_.atraceApps.size();) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextUnformatted(options_.atraceApps[i].c_str());
        ImGui::SameLine();
        const bool removed = ImGui::SmallButton(ICON_FA_XMARK);
        ImGui::PopID();
        if (removed) {
            options_.atraceApps.erase(options_.atraceApps.begin() +
                                      static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }

    // --- run ----------------------------------------------------------------
    ImGui::Separator();

    const CaptureStatus status = context.perfetto.status();

    ImGui::BeginDisabled(status.busy());
    if (ImGui::Button(ICON_FA_CIRCLE_DOT " Start capture", ImVec2{150.0F, 0.0F})) {
        options_.traceName.clear();
        crash::setBreadcrumb("Perfetto: starting a capture");
        if (context.perfetto.start(options_, context.workspaceDir / "traces")) {
            EM_LOG_INFO(kCategory, "capture started");
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!status.busy());
    if (ImGui::Button(ICON_FA_STOP " Cancel", ImVec2{100.0F, 0.0F})) context.perfetto.cancel();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Checkbox("Show config", &showConfig_);
    widgets::termTooltip("Trace config  (TraceConfig text proto)",
                         "Show the exact settings that will be sent to the device's "
                         "tracing service.");

    switch (status.phase) {
        case CaptureStatus::Phase::Recording:
            ImGui::ProgressBar(static_cast<float>(status.progress), ImVec2{-1.0F, 0.0F},
                               "recording");
            break;
        case CaptureStatus::Phase::Pulling:
            ImGui::TextColored(palette.accent, "pulling trace from the device...");
            break;
        case CaptureStatus::Phase::Done:
            ImGui::TextColored(palette.good, "%s", status.message.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_MAGNIFYING_GLASS_CHART " Open in Explore")) {
                openedTrace_ = status.localPath;
                if (auto opened = context.traceProcessor.openTrace(openedTrace_); !opened) {
                    queryError_ = opened.error().message;
                } else {
                    queryError_.clear();
                }
                traceListLoaded_ = false;
            }
            break;
        case CaptureStatus::Phase::Failed:
            ImGui::TextColored(palette.bad, "%s", status.message.c_str());
            if (!status.deviceStderr.empty()) {
                ImGui::BeginChild("##capturestderr", ImVec2{-1.0F, 90.0F},
                                  ImGuiChildFlags_Borders);
                ImGui::TextUnformatted(status.deviceStderr.c_str());
                ImGui::EndChild();
            }
            break;
        case CaptureStatus::Phase::Starting:
            ImGui::TextDisabled("%s", status.message.c_str());
            break;
        case CaptureStatus::Phase::Idle:
            break;
    }

    if (showConfig_) {
        configPreview_ = buildTraceConfig(options_);
        ImGui::SeparatorText(ICON_FA_FILE_CODE "  Config to be sent");
        ImGui::BeginChild("##configpreview", ImVec2{-1.0F, 220.0F}, ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(configPreview_.c_str());
        ImGui::EndChild();
        if (ImGui::Button(ICON_FA_COPY " Copy config")) ImGui::SetClipboardText(configPreview_.c_str());
    }
}

void PerfettoPanel::runQuery(AppContext& context) {
    if (queryRunning_) return;
    queryRunning_ = true;
    queryError_.clear();

    const std::string sql = sql_;
    context.pool.submit([this, &context, sql] {
        auto result = context.traceProcessor.query(sql);
        if (!result) {
            const std::string message = result.error().message;
            context.dispatcher.post([this, message] {
                queryError_ = message;
                queryRunning_ = false;
            });
            return;
        }
        auto value = std::move(result).value();
        context.dispatcher.post([this, value = std::move(value)]() mutable {
            result_ = std::move(value);
            queryRunning_ = false;
        });
    });
}

void PerfettoPanel::drawResultTable() {
    if (result_.columns.empty()) {
        ImGui::TextDisabled(ICON_FA_TABLE "  No results yet.");
        return;
    }

    ImGui::TextDisabled("%zu rows in %.0f ms", result_.rows.size(), result_.elapsedMs);

    const auto columnCount = static_cast<int>(std::min<std::size_t>(result_.columns.size(), 64));
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                       ImGuiTableFlags_Resizable;

    if (!ImGui::BeginTable("##queryresult", columnCount, kFlags, ImVec2{-1.0F, -1.0F})) return;

    for (int i = 0; i < columnCount; ++i) {
        ImGui::TableSetupColumn(result_.columns[static_cast<std::size_t>(i)].c_str());
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    for (const auto& row : result_.rows) {
        ImGui::TableNextRow();
        for (int i = 0; i < columnCount; ++i) {
            ImGui::TableNextColumn();
            const auto index = static_cast<std::size_t>(i);
            if (index < row.size()) ImGui::TextUnformatted(row[index].c_str());
        }
    }
    ImGui::EndTable();
}

void PerfettoPanel::drawExploreTab(AppContext& context) {
    const auto& palette = theme::palette();
    const TraceProcessorState state = context.traceProcessor.state();

    if (!state.binaryFound) {
        ImGui::TextColored(palette.warning, "trace_processor_shell not found");
        ImGui::TextWrapped("%s", state.lastError.c_str());
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS " Re-detect")) context.traceProcessor.relocate();
        ImGui::Spacing();
        ImGui::TextDisabled(
            "curl -LO https://get.perfetto.dev/trace_processor && chmod +x trace_processor");
        return;
    }

    ImGui::TextDisabled("%s", state.binaryPath.c_str());
    if (!state.binaryOrigin.empty()) ImGui::SetItemTooltip("found via %s", state.binaryOrigin.c_str());
    if (!state.version.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", state.version.c_str());
    }

    if (!traceListLoaded_) refreshTraceList(context);

    // --- trace selection ----------------------------------------------------
    const std::string preview =
        openedTrace_.empty() ? "select a trace" : openedTrace_.filename().string();
    ImGui::SetNextItemWidth(380.0F);
    if (ImGui::BeginCombo("##trace", preview.c_str())) {
        if (traceFiles_.empty()) {
            ImGui::TextDisabled("no traces in %s", (context.workspaceDir / "traces").string().c_str());
        }
        for (const auto& path : traceFiles_) {
            const bool selected = path == openedTrace_;
            if (ImGui::Selectable(path.filename().string().c_str(), selected)) {
                openedTrace_ = path;
                if (auto opened = context.traceProcessor.openTrace(path); !opened) {
                    queryError_ = opened.error().message;
                } else {
                    queryError_.clear();
                    result_ = QueryResult{};
                }
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE)) refreshTraceList(context);
    ImGui::SetItemTooltip("Look for new traces in the workspace");

    if (!state.traceLoaded) {
        ImGui::TextDisabled("Open a trace to query it.");
        return;
    }

    // --- canned queries -----------------------------------------------------
    const auto standard = TraceProcessor::standardQueries();
    ImGui::SetNextItemWidth(280.0F);
    if (ImGui::BeginCombo("##standard", "Standard queries")) {
        for (std::size_t i = 0; i < standard.size(); ++i) {
            if (ImGui::Selectable(standard[i].title,
                                  selectedStandardQuery_ == static_cast<int>(i))) {
                selectedStandardQuery_ = static_cast<int>(i);
                sql_ = standard[i].sql;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", standard[i].note);
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(queryRunning_);
    if (ImGui::Button(ICON_FA_PLAY " Run", ImVec2{90.0F, 0.0F})) runQuery(context);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+Enter");

    ImGui::SameLine();
    if (queryRunning_) ImGui::TextColored(palette.accent, "running...");

    // --- editor -------------------------------------------------------------
    ImGui::InputTextMultiline("##sql", &sql_, ImVec2{-1.0F, 130.0F});
    if (ImGui::IsItemFocused() &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)) {
        runQuery(context);
    }

    if (!queryError_.empty()) {
        ImGui::TextColored(palette.bad, "%s", queryError_.c_str());
    }

    ImGui::Separator();
    drawResultTable();
}

void PerfettoPanel::draw(AppContext& context) {
    // Before Begin(): a capture finishing while this tab is hidden still
    // gets its toast.
    {
        const CaptureStatus status = context.perfetto.status();
        if (status.phase != lastPhase_) {
            if (status.phase == CaptureStatus::Phase::Done) {
                const auto path = status.localPath;
                notify::postWithAction(notify::Kind::Success, "Trace captured", status.message,
                                       ICON_FA_MAGNIFYING_GLASS_CHART "  Open in Explore",
                                       [this, &context, path] {
                                           openedTrace_ = path;
                                           if (auto opened = context.traceProcessor.openTrace(path); !opened) {
                                               queryError_ = opened.error().message;
                                           } else {
                                               queryError_.clear();
                                           }
                                           traceListLoaded_ = false;
                                           setVisible(true);
                                           ImGui::SetWindowFocus(windowKey(kId).c_str());
                                       });
            } else if (status.phase == CaptureStatus::Phase::Failed) {
                notify::error("Trace capture failed", status.message);
            }
            lastPhase_ = status.phase;
        }
    }

    if (!ImGui::Begin(windowTitle(), visibleFlag())) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##perfettotabs")) {
        if (ImGui::BeginTabItem(ICON_FA_CIRCLE_DOT " Capture###capture")) {
            drawCaptureTab(context);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_FA_MAGNIFYING_GLASS_CHART " Explore###explore")) {
            drawExploreTab(context);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

}  // namespace em
