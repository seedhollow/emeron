#pragma once

#include <vector>

#include <IconsFontAwesome6.h>

#include "ui/Panel.h"

namespace em {

// Frame-time timeline, jank percentiles, histogram and per-frame phase
// breakdown from `dumpsys gfxinfo <pkg> framestats`.
class FrameTimePanel final : public Panel {
public:
    static constexpr const char* kId = "Frame Time";

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_STOPWATCH; }
    void draw(AppContext& context) override;
    void onDeviceChanged(AppContext& context) override;

private:
    int selectedFrame_ = -1;
    float windowSeconds_ = 20.0F;
    bool followLatest_ = true;
    bool showPhases_ = true;
    float refreshOverride_ = 0.0F;  // 0 means "infer"

    // Scratch buffers reused across frames so drawing allocates nothing.
    std::vector<double> plotX_;
    std::vector<double> plotY_;
    std::vector<double> histogram_;
};

}  // namespace em
