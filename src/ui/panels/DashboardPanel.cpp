#include "ui/panels/DashboardPanel.h"

#include <algorithm>
#include <string>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "adb/DeviceManager.h"
#include "app/Theme.h"
#include "collect/MetricSampler.h"
#include "core/StringUtil.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace em {
namespace {

// A compact labelled readout. Returns the width consumed so tiles can wrap.
void statTile(const char* label, const std::string& value, ImVec4 color,
              const char* term = nullptr, const char* explanation = nullptr) {
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", label);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.35F);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    if (term != nullptr) widgets::termTooltip(term, explanation);
}

// Collectors use a negative value to mean "this device did not report it".
// Precision and suffix rather than a format string, so the format passed to
// em::format stays a literal and -Wformat can still check it.
std::string valueOrDash(double value, int precision, const char* suffix) {
    if (value < 0.0) return "--";
    return format("%.*f%s", precision, value, suffix);
}

// Shared setup for every time-series plot on this panel.
bool beginTimePlot(const char* title, const MetricHistory& history, float windowSeconds,
                   bool lockX, const char* yLabel, double yMin, double yMax) {
    if (!ImPlot::BeginPlot(title, ImVec2{-1.0F, 180.0F})) return false;

    ImPlot::SetupAxes("time (s)", yLabel, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);

    const double now = history.t.empty() ? 0.0 : history.t.back();
    if (lockX) {
        ImPlot::SetupAxisLimits(ImAxis_X1, now - static_cast<double>(windowSeconds), now,
                                ImPlotCond_Always);
    } else {
        ImPlot::SetupAxisLimits(ImAxis_X1, now - static_cast<double>(windowSeconds), now,
                                ImPlotCond_Once);
    }
    if (yMax > yMin) {
        ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);
    } else {
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Once);
    }
    return true;
}

// The ring buffers are contiguous, so ImPlotSpec::Offset lets ImPlot read them
// straight out of storage with no intermediate copy.
ImPlotSpec ringSpec(const RingBuffer<double>& t) {
    ImPlotSpec spec;
    spec.Offset = t.plotOffset();
    spec.LineWeight = 1.6F;
    return spec;
}

void plotSeries(const char* label, const RingBuffer<double>& t, const RingBuffer<double>& y) {
    const int count = static_cast<int>(std::min(t.size(), y.size()));
    if (count <= 1) return;
    ImPlot::PlotLine(label, t.data(), y.data(), count, ringSpec(t));
}

void plotShadedSeries(const char* label, const RingBuffer<double>& t,
                      const RingBuffer<double>& y) {
    const int count = static_cast<int>(std::min(t.size(), y.size()));
    if (count <= 1) return;

    ImPlotSpec fill = ringSpec(t);
    fill.FillAlpha = 0.22F;
    ImPlot::PlotShaded(label, t.data(), y.data(), count, 0.0, fill);
    ImPlot::PlotLine(label, t.data(), y.data(), count, ringSpec(t));
}

}  // namespace

void DashboardPanel::draw(AppContext& context) {
    if (!ImGui::Begin(windowTitle(), visibleFlag())) {
        ImGui::End();
        return;
    }

    const auto& palette = theme::palette();

    if (!context.hasDevice()) {
        ImGui::TextDisabled(ICON_FA_PLUG "  No device selected.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Attach a device over USB (or `adb connect <host:port>` for wireless) and pick it "
            "from the toolbar. If nothing shows up, check that the USB debugging prompt was "
            "accepted on the device.");
        ImGui::End();
        return;
    }

    const ProbeSupport support = context.metrics.support();

    // --- controls -----------------------------------------------------------
    ImGui::SetNextItemWidth(160.0F);
    ImGui::SliderFloat("Show last", &windowSeconds_, 10.0F, 600.0F, "%.0f s");
    widgets::termTooltip("Chart time window", "How many seconds of history the charts show.");
    ImGui::SameLine();
    ImGui::Checkbox("Follow latest", &lockXAxis_);
    widgets::termTooltip("Auto-scroll", "Keep the charts on the newest sample. Turn off to "
                                        "pan and zoom back through history.");
    ImGui::SameLine();
    ImGui::Checkbox("Each CPU core", &showPerCore_);
    widgets::termTooltip("Per-core CPU", "Add a line per CPU core to the CPU chart.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!support.thermal);
    ImGui::Checkbox("All temperature sensors", &showThermalZones_);
    ImGui::EndDisabled();
    widgets::termTooltip("Thermal zones  (/sys/class/thermal)",
                         support.thermal
                             ? "List every temperature sensor the device exposes."
                             : "This device does not let adb read its temperature sensors.");

    ImGui::Separator();

    context.metrics.readHistory([&](const MetricHistory& history) {
        const MetricSample& latest = history.latest;

        if (history.empty()) {
            if (!history.lastError.empty()) {
                ImGui::TextColored(palette.bad, "%s", history.lastError.c_str());
            } else if (!context.metrics.running()) {
                ImGui::TextDisabled(ICON_FA_PAUSE "  Sampling is paused.");
            } else {
                ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  Waiting for the first sample...");
            }
            return;
        }

        // --- stat tiles -----------------------------------------------------
        statTile(ICON_FA_MICROCHIP " CPU", valueOrDash(latest.cpuTotalPct, 1, "%"),
                 theme::budgetColor(latest.cpuTotalPct, 85.0), "Device CPU usage  (/proc/stat)",
                 "How busy all CPU cores are together, across every app and the system.");
        ImGui::SameLine(0.0F, 28.0F);

        const double memPct = latest.memTotalMiB > 0.0
                                  ? 100.0 * latest.memUsedMiB / latest.memTotalMiB
                                  : -1.0;
        statTile(ICON_FA_MEMORY " Memory used", valueOrDash(memPct, 0, "%"),
                 theme::budgetColor(memPct, 90.0), "MemTotal - MemAvailable  (/proc/meminfo)",
                 "How much of the device's RAM is in use. Near the top, Android starts "
                 "killing background apps.");
        ImGui::SameLine(0.0F, 28.0F);

        statTile((std::string{icons::battery(latest.batteryLevelPct)} + " Battery").c_str(),
                 valueOrDash(latest.batteryLevelPct, 0, "%"), palette.accent,
                 "Battery level  (dumpsys battery)", "Charge remaining.");
        ImGui::SameLine(0.0F, 28.0F);

        statTile(ICON_FA_TEMPERATURE_HALF " Battery temp", valueOrDash(latest.batteryTempC, 1, " C"),
                 theme::budgetColor(latest.batteryTempC, 40.0),
                 "Battery temperature  (dumpsys battery)",
                 "Sustained load warms the battery before it shows up as slow frames; above "
                 "about 40 C many devices start limiting performance.");
        ImGui::SameLine(0.0F, 28.0F);

        statTile(ICON_FA_FIRE " Hottest chip", valueOrDash(latest.maxThermalC, 1, " C"),
                 theme::budgetColor(latest.maxThermalC, 70.0),
                 "SoC max -- hottest thermal zone  (/sys/class/thermal)",
                 "The hottest temperature sensor on the processor. Around 70-85 C the device "
                 "slows the CPU and GPU to cool down (thermal throttling), which shows up as "
                 "slower frames. \"--\" means the device does not let adb read these "
                 "sensors, which is common on MediaTek phones.");

        if (latest.hasApp()) {
            ImGui::SameLine(0.0F, 28.0F);
            statTile(ICON_FA_GAUGE_HIGH " App CPU", valueOrDash(latest.appCpuPct, 1, "%"),
                     theme::budgetColor(latest.appCpuPct, 100.0),
                     "utime + stime  (/proc/<pid>/stat)",
                     "CPU used by the selected app's main process. 100% means one full core, "
                     "so a busy app on several cores can go above 100%.");
            ImGui::SameLine(0.0F, 28.0F);
            statTile(ICON_FA_MEMORY " App memory", valueOrDash(latest.appRssMiB, 0, " MiB"),
                     palette.accent, "RSS -- resident set size  (VmRSS, /proc/<pid>/status)",
                     "Memory the app's process currently holds in RAM, including shared "
                     "libraries. A steady climb while you use the app can point to a leak.");
        }

        ImGui::Spacing();
        if (!context.hasPackage()) {
            ImGui::TextDisabled(
                "Pick a package in the toolbar to add per-app CPU and memory.");
        } else if (!latest.hasApp()) {
            ImGui::TextColored(palette.warning, "%s is not running on the device.",
                               context.selectedPackage.c_str());
        }

        ImGui::Spacing();

        // --- plots ----------------------------------------------------------
        if (beginTimePlot(ICON_FA_MICROCHIP "  CPU##plot", history, windowSeconds_, lockXAxis_, "%", 0.0, 100.0)) {
            plotShadedSeries("total", history.t, history.cpuTotal);
            if (showPerCore_) {
                for (std::size_t i = 0; i < history.cpuCores.size(); ++i) {
                    const std::string label = "cpu" + std::to_string(i);
                    plotSeries(label.c_str(), history.t, history.cpuCores[i]);
                }
            }
            if (latest.hasApp()) {
                plotSeries(context.selectedPackage.c_str(), history.t, history.appCpu);
            }
            ImPlot::EndPlot();
        }

        const double memMax = latest.memTotalMiB > 0.0 ? latest.memTotalMiB * 1.05 : 0.0;
        if (beginTimePlot(ICON_FA_MEMORY "  Memory##plot", history, windowSeconds_, lockXAxis_, "MiB", 0.0,
                          memMax)) {
            plotShadedSeries("used", history.t, history.memUsed);
            plotSeries("available", history.t, history.memAvailable);
            if (latest.hasApp()) {
                plotSeries((context.selectedPackage + " RSS").c_str(), history.t,
                           history.appRss);
            }
            ImPlot::EndPlot();
        }

        if (support.battery || support.thermal) {
            if (beginTimePlot(ICON_FA_TEMPERATURE_HALF "  Thermal and power##plot", history, windowSeconds_, lockXAxis_,
                              "C / mA", 0.0, 0.0)) {
                plotSeries("battery (C)", history.t, history.batteryTemp);
                plotSeries("hottest chip (C)", history.t, history.maxThermal);
                plotSeries("battery current (mA)", history.t, history.batteryCurrent);
                ImPlot::EndPlot();
            }
        }

        // --- thermal zone detail -------------------------------------------
        if (showThermalZones_ && !latest.thermalC.empty()) {
            ImGui::SeparatorText(ICON_FA_FIRE "  Thermal zones");
            if (ImGui::BeginTable("##zones", 2,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY,
                                  ImVec2{0.0F, 160.0F})) {
                ImGui::TableSetupColumn("Zone");
                ImGui::TableSetupColumn("Temperature", ImGuiTableColumnFlags_WidthFixed, 120.0F);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (std::size_t i = 0; i < latest.thermalC.size(); ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const std::string name =
                        i < support.thermalZoneNames.size() && !support.thermalZoneNames[i].empty()
                            ? support.thermalZoneNames[i]
                            : "thermal_zone" + std::to_string(i);
                    ImGui::TextUnformatted(name.c_str());

                    ImGui::TableNextColumn();
                    ImGui::TextColored(theme::budgetColor(latest.thermalC[i], 70.0), "%.1f C",
                                       latest.thermalC[i]);
                }
                ImGui::EndTable();
            }
        }

        ImGui::Spacing();
        ImGui::TextDisabled("%llu readings, every %lld ms  |  %d CPU cores  |  reading:%s%s%s%s%s",
                            static_cast<unsigned long long>(history.sampleCount),
                            static_cast<long long>(context.metrics.interval().count()),
                            support.coreCount,
                            support.procStat ? " CPU" : "",
                            support.meminfo ? ", memory" : "",
                            support.battery ? ", battery" : "",
                            support.thermal ? ", temperature" : "",
                            support.perApp ? ", app" : "");
        widgets::termTooltip("Samples / probes",
                             "What emeron could read from this device. Anything missing is "
                             "not readable over adb here, so its tile shows \"--\".");

        if (!history.lastError.empty()) {
            ImGui::TextColored(palette.bad, "%s", history.lastError.c_str());
        }
    });

    ImGui::End();
}

}  // namespace em
