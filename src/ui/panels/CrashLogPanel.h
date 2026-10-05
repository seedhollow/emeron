#pragma once

#include <string>
#include <vector>

#include "core/CrashHandler.h"
#include <IconsFontAwesome6.h>

#include "ui/Panel.h"

namespace em {

// Browses whatever emeron's own crash handler left behind in
// <workspace>/crash-logs -- a report per segfault/abort/etc., each with a
// backtrace and the last thing the UI was doing. Unrelated to device logcat
// or device crashes: this is for bugs in emeron itself.
class CrashLogPanel final : public Panel {
public:
    static constexpr const char* kId = "Crash Logs";

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_BOMB; }
    void draw(AppContext& context) override;

private:
    void refresh();

    std::vector<crash::CrashReport> reports_;
    int selected_ = -1;
    std::string selectedText_;
    bool loaded_ = false;
    bool confirmClearAll_ = false;
};

}  // namespace em
