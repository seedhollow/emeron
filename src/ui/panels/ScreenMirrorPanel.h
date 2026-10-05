#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include "collect/ScreenMirror.h"
#include "ui/Panel.h"

namespace em {

// The device's screen, live, and a way to use it from the computer: click to
// tap, drag to swipe, hold to long-press, wheel to scroll, the system keys,
// typing, and screenshots.
class ScreenMirrorPanel final : public Panel {
public:
    static constexpr const char* kId = "Screen";

    [[nodiscard]] std::string_view id() const noexcept override { return kId; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_MOBILE_SCREEN; }
    void draw(AppContext& context) override;
    void onDeviceChanged(AppContext& context) override;
    void onShutdown(AppContext& context) override;

private:
    void drawToolbar(AppContext& context);
    void drawScreen(AppContext& context);
    void handlePointer(AppContext& context, ImVec2 imageMin, float scale);
    void handleTyping(AppContext& context);
    void uploadFrame();
    void saveScreenshot(AppContext& context);
    void releaseTexture();

    // What the user asked for; capture actually runs only while the panel is
    // on screen, so a hidden tab costs the phone nothing.
    bool wantRunning_ = false;

    unsigned int texture_ = 0;
    std::uint32_t textureWidth_ = 0;
    std::uint32_t textureHeight_ = 0;
    std::shared_ptr<const ScreenFrame> shown_;

    // Pointer gesture in progress, in device pixels.
    bool pressing_ = false;
    ImVec2 pressDevice_{};
    ImVec2 lastTouch_{};  // last position sent as a live touch
    ImVec2 pressScreen_{};
    double pressTime_ = 0.0;
    struct Ripple {
        ImVec2 device;
        double time;
    };
    std::vector<Ripple> ripples_;

    float wheelAccumulated_ = 0.0F;
    double lastWheelSend_ = 0.0;

    bool typeIntoDevice_ = false;
    bool pickHovering_ = false;  // Inspect mode: the pointer was over the screen
    std::string pendingTyping_;
    double lastTypedTime_ = 0.0;
    std::string textField_;

    std::string status_;
    bool statusOk_ = true;
    std::filesystem::path lastScreenshot_;
    std::string announcedNotice_;  // the engine notice already toasted
};

}  // namespace em
