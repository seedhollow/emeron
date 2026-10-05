#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "collect/LogcatReader.h"
#include <IconsFontAwesome6.h>

#include "ui/Panel.h"

namespace em {

// Device logcat for the selected process.
//
// Filtering is host side and applied at draw time, so widening a filter
// reveals context already in the buffer instead of requiring a restart. The
// filtered index list is cached and only rebuilt when the buffer revision or
// the filter signature changes -- rebuilding 20k substring searches every
// frame would dominate the frame budget.
class LogcatPanel final : public Panel {
public:
    static constexpr const char* kId = "Logcat";

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_TERMINAL; }
    void draw(AppContext& context) override;
    void onDeviceChanged(AppContext& context) override;
    void onShutdown(AppContext& context) override;

private:
    void drawToolbar(AppContext& context);
    void drawOptionsPopup(AppContext& context);
    void drawTable(AppContext& context, const LogcatSnapshot& snapshot);
    void rebuildFilter(const LogcatSnapshot& snapshot);

    // The filter rule itself lives in LogcatReader.h; this is just the widget
    // state bound to it.
    LogcatFilter filter_;
    int minLevel_ = static_cast<int>(LogcatLevel::Verbose);

    // Device-side stream options mirrored for the widgets.
    int scope_ = static_cast<int>(LogcatScope::SelectedProcess);
    bool deviceSideFilter_ = true;
    int backlogLines_ = 400;
    int deviceMinLevel_ = static_cast<int>(LogcatLevel::Verbose);
    int bufferChoice_ = 0;
    int capacityLines_ = 20'000;

    bool autoScroll_ = true;
    bool wrapMessages_ = false;
    bool showTid_ = false;

    std::vector<std::uint32_t> filtered_;
    std::uint64_t cachedRevision_ = 0;
    std::string cachedSignature_;
    bool cacheValid_ = false;
    bool startedOnce_ = false;
};

}  // namespace em
