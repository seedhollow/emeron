#include "ui/panels/SensorPanel.h"

#include <algorithm>
#include <string>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <implot.h>

#include "app/Theme.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"
#include <IconsFontAwesome6.h>
#include "collect/SensorReader.h"
#include "core/StringUtil.h"
#include "core/TaskQueue.h"

namespace em {
namespace {

const char* axisName(int index, int axisCount) {
    static constexpr const char* kXyz[] = {"x", "y", "z", "w"};
    if (axisCount >= 3 && index < 4) return kXyz[index];
    return "value";
}

std::string formatValues(const std::vector<double>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out += ", ";
        out += format("%.4f", values[i]);
    }
    return out.empty() ? "--" : out;
}

}  // namespace

void SensorPanel::onDeviceChanged(AppContext& context) {
    requested_ = false;
    charted_.clear();
    context.sensors.clear();
}

void SensorPanel::onShutdown(AppContext& context) { context.sensors.stopPolling(); }

void SensorPanel::drawSensorTable(AppContext& context) {
    const auto& palette = theme::palette();

    context.sensors.readSnapshot([&](const SensorSnapshot& snapshot) {
        if (!snapshot.lastError.empty()) {
            ImGui::TextColored(palette.bad, "%s", snapshot.lastError.c_str());
        }
        if (snapshot.sensors.empty()) {
            ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  No sensors enumerated yet.");
            return;
        }

        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_Hideable |
                                           ImGuiTableFlags_Resizable |
                                           ImGuiTableFlags_SizingStretchProp;

        if (!ImGui::BeginTable("##sensors", 7, kFlags, ImVec2{0.0F, 280.0F})) return;

        // Same trap Device Files had: ~500 px of fixed columns in a ~340 px
        // dock squeezed the sensor names out entirely (seen in a --screenshot).
        // Type is carried by the icon and the name; the secondary columns start
        // hidden -- right-click the header to show them.
        ImGui::TableSetupColumn(ICON_FA_CHART_LINE, ImGuiTableColumnFlags_WidthFixed, 30.0F);
        ImGui::TableSetupColumn("Sensor", ImGuiTableColumnFlags_WidthStretch |
                                              ImGuiTableColumnFlags_NoHide, 2.2F);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed |
                                            ImGuiTableColumnFlags_DefaultHide, 120.0F);
        ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthFixed |
                                              ImGuiTableColumnFlags_DefaultHide, 100.0F);
        ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed |
                                            ImGuiTableColumnFlags_DefaultHide, 110.0F);
        ImGui::TableSetupColumn("Reports", ImGuiTableColumnFlags_WidthFixed |
                                            ImGuiTableColumnFlags_DefaultHide, 80.0F);
        ImGui::TableSetupColumn("Latest value", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupScrollFreeze(0, 1);
        static constexpr const char* kTerms[] = {
            "Chart", "Sensor", "Type  (android.sensor.*)", "Vendor",
            "Rate  (min - max Hz)", "Reporting mode", "Latest value"};
        static constexpr const char* kExplanations[] = {
            "Tick to draw this sensor's values on a live chart below.",
            "Sensor name as the device reports it.",
            "What kind of sensor it is.",
            "Who made the sensor chip or driver.",
            "How often the sensor can deliver readings, in readings per second.",
            "continuous: reports at a steady rate. on-change: only when the value changes. "
            "one-shot: fires once (e.g. significant motion). special: anything else.",
            "Most recent reading. \"idle\" means no app on the device has the sensor turned "
            "on, so Android is not reading it."};
        widgets::headersRowWithTooltips(kTerms, kExplanations, 7);

        for (const auto& sensor : snapshot.sensors) {
            if (!filter_.empty() && !containsIgnoreCase(sensor.name, filter_) &&
                !containsIgnoreCase(sensor.typeString, filter_)) {
                continue;
            }

            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(sensor.handle));

            ImGui::TableNextColumn();
            bool charted = charted_.contains(sensor.handle);
            if (ImGui::Checkbox("##chart", &charted)) {
                if (charted) {
                    charted_.insert(sensor.handle);
                } else {
                    charted_.erase(sensor.handle);
                }
            }

            ImGui::TableNextColumn();
            ImGui::TextColored(theme::palette().accent, "%s", icons::sensor(sensor.typeId));
            ImGui::SameLine();
            ImGui::TextUnformatted(sensor.name.c_str());
            // The secondary columns start hidden, so everything they hold is
            // also reachable here.
            if (ImGui::BeginItemTooltip()) {
                ImGui::Text("%s %s", icons::sensor(sensor.typeId), sensor.shortType().c_str());
                ImGui::TextDisabled("made by %s  |  reports %s  |  %s", sensor.vendor.c_str(),
                                    sensor.reportingMode.empty() ? "--" : sensor.reportingMode.c_str(),
                                    sensor.wakeUp ? "can wake the device" : "does not wake the device");
                if (sensor.maxRateHz > 0.0) {
                    ImGui::TextDisabled("%.1f - %.1f readings per second", sensor.minRateHz,
                                        sensor.maxRateHz);
                }
                ImGui::TextDisabled("handle 0x%08x  |  version %d  |  permission: %s", sensor.handle,
                                    sensor.version,
                                    sensor.permission.empty() ? "n/a" : sensor.permission.c_str());
                ImGui::EndTooltip();
            }

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", sensor.shortType().c_str());

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", sensor.vendor.c_str());

            ImGui::TableNextColumn();
            if (sensor.maxRateHz > 0.0) {
                ImGui::TextDisabled("%.1f - %.1f Hz", sensor.minRateHz, sensor.maxRateHz);
            } else {
                ImGui::TextDisabled("--");
            }

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", sensor.reportingMode.empty()
                                          ? "--"
                                          : sensor.reportingMode.c_str());

            ImGui::TableNextColumn();
            const auto it = snapshot.latest.find(sensor.handle);
            if (it == snapshot.latest.end()) {
                ImGui::TextDisabled("not running");
            } else {
                ImGui::TextColored(palette.accent, "%s",
                                   formatValues(it->second.values).c_str());
            }

            ImGui::PopID();
        }
        ImGui::EndTable();

        ImGui::TextDisabled("%zu sensors  |  %d turned on by apps  |  %llu reads",
                            snapshot.sensors.size(), snapshot.activeSensorCount,
                            static_cast<unsigned long long>(snapshot.pollCount));
        widgets::termTooltip("Active sensors / polls",
                             "Sensors only produce values while an app on the device has "
                             "them turned on. \"reads\" counts how often emeron has asked "
                             "the device for fresh values.");
    });
}

void SensorPanel::drawCharts(AppContext& context) {
    if (charted_.empty()) return;

    context.sensors.readSnapshot([&](const SensorSnapshot& snapshot) {
        for (const std::uint32_t handle : charted_) {
            const auto descriptor =
                std::find_if(snapshot.sensors.begin(), snapshot.sensors.end(),
                             [handle](const SensorDescriptor& s) { return s.handle == handle; });
            if (descriptor == snapshot.sensors.end()) continue;

            const auto trace = snapshot.traces.find(handle);
            const std::string title = descriptor->name + "##chart" + std::to_string(handle);

            if (!ImPlot::BeginPlot(title.c_str(), ImVec2{-1.0F, 170.0F})) continue;
            ImPlot::SetupAxes("time (s)", nullptr, ImPlotAxisFlags_NoTickLabels);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -1.0, 1.0, ImPlotCond_Once);

            if (trace != snapshot.traces.end() && !trace->second.t.empty()) {
                const SensorTrace& series = trace->second;
                const double now = series.t.back();
                ImPlot::SetupAxisLimits(ImAxis_X1, now - 60.0, now, ImPlotCond_Always);

                for (std::size_t axis = 0; axis < series.axes.size(); ++axis) {
                    const int count =
                        static_cast<int>(std::min(series.t.size(), series.axes[axis].size()));
                    if (count <= 1) continue;
                    ImPlotSpec spec;
                    spec.Offset = series.t.plotOffset();
                    spec.LineWeight = 1.6F;
                    ImPlot::PlotLine(axisName(static_cast<int>(axis), descriptor->axisCount()),
                                     series.t.data(), series.axes[axis].data(), count, spec);
                }
            } else {
                ImPlot::Annotation(0.5, 0.5, theme::palette().muted, ImVec2{0.0F, 0.0F}, true,
                                   "no readings -- no app on the device has this sensor on");
            }
            ImPlot::EndPlot();
        }
    });
}

void SensorPanel::draw(AppContext& context) {
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
        context.sensors.enumerate(context.pool);
    }

    if (ImGui::Button(ICON_FA_ARROWS_ROTATE)) context.sensors.enumerate(context.pool);
    ImGui::SetItemTooltip("List the sensors again");

    ImGui::SameLine();
    bool polling = context.sensors.polling();
    if (ImGui::Button(polling ? ICON_FA_PAUSE " Stop live values###poll"
                              : ICON_FA_PLAY " Live values###poll")) {
        if (polling) {
            context.sensors.stopPolling();
        } else {
            context.sensors.startPolling();
        }
    }
    widgets::termTooltip("Poll  (dumpsys sensorservice)",
                         "Keep reading the latest sensor values from the device. Each read "
                         "dumps the whole sensor service, which costs a little device CPU -- "
                         "leave it off when you do not need it.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0F);
    if (ImGui::SliderInt("Read every", &intervalMs_, 250, 5000, "%d ms")) {
        context.sensors.setInterval(std::chrono::milliseconds{intervalMs_});
    }
    widgets::termTooltip("Poll interval", "How long to wait between reads.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0F);
    ImGui::InputTextWithHint("##sensorfilter", ICON_FA_FILTER " filter", &filter_);

    ImGui::SameLine();
    ImGui::Checkbox(ICON_FA_FILE_LINES " Show raw output", &showRawDump_);
    widgets::termTooltip("Raw dump  (dumpsys sensorservice)",
                         "Show the device's full text output instead of the table.");

    // Be explicit about the limit rather than letting the user conclude the
    // tool is broken when the gyroscope reads "idle".
    ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
    ImGui::TextWrapped(
        "Every sensor on the device is listed below. Values only appear for sensors that an "
        "app on the device has turned on, and they refresh at most about twice a second -- "
        "that is a limit of reading them over adb, not of this tool. Smooth gyroscope "
        "streaming needs a helper app on the device; see the note in SensorReader.h.");
    ImGui::PopStyleColor();

    ImGui::Separator();

    if (showRawDump_) {
        context.sensors.readSnapshot([](const SensorSnapshot& snapshot) {
            ImGui::BeginChild("##rawdump", ImVec2{-1.0F, -1.0F}, ImGuiChildFlags_Borders);
            ImGui::TextUnformatted(snapshot.rawDump.c_str());
            ImGui::EndChild();
        });
        ImGui::End();
        return;
    }

    drawSensorTable(context);
    drawCharts(context);

    ImGui::End();
}

}  // namespace em
