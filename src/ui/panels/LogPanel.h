#pragma once

#include <cstdint>
#include <string>

#include "core/Log.h"
#include <IconsFontAwesome6.h>

#include "ui/Panel.h"

namespace em {

// emeron's own log. Device logcat is a separate concern and belongs in its own
// panel; this one is about what the tool is doing.
class LogPanel final : public Panel {
public:
    static constexpr const char* kId = "Log";

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_SCROLL; }
    void draw(AppContext& context) override;

private:
    std::string filter_;
    int minLevel_ = static_cast<int>(LogLevel::Debug);
    bool autoScroll_ = true;
    std::uint64_t lastRevision_ = 0;
    std::vector<LogEntry> cache_;
};

}  // namespace em
