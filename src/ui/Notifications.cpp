#include "ui/Notifications.h"

#include <atomic>
#include <mutex>
#include <vector>

// Inside the main window, above the status bar; long enough to read a path.
#define NOTIFY_RENDER_OUTSIDE_MAIN_WINDOW false
#define NOTIFY_PADDING_Y 44.f
#define NOTIFY_DEFAULT_DISMISS 4000
#define NOTIFY_OPACITY 0.95f
#include <ImGuiNotify.hpp>

#include "app/Theme.h"

namespace em::notify {
namespace {

struct Pending {
    Kind kind;
    std::string title;
    std::string text;
    std::string buttonLabel;
    std::function<void()> onPress;
};

std::mutex gMutex;
std::vector<Pending> gPending;
std::atomic<bool> gEnabled{true};

ImGuiToastType toType(Kind kind) {
    switch (kind) {
        case Kind::Success: return ImGuiToastType::Success;
        case Kind::Warning: return ImGuiToastType::Warning;
        case Kind::Error: return ImGuiToastType::Error;
        case Kind::Info: break;
    }
    return ImGuiToastType::Info;
}

// The theme's semantic colours, which are contrast-checked in both modes
// (tests/test_theme.cpp), instead of ImGuiNotify's built-in ones.
ImVec4 paletteColor(ImGuiToastType type) {
    const auto& p = theme::palette();
    switch (type) {
        case ImGuiToastType::Success: return p.good;
        case ImGuiToastType::Warning: return p.warning;
        case ImGuiToastType::Error: return p.bad;
        case ImGuiToastType::Info: return p.accent;
        default: break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

}  // namespace

void post(Kind kind, std::string title, std::string text) {
    postWithAction(kind, std::move(title), std::move(text), {}, nullptr);
}

void postWithAction(Kind kind, std::string title, std::string text, std::string buttonLabel,
                    std::function<void()> onPress) {
    if (!gEnabled.load()) return;
    const std::lock_guard lock{gMutex};
    gPending.push_back({kind, std::move(title), std::move(text), std::move(buttonLabel), std::move(onPress)});
}

void setEnabled(bool enabled) {
    gEnabled.store(enabled);
    if (!enabled) {
        const std::lock_guard lock{gMutex};
        gPending.clear();
    }
}

bool enabled() { return gEnabled.load(); }

void render() {
    ImGuiToast::colorOverride = &paletteColor;

    std::vector<Pending> pending;
    {
        const std::lock_guard lock{gMutex};
        pending.swap(gPending);
    }
    for (auto& p : pending) {
        // Errors stay until read: they are the ones that matter if missed.
        const int dismissMs = p.kind == Kind::Error ? 10'000 : NOTIFY_DEFAULT_DISMISS;
        ImGuiToast toast{toType(p.kind), dismissMs};
        toast.setTitle("%s", p.title.c_str());
        if (!p.text.empty()) toast.setContent("%s", p.text.c_str());
        if (p.onPress) {
            toast.setButtonLabel("%s", p.buttonLabel.c_str());
            toast.setOnButtonPress(p.onPress);
        }
        ImGui::InsertNotification(toast);
    }
    if (!gEnabled.load()) ImGui::notifications.clear();

    // Toasts are drawn in the theme's popup colours, with rounded corners.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
    ImGui::RenderNotifications();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

}  // namespace em::notify
