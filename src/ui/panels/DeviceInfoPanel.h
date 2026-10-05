#pragma once

#include <string>

#include <IconsFontAwesome6.h>

#include "ui/Panel.h"

namespace em {

class DeviceInfoPanel final : public Panel {
public:
    static constexpr const char* kId = "Device Info";

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_CIRCLE_INFO; }
    void draw(AppContext& context) override;
    void onDeviceChanged(AppContext& context) override;

private:
    std::string propertyFilter_;
    bool requested_ = false;
    bool showRawProperties_ = false;
};

}  // namespace em
