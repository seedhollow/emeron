#pragma once

#include <string>
#include <string_view>

#include "app/AppContext.h"

namespace em {

// A dockable window.
//
// `id()` is the ImGui window name and must stay stable across releases: it is
// the key imgui.ini uses to restore the user's layout.
class Panel {
public:
    Panel() = default;
    virtual ~Panel() = default;

    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;
    Panel(Panel&&) = delete;
    Panel& operator=(Panel&&) = delete;

    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    [[nodiscard]] virtual std::string_view menuLabel() const noexcept { return id(); }

    // Font Awesome glyph shown in the window's tab and in the View menu.
    [[nodiscard]] virtual const char* icon() const noexcept { return ""; }

    // What to pass to ImGui::Begin: "<icon>  <id>###<id>". ImGui hashes only
    // what follows "###" and skips the marker itself, so this title has exactly
    // the identity of the plain "<id>" -- the icon is display-only, and layouts
    // saved in imgui.ini before icons existed still match. Use windowKey()
    // wherever a window is looked up by name. (tests/test_icons.cpp)
    [[nodiscard]] const char* windowTitle() {
        if (title_.empty()) title_ = std::string{icon()} + "  " + std::string{id()} + windowKey(id());
        return title_.c_str();
    }

    [[nodiscard]] static std::string windowKey(std::string_view panelId) {
        return "###" + std::string{panelId};
    }

    // Called once per frame while visible. Implementations call ImGui::Begin
    // themselves so each panel controls its own window flags.
    virtual void draw(AppContext& context) = 0;

    // The selected device changed; drop any cached per-device state.
    virtual void onDeviceChanged(AppContext& context) { (void)context; }

    // Called when the app is shutting down, before the collectors stop.
    virtual void onShutdown(AppContext& context) { (void)context; }

    [[nodiscard]] bool visible() const noexcept { return visible_; }
    void setVisible(bool value) noexcept { visible_ = value; }
    [[nodiscard]] bool* visibleFlag() noexcept { return &visible_; }

private:
    bool visible_ = true;
    std::string title_;
};

}  // namespace em
