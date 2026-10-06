#include "app/Theme.h"

#include <implot.h>

namespace em::theme {
namespace {

constexpr ImVec4 rgba(float r, float g, float b, float a = 1.0F) {
    return ImVec4{r / 255.0F, g / 255.0F, b / 255.0F, a};
}

const Palette kDarkPalette{
    /*good=*/rgba(94, 204, 128),
    /*warning=*/rgba(230, 184, 80),
    /*bad=*/rgba(226, 110, 92),
    /*critical=*/rgba(242, 92, 122),
    /*muted=*/rgba(150, 156, 168),
    /*accent=*/rgba(94, 160, 230),
    /*windowClear=*/rgba(16, 17, 21),
};

// Darker and more saturated than their dark-mode counterparts so they keep
// roughly the same contrast ratio against a near-white background.
const Palette kLightPalette{
    /*good=*/rgba(22, 128, 62),
    /*warning=*/rgba(140, 92, 0),
    /*bad=*/rgba(190, 48, 38),
    /*critical=*/rgba(166, 16, 56),
    /*muted=*/rgba(108, 114, 126),
    /*accent=*/rgba(26, 98, 188),
    /*windowClear=*/rgba(226, 229, 234),
};

Mode gMode = Mode::Dark;

// Geometry is identical in both modes; only the colour tables differ.
void applyShape(ImGuiStyle& style) {
    // Rounding: small and consistent, like a desktop IDE -- enough to soften
    // the edges, not so much that controls look like a phone app.
    style.WindowRounding = 6.0F;
    style.ChildRounding = 4.0F;
    style.FrameRounding = 4.0F;
    style.PopupRounding = 6.0F;
    style.ScrollbarRounding = 8.0F;
    style.GrabRounding = 4.0F;
    style.TabRounding = 5.0F;

    // Spacing: controls tall enough to hit (about 26 px with Inter 14), and
    // room between groups so dense panels still read as rows and sections.
    style.WindowPadding = ImVec2{12.0F, 10.0F};
    style.FramePadding = ImVec2{9.0F, 5.0F};
    style.ItemSpacing = ImVec2{8.0F, 7.0F};
    style.ItemInnerSpacing = ImVec2{6.0F, 5.0F};
    style.CellPadding = ImVec2{8.0F, 4.0F};
    style.TouchExtraPadding = ImVec2{0.0F, 0.0F};
    style.IndentSpacing = 18.0F;
    style.ScrollbarSize = 11.0F;
    style.GrabMinSize = 10.0F;

    // Borders: hairlines between areas, none on controls.
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.TabBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F;
    // The selected tab of the focused tab bar gets an accent line on top, the
    // way VS Code and Android Studio mark where the keyboard is.
    style.TabBarOverlineSize = 2.0F;
    style.DockingSeparatorSize = 2.0F;
    style.SeparatorTextBorderSize = 1.0F;
    style.SeparatorTextPadding = ImVec2{0.0F, 4.0F};

    style.WindowTitleAlign = ImVec2{0.0F, 0.5F};
    // No collapse arrow in title and dock tab bars: nothing in emeron is
    // meant to be collapsed, and it was the first thing in every panel.
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SelectableTextAlign = ImVec2{0.0F, 0.5F};
    style.TabCloseButtonMinWidthUnselected = 0.0F;  // close buttons on hover/selected only
}

void applyDarkColors(ImGuiStyle& style) {
    ImGui::StyleColorsDark();
    ImVec4* colors = style.Colors;

    // Three levels: chrome (title, menu, tab strips) darkest, panels in the
    // middle, controls lightest. A selected tab is the panel's own colour, so
    // it reads as the front of that panel.
    const ImVec4 chrome = rgba(19, 21, 25);
    const ImVec4 panel = rgba(25, 27, 32);
    colors[ImGuiCol_Text] = rgba(222, 225, 231);
    colors[ImGuiCol_WindowBg] = panel;
    colors[ImGuiCol_ChildBg] = rgba(0, 0, 0, 0.0F);
    colors[ImGuiCol_PopupBg] = rgba(31, 34, 41, 0.99F);
    colors[ImGuiCol_Border] = rgba(43, 47, 56);
    colors[ImGuiCol_BorderShadow] = rgba(0, 0, 0, 0.0F);
    colors[ImGuiCol_FrameBg] = rgba(36, 39, 47);
    colors[ImGuiCol_FrameBgHovered] = rgba(44, 48, 58);
    colors[ImGuiCol_FrameBgActive] = rgba(51, 56, 68);
    colors[ImGuiCol_TitleBg] = chrome;
    colors[ImGuiCol_TitleBgActive] = chrome;
    colors[ImGuiCol_TitleBgCollapsed] = chrome;
    colors[ImGuiCol_MenuBarBg] = chrome;
    colors[ImGuiCol_ScrollbarBg] = rgba(0, 0, 0, 0.0F);
    colors[ImGuiCol_ScrollbarGrab] = rgba(58, 63, 75);
    colors[ImGuiCol_ScrollbarGrabHovered] = rgba(74, 80, 95);
    colors[ImGuiCol_ScrollbarGrabActive] = rgba(88, 95, 112);
    colors[ImGuiCol_Button] = rgba(40, 44, 53);
    colors[ImGuiCol_ButtonHovered] = rgba(51, 56, 68);
    colors[ImGuiCol_ButtonActive] = rgba(61, 67, 82);
    colors[ImGuiCol_Separator] = rgba(43, 47, 56);
    colors[ImGuiCol_SeparatorHovered] = rgba(70, 76, 90);
    colors[ImGuiCol_ResizeGrip] = rgba(0, 0, 0, 0.0F);
    colors[ImGuiCol_Tab] = chrome;
    colors[ImGuiCol_TabHovered] = rgba(38, 42, 51);
    colors[ImGuiCol_TabSelected] = panel;
    colors[ImGuiCol_TabDimmed] = chrome;
    colors[ImGuiCol_TabDimmedSelected] = panel;
    colors[ImGuiCol_DockingEmptyBg] = rgba(16, 17, 21);
    colors[ImGuiCol_TableHeaderBg] = rgba(31, 34, 41);
    colors[ImGuiCol_TableBorderStrong] = rgba(43, 47, 56);
    colors[ImGuiCol_TableBorderLight] = rgba(36, 39, 47);
    colors[ImGuiCol_TableRowBgAlt] = rgba(255, 255, 255, 0.022F);
    colors[ImGuiCol_ModalWindowDimBg] = rgba(0, 0, 0, 0.45F);
}

void applyLightColors(ImGuiStyle& style) {
    ImGui::StyleColorsLight();
    ImVec4* colors = style.Colors;

    // Off-white rather than pure white: a full-brightness background next to
    // dark text is harsh for the long sessions this tool is used in.
    colors[ImGuiCol_WindowBg] = rgba(247, 248, 250);
    colors[ImGuiCol_ChildBg] = rgba(0, 0, 0, 0.0F);
    colors[ImGuiCol_PopupBg] = rgba(255, 255, 255, 0.99F);
    colors[ImGuiCol_Border] = rgba(203, 208, 217);
    colors[ImGuiCol_FrameBg] = rgba(235, 238, 242);
    colors[ImGuiCol_FrameBgHovered] = rgba(224, 229, 236);
    colors[ImGuiCol_FrameBgActive] = rgba(211, 218, 228);
    colors[ImGuiCol_TitleBg] = rgba(233, 236, 241);
    colors[ImGuiCol_TitleBgActive] = rgba(233, 236, 241);
    colors[ImGuiCol_TitleBgCollapsed] = rgba(238, 240, 244);
    colors[ImGuiCol_MenuBarBg] = rgba(233, 236, 241);
    colors[ImGuiCol_ScrollbarBg] = rgba(0, 0, 0, 0.0F);
    colors[ImGuiCol_ScrollbarGrab] = rgba(197, 203, 212);
    colors[ImGuiCol_ScrollbarGrabHovered] = rgba(176, 184, 196);
    colors[ImGuiCol_ScrollbarGrabActive] = rgba(156, 165, 180);
    colors[ImGuiCol_Button] = rgba(228, 232, 238);
    colors[ImGuiCol_ButtonHovered] = rgba(214, 221, 232);
    colors[ImGuiCol_ButtonActive] = rgba(198, 207, 221);
    colors[ImGuiCol_Header] = rgba(223, 228, 236);
    colors[ImGuiCol_HeaderHovered] = rgba(208, 217, 230);
    colors[ImGuiCol_HeaderActive] = rgba(193, 204, 221);
    colors[ImGuiCol_Separator] = rgba(208, 213, 221);
    colors[ImGuiCol_Tab] = rgba(233, 236, 241);
    colors[ImGuiCol_TabHovered] = rgba(208, 217, 230);
    colors[ImGuiCol_TabSelected] = rgba(247, 248, 250);
    colors[ImGuiCol_TabDimmed] = rgba(233, 236, 241);
    colors[ImGuiCol_TabDimmedSelected] = rgba(247, 248, 250);
    colors[ImGuiCol_DockingEmptyBg] = rgba(226, 229, 234);
    colors[ImGuiCol_Text] = rgba(28, 32, 40);
    colors[ImGuiCol_TableHeaderBg] = rgba(231, 235, 241);
    colors[ImGuiCol_TableBorderStrong] = rgba(198, 204, 213);
    colors[ImGuiCol_TableBorderLight] = rgba(219, 224, 231);
    colors[ImGuiCol_TableRowBgAlt] = rgba(0, 0, 0, 0.028F);
}

void applyPlotStyle(Mode target) {
    if (target == Mode::Light) {
        ImPlot::StyleColorsLight();
    } else {
        ImPlot::StyleColorsDark();
    }

    ImPlotStyle& style = ImPlot::GetStyle();

    // ImPlot v1.0 moved LineWeight/MarkerSize/FillAlpha out of ImPlotStyle and
    // into per-item ImPlotSpec, so they are set at the call site instead.
    style.PlotPadding = ImVec2{8.0F, 6.0F};
    style.LabelPadding = ImVec2{4.0F, 3.0F};
    style.LegendPadding = ImVec2{6.0F, 6.0F};
    style.PlotDefaultSize = ImVec2{360.0F, 220.0F};
    style.PlotMinSize = ImVec2{180.0F, 110.0F};

    if (target == Mode::Light) {
        style.Colors[ImPlotCol_FrameBg] = rgba(247, 248, 250);
        style.Colors[ImPlotCol_PlotBg] = rgba(253, 253, 254);
        style.Colors[ImPlotCol_PlotBorder] = rgba(205, 210, 219);
        style.Colors[ImPlotCol_AxisGrid] = rgba(200, 206, 216, 0.75F);
    } else {
        style.Colors[ImPlotCol_FrameBg] = rgba(24, 26, 31);
        style.Colors[ImPlotCol_PlotBg] = rgba(20, 22, 27);
        style.Colors[ImPlotCol_PlotBorder] = rgba(48, 52, 62);
        style.Colors[ImPlotCol_AxisGrid] = rgba(52, 56, 66, 0.55F);
    }
}

}  // namespace

std::string_view toString(Mode value) noexcept {
    return value == Mode::Light ? "light" : "dark";
}

std::optional<Mode> parseMode(std::string_view text) noexcept {
    if (text == "dark") return Mode::Dark;
    if (text == "light") return Mode::Light;
    return std::nullopt;
}

Mode other(Mode value) noexcept {
    return value == Mode::Dark ? Mode::Light : Mode::Dark;
}

Mode mode() noexcept {
    return gMode;
}

const Palette& palette() noexcept {
    return gMode == Mode::Light ? kLightPalette : kDarkPalette;
}

void apply(Mode target) {
    gMode = target;

    ImGuiStyle& style = ImGui::GetStyle();
    if (target == Mode::Light) {
        applyLightColors(style);
    } else {
        applyDarkColors(style);
    }
    applyShape(style);

    const Palette& active = palette();
    style.Colors[ImGuiCol_TextDisabled] = active.muted;
    style.Colors[ImGuiCol_CheckMark] = active.accent;
    style.Colors[ImGuiCol_SliderGrab] = active.accent;
    style.Colors[ImGuiCol_SliderGrabActive] = active.accent;
    style.Colors[ImGuiCol_PlotHistogram] = active.accent;
    const auto accent = [&](float alpha) {
        return ImVec4{active.accent.x, active.accent.y, active.accent.z, alpha};
    };
    style.Colors[ImGuiCol_DockingPreview] = accent(0.42F);
    // Selection and focus in the accent, like every IDE: selected rows, the
    // focused tab's overline, text selection, keyboard navigation.
    const bool light = target == Mode::Light;
    style.Colors[ImGuiCol_Header] = accent(light ? 0.16F : 0.20F);
    style.Colors[ImGuiCol_HeaderHovered] =
        light ? ImVec4{0.0F, 0.0F, 0.0F, 0.05F} : ImVec4{1.0F, 1.0F, 1.0F, 0.06F};
    style.Colors[ImGuiCol_HeaderActive] = accent(light ? 0.24F : 0.30F);
    style.Colors[ImGuiCol_TabSelectedOverline] = active.accent;
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4{0.0F, 0.0F, 0.0F, 0.0F};
    style.Colors[ImGuiCol_TextSelectedBg] = accent(0.35F);
    style.Colors[ImGuiCol_NavCursor] = active.accent;
    style.Colors[ImGuiCol_SeparatorActive] = active.accent;
    style.Colors[ImGuiCol_ResizeGripHovered] = accent(0.5F);
    style.Colors[ImGuiCol_ResizeGripActive] = active.accent;
    // On a near-white panel a filled control alone barely shows; light
    // themes outline inputs, checkboxes and buttons instead.
    style.FrameBorderSize = light ? 1.0F : 0.0F;

    // Multi-viewport windows are real OS windows, so they must be opaque and
    // square -- a translucent rounded viewport shows the desktop through it.
    if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        style.WindowRounding = 0.0F;
        style.Colors[ImGuiCol_WindowBg].w = 1.0F;
    }

    applyPlotStyle(target);
}

ImVec4 budgetColor(double value, double budget) noexcept {
    const Palette& active = palette();
    if (budget <= 0.0) return active.muted;

    const double ratio = value / budget;
    if (ratio <= 0.75) return active.good;
    if (ratio <= 1.0) return active.warning;
    if (ratio <= 2.0) return active.bad;
    return active.critical;
}

ImVec4 frameTimeColor(double frameMs, double refreshPeriodMs) noexcept {
    const Palette& active = palette();
    if (refreshPeriodMs <= 0.0) return active.muted;

    const double ratio = frameMs / refreshPeriodMs;
    if (ratio <= 1.0) return active.good;
    if (ratio <= 2.0) return active.warning;
    if (ratio <= 4.0) return active.bad;
    return active.critical;
}

}  // namespace em::theme
