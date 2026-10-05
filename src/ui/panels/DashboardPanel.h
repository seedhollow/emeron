#pragma once

#include <IconsFontAwesome6.h>

#include "ui/Panel.h"

namespace em {

// Live device-wide and per-app metrics: CPU, memory, battery, thermal.
class DashboardPanel final : public Panel {
public:
    static constexpr const char* kId = "Dashboard";

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_GAUGE_HIGH; }
    void draw(AppContext& context) override;

private:
    float windowSeconds_ = 60.0F;
    bool showPerCore_ = false;
    bool showThermalZones_ = false;
    bool lockXAxis_ = true;
};

}  // namespace em
