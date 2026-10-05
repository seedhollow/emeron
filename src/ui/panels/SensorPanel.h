#pragma once

#include <cstdint>
#include <set>
#include <string>

#include <IconsFontAwesome6.h>

#include "ui/Panel.h"

namespace em {

class SensorPanel final : public Panel {
public:
    static constexpr const char* kId = "Sensors";

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_COMPASS; }
    void draw(AppContext& context) override;
    void onDeviceChanged(AppContext& context) override;
    void onShutdown(AppContext& context) override;

private:
    void drawSensorTable(AppContext& context);
    void drawCharts(AppContext& context);

    std::set<std::uint32_t> charted_;
    std::string filter_;
    bool requested_ = false;
    bool showRawDump_ = false;
    int intervalMs_ = 1000;
};

}  // namespace em
