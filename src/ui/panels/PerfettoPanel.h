#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "collect/PerfettoCapture.h"
#include "collect/TraceProcessor.h"
#include <IconsFontAwesome6.h>

#include "ui/Panel.h"

namespace em {

// Capture a Perfetto trace from the host, then query it with SQL through
// trace_processor_shell.
class PerfettoPanel final : public Panel {
public:
    static constexpr const char* kId = "Perfetto";

    PerfettoPanel();

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_WAVE_SQUARE; }
    void draw(AppContext& context) override;
    void onDeviceChanged(AppContext& context) override;

private:
    void drawCaptureTab(AppContext& context);
    void drawExploreTab(AppContext& context);
    void drawResultTable();
    void runQuery(AppContext& context);
    void refreshTraceList(AppContext& context);

    PerfettoCaptureOptions options_;
    std::string configPreview_;
    bool showConfig_ = false;
    bool supportProbed_ = false;

    // Explore tab
    std::vector<std::filesystem::path> traceFiles_;
    std::filesystem::path openedTrace_;
    std::string sql_;
    QueryResult result_;
    std::string queryError_;
    CaptureStatus::Phase lastPhase_ = CaptureStatus::Phase::Idle;  // for the finished/failed toast
    bool queryRunning_ = false;
    bool traceListLoaded_ = false;
    int selectedStandardQuery_ = -1;
    char atraceAppBuffer_[128] = {};
};

}  // namespace em
