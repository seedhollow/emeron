#include "ui/panels/FrameTimePanel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include <imgui.h>
#include <implot.h>

#include "adb/DeviceManager.h"
#include "app/Theme.h"
#include <IconsFontAwesome6.h>
#include "collect/FrameStats.h"
#include "core/StringUtil.h"
#include "core/TaskQueue.h"
#include "ui/Widgets.h"

namespace em {
namespace {

struct PhaseRow {
    const char* label;  // plain language, shown in the table
    const char* term;   // the gfxinfo columns it is measured between, in the tooltip
    double FrameRecord::*member;
    const char* explanation;
};

// Ordered as the frame itself proceeds, so reading the table top to bottom
// follows the frame through the pipeline.
constexpr PhaseRow kPhases[] = {
    {"Touch & input", "Input  (HandleInputStart -> AnimationStart)", &FrameRecord::inputMs,
     "Delivering touch and key events to your views and running their handlers."},
    {"Animations", "Animation  (AnimationStart -> PerformTraversalsStart)",
     &FrameRecord::animationMs,
     "Running animators and Choreographer animation callbacks."},
    {"Measure & layout", "Layout  (PerformTraversalsStart -> DrawStart)",
     &FrameRecord::layoutMs,
     "Measuring and positioning views (performTraversals). Spikes usually mean the "
     "view tree is being re-measured, often from nested wrap_content or weights."},
    {"Record drawing", "Draw  (DrawStart -> SyncQueued)", &FrameRecord::drawMs,
     "Your views' onDraw() calls, recorded into a display list on the UI thread. "
     "Expensive custom drawing lands here."},
    {"Hand to render thread", "Sync  (SyncStart -> IssueDrawCommandsStart)",
     &FrameRecord::syncMs,
     "Passing the recorded frame to the render thread, including uploading any "
     "new bitmaps. Large or frequently changing bitmaps make this slow."},
    {"Send to GPU", "Issue draw commands  (IssueDrawCommandsStart -> SwapBuffers)",
     &FrameRecord::issueMs,
     "The render thread turning the frame into GL/Vulkan commands. Heavy overdraw, "
     "shadows and complex paths cost time here."},
    {"Hand to display", "Swap  (SwapBuffers -> FrameCompleted)", &FrameRecord::swapMs,
     "Queueing the finished frame for the display compositor (SurfaceFlinger)."},
    {"GPU work", "GPU  (SwapBuffers -> GpuCompleted)", &FrameRecord::gpuMs,
     "Time the GPU itself spent drawing. Only reported on Android 10+ when the "
     "driver supports it."},
};

// "com.example.app/com.example.app.DetailActivity" -> "DetailActivity".
std::string shortWindowName(const std::string& window) {
    const auto cut = window.find_last_of("./");
    return cut == std::string::npos ? window : window.substr(cut + 1);
}

void statCell(const char* label, const char* term, const char* explanation, double value,
              double budget) {
    ImGui::TableNextColumn();
    widgets::termLabel(label, term, explanation);
    ImGui::TableNextColumn();
    ImGui::TextColored(theme::budgetColor(value, budget), "%.2f ms", value);
}

}  // namespace

void FrameTimePanel::onDeviceChanged(AppContext& context) {
    (void)context;
    selectedFrame_ = -1;
}

void FrameTimePanel::draw(AppContext& context) {
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
    if (!context.hasPackage()) {
        ImGui::TextDisabled(ICON_FA_CUBE "  No package selected.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Pick a package in the toolbar, or press Foreground to grab whatever is on "
            "screen. Frame data comes from `dumpsys gfxinfo <package> framestats`, which "
            "only reports for a hardware-accelerated process that is currently running.");
        ImGui::End();
        return;
    }

    // --- controls -----------------------------------------------------------
    if (ImGui::Button(ICON_FA_ROTATE_LEFT " Reset")) context.frames.requestReset();
    widgets::termTooltip("Reset frame counters  (dumpsys gfxinfo <package> reset)",
                         "Clears the frames held here and Android's own counters, so both "
                         "start from zero. Press it right before the interaction you want to "
                         "measure.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0F);
    ImGui::SliderFloat("Show last", &windowSeconds_, 2.0F, 120.0F, "%.0f s");
    widgets::termTooltip("Chart time window", "How many seconds of frames the chart shows.");
    ImGui::SameLine();
    ImGui::Checkbox("Follow latest", &followLatest_);
    widgets::termTooltip("Auto-scroll", "Keep the chart on the newest frames. Turn off to "
                                        "pan and zoom back through history.");
    ImGui::SameLine();
    ImGui::Checkbox("Frame breakdown", &showPhases_);
    widgets::termTooltip("Per-phase timings", "Show where one frame's time went, step by step.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0F);
    if (ImGui::SliderFloat("Display rate", &refreshOverride_, 0.0F, 20.0F,
                           refreshOverride_ <= 0.0F ? "auto" : "%.2f ms")) {
        context.frames.setRefreshPeriodOverrideMs(static_cast<double>(refreshOverride_));
    }
    widgets::termTooltip("Refresh period override",
                         "Leave on auto: emeron reads it from each frame's interval, so 60, "
                         "90, 120 Hz and variable refresh are handled. Set it only to judge "
                         "frames against a different display rate. Late frames are counted by "
                         "Android's per-frame deadline either way.");

    ImGui::Separator();

    context.frames.readSnapshot([&](const FrameStatsSnapshot& snapshot) {
        if (!snapshot.lastError.empty() && snapshot.frames.empty()) {
            ImGui::TextColored(palette.bad, ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                               snapshot.lastError.c_str());
            ImGui::Spacing();
            // Only the "no section at all" case says anything about the app;
            // anything else is a format emeron did not understand.
            if (snapshot.lastError.find("no ---PROFILEDATA---") != std::string::npos) {
                ImGui::TextWrapped(
                    "gfxinfo returned no frame data for this package. Check that it is the "
                    "right package and that its process is running. Apps drawn in software "
                    "(rare -- hardware acceleration is the default since Android 4) also "
                    "report none.");
            } else {
                ImGui::TextWrapped(
                    "emeron could not read this device's frame data. That is a bug in "
                    "emeron, not in your app -- the output of `adb shell dumpsys gfxinfo "
                    "<package> framestats` would help fix it.");
            }
            return;
        }
        if (snapshot.frames.empty()) {
            ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  No frames yet.");
            ImGui::Spacing();
            if (snapshot.summary.valid() && snapshot.summary.totalFrames == 0) {
                ImGui::TextWrapped(
                    "Android has recorded 0 frames for %s since its counters were last "
                    "reset: the app is on a screen that is not redrawing. Scroll, tap or "
                    "start an animation in the app and its frames will appear here.",
                    context.selectedPackage.c_str());
            } else {
                ImGui::TextWrapped("Interact with the app on the device; frames appear here "
                                   "as it draws them.");
            }
            return;
        }

        const JankStats& stats = snapshot.stats;
        const double budget = snapshot.refreshPeriodMs;

        // --- headline numbers ----------------------------------------------
        if (ImGui::BeginTable("##jankstats", 8,
                              ImGuiTableFlags_SizingStretchSame |
                                  ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableNextRow();
            statCell("Typical", "p50 -- median frame time",
                     "Half of all frames took less than this. The best single number for "
                     "how the app usually feels.",
                     stats.p50Ms, budget);
            statCell("90% under", "p90 -- 90th percentile",
                     "9 frames in 10 finished within this time; the slowest 1 in 10 took "
                     "longer.",
                     stats.p90Ms, budget);
            statCell("95% under", "p95 -- 95th percentile",
                     "19 frames in 20 finished within this time.", stats.p95Ms, budget);
            statCell("99% under", "p99 -- 99th percentile",
                     "99 frames in 100 finished within this time. Occasional hitches show "
                     "up here first.",
                     stats.p99Ms, budget);
            ImGui::TableNextRow();
            statCell("Average", "mean frame time",
                     "Total time divided by the number of frames. A few very slow frames "
                     "pull it up, so Typical is usually more representative.",
                     stats.meanMs, budget);
            statCell("Slowest", "max -- worst frame",
                     "The single slowest frame collected. Click the chart to inspect any "
                     "frame.",
                     stats.maxMs, budget);

            ImGui::TableNextColumn();
            widgets::termLabel(
                "Late frames", "Janky frames  (dumpsys gfxinfo)",
                stats.deadlineFrames > 0
                    ? "Frames that finished after their own deadline -- what the user sees as "
                      "a stutter, and exactly what Android counts as \"Janky frames\". The "
                      "deadline allows for pipelining, so a frame can take longer than one "
                      "refresh and still be on time."
                    : "Frames that took longer than one refresh. This Android version reports "
                      "no per-frame deadline, so duration is the only available measure.");
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::budgetColor(stats.jankPct, 5.0), "%.1f%% (%zu)",
                               stats.jankPct, stats.jankCount);

            ImGui::TableNextColumn();
            const std::string budgetHelp = format(
                "The time the display gives each frame: 16.7 ms at 60 Hz, 8.3 ms at 120 Hz. "
                "This display is running at %.0f Hz.",
                budget > 0.0 ? 1000.0 / budget : 0.0);
            widgets::termLabel("Frame budget", "Refresh period  (vsync interval)",
                               budgetHelp.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.2f ms", budget);
            ImGui::EndTable();
        }

        // --- severity breakdown --------------------------------------------
        ImGui::Spacing();
        const auto total = static_cast<float>(std::max<std::size_t>(stats.frameCount, 1));
        const struct {
            const char* label;
            const char* term;
            const char* explanation;
            std::size_t count;
            ImVec4 color;
        } buckets[] = {
            {ICON_FA_CIRCLE_CHECK " On time", "Met its deadline",
             "Finished in time for the display. The user saw no stutter.", stats.within1,
             palette.good},
            {ICON_FA_CIRCLE_EXCLAMATION " 1 frame late", "Missed by up to one refresh",
             "Shown one refresh late: a single, brief hitch.", stats.within2, palette.warning},
            {ICON_FA_TRIANGLE_EXCLAMATION " 2-3 frames late", "Missed by 2-3 refreshes",
             "A clearly visible stutter.", stats.within4, palette.bad},
            {ICON_FA_SNOWFLAKE " Froze", "Missed by 4 or more refreshes",
             "Long enough to read as the app freezing rather than stuttering.", stats.beyond4,
             palette.critical},
        };
        // Wraps instead of running off the panel edge (seen in a --screenshot).
        bool firstBucket = true;
        for (const auto& bucket : buckets) {
            const float width = 110.0F + ImGui::CalcTextSize(bucket.label).x +
                                ImGui::GetStyle().ItemSpacing.x;
            if (!firstBucket) widgets::sameLineOrWrap(width, 20.0F);
            firstBucket = false;

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bucket.color);
            const float fraction = static_cast<float>(bucket.count) / total;
            ImGui::ProgressBar(fraction, ImVec2{110.0F, 0.0F},
                               format("%zu", bucket.count).c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            widgets::termLabel(bucket.label, bucket.term, bucket.explanation);
        }

        // --- timeline -------------------------------------------------------
        plotX_.clear();
        plotY_.clear();
        plotX_.reserve(snapshot.frames.size());
        plotY_.reserve(snapshot.frames.size());
        for (const auto& frame : snapshot.frames) {
            plotX_.push_back(frame.startSec);
            plotY_.push_back(frame.totalMs);
        }

        const double latest = plotX_.empty() ? 0.0 : plotX_.back();
        const double yMax = std::max(budget * 4.0, stats.p99Ms * 1.2);

        if (ImPlot::BeginPlot("Frame time##timeline", ImVec2{-1.0F, 220.0F})) {
            ImPlot::SetupAxes("time (s)", "ms");
            ImPlot::SetupAxisLimits(ImAxis_X1, latest - static_cast<double>(windowSeconds_),
                                    latest,
                                    followLatest_ ? ImPlotCond_Always : ImPlotCond_Once);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, yMax, ImPlotCond_Always);

            // Budget lines at 1x, 2x and 4x the refresh period: the eye can
            // then read severity straight off the chart.
            for (const auto [multiple, color] :
                 {std::pair{1.0, palette.good}, std::pair{2.0, palette.warning},
                  std::pair{4.0, palette.bad}}) {
                double level = budget * multiple;
                ImPlotSpec guide;
                guide.LineColor = color;
                guide.LineWeight = 1.0F;
                guide.Flags = ImPlotInfLinesFlags_Horizontal;
                const std::string guideName = multiple == 1.0 ? std::string{"1 refresh"}
                                                              : format("%.0f refreshes", multiple);
                ImPlot::PlotInfLines(guideName.c_str(), &level, 1, guide);
            }

            ImPlotSpec fill;
            fill.FillAlpha = 0.2F;
            ImPlot::PlotShaded("frame time", plotX_.data(), plotY_.data(),
                               static_cast<int>(plotX_.size()), 0.0, fill);
            ImPlotSpec line;
            line.LineWeight = 1.6F;
            ImPlot::PlotLine("frame time", plotX_.data(), plotY_.data(),
                             static_cast<int>(plotX_.size()), line);

            // Clicking the plot selects the nearest frame for the phase table.
            if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                auto nearest = std::lower_bound(plotX_.begin(), plotX_.end(), mouse.x);
                if (nearest != plotX_.begin() &&
                    (nearest == plotX_.end() ||
                     std::abs(*nearest - mouse.x) > std::abs(*(nearest - 1) - mouse.x))) {
                    --nearest;
                }
                selectedFrame_ = static_cast<int>(std::distance(plotX_.begin(), nearest));
            }

            if (selectedFrame_ >= 0 &&
                selectedFrame_ < static_cast<int>(plotX_.size())) {
                double marker = plotX_[static_cast<std::size_t>(selectedFrame_)];
                ImPlotSpec cursor;
                cursor.LineColor = palette.accent;
                cursor.LineWeight = 1.5F;
                ImPlot::PlotInfLines("##selected", &marker, 1, cursor);
            }
            ImPlot::EndPlot();
        }

        // --- histogram ------------------------------------------------------
        histogram_.assign(plotY_.begin(), plotY_.end());
        if (ImPlot::BeginPlot("How frame times are spread##hist", ImVec2{-1.0F, 150.0F})) {
            ImPlot::SetupAxes("ms", "frames");
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, yMax, ImPlotCond_Always);
            ImPlot::PlotHistogram("frames", histogram_.data(),
                                  static_cast<int>(histogram_.size()), 48);
            double budgetLine = budget;
            ImPlotSpec guide;
            guide.LineColor = palette.warning;
            guide.LineWeight = 1.5F;
            ImPlot::PlotInfLines("budget", &budgetLine, 1, guide);
            ImPlot::EndPlot();
        }

        // --- per-frame phases ----------------------------------------------
        if (showPhases_) {
            ImGui::SeparatorText(ICON_FA_LIST_OL "  Frame breakdown");

            if (selectedFrame_ < 0 ||
                selectedFrame_ >= static_cast<int>(snapshot.frames.size())) {
                // Default to the worst frame in view: that is the one worth
                // explaining.
                auto worst = std::max_element(
                    snapshot.frames.begin(), snapshot.frames.end(),
                    [](const FrameRecord& a, const FrameRecord& b) {
                        return a.totalMs < b.totalMs;
                    });
                selectedFrame_ =
                    static_cast<int>(std::distance(snapshot.frames.begin(), worst));
            }

            const FrameRecord& frame =
                snapshot.frames[static_cast<std::size_t>(selectedFrame_)];

            ImGui::Text("Frame %d of %zu at t=%.3f s", selectedFrame_ + 1,
                        snapshot.frames.size(), frame.startSec);
            if (!frame.window.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled(ICON_FA_WINDOW_MAXIMIZE " %s",
                                    shortWindowName(frame.window).c_str());
                ImGui::SetItemTooltip("%s", frame.window.c_str());
            }
            ImGui::SameLine();
            ImGui::TextColored(theme::frameTimeColor(frame.totalMs, budget), "%.2f ms",
                               frame.totalMs);
            ImGui::SameLine();
            if (frame.excluded) {
                ImGui::TextDisabled("(excluded: flags=0x%llx)",
                                    static_cast<unsigned long long>(frame.flags));
            } else if (frame.janky) {
                ImGui::TextColored(palette.bad, ICON_FA_TRIANGLE_EXCLAMATION " JANK");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_ARROW_DOWN_WIDE_SHORT " worst")) selectedFrame_ = -1;
            ImGui::SetItemTooltip("Jump to the slowest frame");

            if (ImGui::BeginTable("##phases", 3,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthFixed, 170.0F);
                ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0F);
                ImGui::TableSetupColumn("Share of this frame");
                ImGui::TableHeadersRow();

                for (const auto& phase : kPhases) {
                    const double value = frame.*phase.member;
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(phase.label);
                    widgets::termTooltip(phase.term, phase.explanation);

                    ImGui::TableNextColumn();
                    if (value < 0.0) {
                        ImGui::TextDisabled("--");
                        widgets::termTooltip("Not reported",
                                             "This device or Android version does not "
                                             "report this step.");
                    } else {
                        ImGui::TextColored(theme::budgetColor(value, budget * 0.5), "%.2f",
                                           value);
                    }

                    ImGui::TableNextColumn();
                    if (value >= 0.0 && frame.totalMs > 0.0) {
                        ImGui::ProgressBar(
                            static_cast<float>(value / frame.totalMs),
                            ImVec2{-1.0F, 0.0F},
                            format("%.0f%%", 100.0 * value / frame.totalMs).c_str());
                    }
                }

                // Buffer queue timings are reported directly, not as a span.
                const struct {
                    const char* label;
                    const char* term;
                    const char* explanation;
                    double value;
                } extras[] = {
                    {"Wait for a free buffer", "DequeueBufferDuration",
                     "Time spent waiting for the display to hand back a buffer to draw into. "
                     "High values mean the GPU or the display is behind.",
                     frame.dequeueMs},
                    {"Submit the buffer", "QueueBufferDuration",
                     "Time spent handing the finished buffer to the compositor.",
                     frame.queueMs},
                    {"Late start", "Vsync - IntendedVsync",
                     "How long after its intended vsync the frame actually started. High "
                     "values mean the UI thread was still busy with something else.",
                     frame.vsyncLatencyMs()},
                };
                for (const auto& extra : extras) {
                    const double value = extra.value;
                    if (value < 0.0) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    widgets::termLabel(extra.label, extra.term, extra.explanation);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", value);
                    ImGui::TableNextColumn();
                }
                ImGui::EndTable();
            }
        }

        // --- platform's own numbers, for cross-checking ---------------------
        if (snapshot.summary.valid()) {
            ImGui::SeparatorText(ICON_FA_CLIPBOARD_LIST "  Android's own count");
            widgets::termTooltip("dumpsys gfxinfo summary",
                                 "Counted by Android since its counters were last reset, over "
                                 "every frame -- emeron's numbers above cover the frames it has "
                                 "collected. Press Reset to make both cover the same period.");

            const GfxSummary& g = snapshot.summary;
            const struct {
                const char* label;
                const char* term;
                const char* explanation;
                std::string value;
            } counters[] = {
                {"Frames drawn", "Total frames rendered", "", format("%ld", g.totalFrames)},
                {"Late", "Janky frames", "Frames that missed their deadline.",
                 format("%ld (%.1f%%)", g.jankyFrames, g.jankyPct)},
                {"Typical", "50th percentile", "Half of all frames were faster.",
                 format("%.0f ms", g.p50Ms)},
                {"99% under", "99th percentile", "99 frames in 100 were faster.",
                 format("%.0f ms", g.p99Ms)},
                {"Started late", "Number Missed Vsync",
                 "Frames whose work began after the vsync they were meant for, usually "
                 "because the UI thread was busy.",
                 format("%ld", g.missedVsync)},
                {"Slow to respond to touch", "Number High input latency",
                 "Frames where a touch event waited a long time before it was handled.",
                 format("%ld", g.highInputLatency)},
                {"Slow UI thread", "Number Slow UI thread",
                 "Frames where the UI thread's part (input, animation, layout, draw) took "
                 "too long.",
                 format("%ld", g.slowUiThread)},
                {"Slow GPU commands", "Number Slow issue draw commands",
                 "Frames where turning the frame into GPU commands took too long.",
                 format("%ld", g.slowDrawCommands)},
                {"Slow bitmap uploads", "Number Slow bitmap uploads",
                 "Frames delayed by uploading bitmaps to the GPU.",
                 format("%ld", g.slowBitmapUploads)},
            };
            bool firstCounter = true;
            for (const auto& counter : counters) {
                const float width = ImGui::CalcTextSize(counter.label).x +
                                    ImGui::CalcTextSize(counter.value.c_str()).x + 24.0F;
                if (!firstCounter) widgets::sameLineOrWrap(width, 18.0F);
                firstCounter = false;
                widgets::termLabel(counter.label, counter.term, counter.explanation);
                ImGui::SameLine();
                ImGui::TextUnformatted(counter.value.c_str());
            }
        }

        ImGui::TextDisabled("%zu frames collected  |  %llu updates  |  ~%.1f frames per second",
                            snapshot.frames.size(),
                            static_cast<unsigned long long>(snapshot.pollCount),
                            stats.estimatedFps);
        widgets::termTooltip("Collected frames / polls / observed FPS",
                             "emeron reads the device's last ~120 frames per window on each "
                             "update and keeps adding new ones, so it can hold far more than the "
                             "device itself does. Frames per second is how fast the app actually "
                             "drew over that period -- an idle app draws nothing, so this can be "
                             "low without anything being wrong.");

        // gfxinfo reports each window separately; all are shown together, as
        // the platform's process-wide summary above does.
        std::map<std::string, std::size_t> perWindow;
        for (const auto& f : snapshot.frames) {
            if (!f.window.empty()) ++perWindow[shortWindowName(f.window)];
        }
        if (perWindow.size() > 1) {
            std::string windows;
            for (const auto& [name, count] : perWindow) {
                if (!windows.empty()) windows += ",  ";
                windows += name + " " + std::to_string(count);
            }
            ImGui::TextDisabled(ICON_FA_WINDOW_RESTORE "  frames from %zu windows: %s",
                                perWindow.size(), windows.c_str());
        }
    });

    ImGui::End();
}

}  // namespace em
