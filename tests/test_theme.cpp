#include "TestHarness.h"

#include <cmath>
#include <string>

#include <imgui.h>
#include <implot.h>

#include "app/Theme.h"

using namespace em;

namespace {

// ImGui and ImPlot contexts work without a window as long as NewFrame is never
// called, which is all the style tables need.
struct GuiContext {
    GuiContext() {
        ImGui::CreateContext();
        ImPlot::CreateContext();
    }
    ~GuiContext() {
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }
    GuiContext(const GuiContext&) = delete;
    GuiContext& operator=(const GuiContext&) = delete;
};

// WCAG relative luminance and contrast ratio.
double channelLuminance(double value) {
    return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

double luminance(const ImVec4& color) {
    return 0.2126 * channelLuminance(static_cast<double>(color.x)) +
           0.7152 * channelLuminance(static_cast<double>(color.y)) +
           0.0722 * channelLuminance(static_cast<double>(color.z));
}

double contrastRatio(const ImVec4& a, const ImVec4& b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    const double hi = la > lb ? la : lb;
    const double lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

bool sameColor(const ImVec4& a, const ImVec4& b) {
    constexpr float kEpsilon = 1e-6F;
    return std::abs(a.x - b.x) < kEpsilon && std::abs(a.y - b.y) < kEpsilon &&
           std::abs(a.z - b.z) < kEpsilon && std::abs(a.w - b.w) < kEpsilon;
}

}  // namespace

TEST_CASE("theme mode names round-trip") {
    CHECK_EQ(std::string{theme::toString(theme::Mode::Dark)}, std::string{"dark"});
    CHECK_EQ(std::string{theme::toString(theme::Mode::Light)}, std::string{"light"});

    CHECK(theme::parseMode("dark") == theme::Mode::Dark);
    CHECK(theme::parseMode("light") == theme::Mode::Light);

    // An unknown value must not silently become a valid mode -- the caller
    // decides the fallback.
    CHECK(!theme::parseMode("solarized").has_value());
    CHECK(!theme::parseMode("").has_value());
    CHECK(!theme::parseMode("Dark").has_value());
}

TEST_CASE("other() flips the mode") {
    CHECK(theme::other(theme::Mode::Dark) == theme::Mode::Light);
    CHECK(theme::other(theme::Mode::Light) == theme::Mode::Dark);
}

TEST_CASE("apply sets the active mode and palette") {
    const GuiContext gui;

    theme::apply(theme::Mode::Light);
    CHECK(theme::mode() == theme::Mode::Light);
    const theme::Palette light = theme::palette();

    theme::apply(theme::Mode::Dark);
    CHECK(theme::mode() == theme::Mode::Dark);
    const theme::Palette dark = theme::palette();

    // The two modes are distinct palettes, not one tinted.
    CHECK(!sameColor(light.good, dark.good));
    CHECK(!sameColor(light.warning, dark.warning));
    CHECK(!sameColor(light.muted, dark.muted));
    CHECK(!sameColor(light.windowClear, dark.windowClear));
}

TEST_CASE("light mode is actually light and dark is actually dark") {
    const GuiContext gui;

    theme::apply(theme::Mode::Dark);
    const double darkBg = luminance(ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
    const double darkClear = luminance(theme::palette().windowClear);

    theme::apply(theme::Mode::Light);
    const double lightBg = luminance(ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
    const double lightClear = luminance(theme::palette().windowClear);

    CHECK(darkBg < 0.1);
    CHECK(lightBg > 0.5);
    // The backbuffer clear must track the theme, or the frame behind the
    // dockspace stays the wrong colour after a switch.
    CHECK(darkClear < 0.1);
    CHECK(lightClear > 0.5);
}

TEST_CASE("every semantic colour is readable against its own background") {
    // These are used as coloured *text* in tables and status lines, so the bar
    // is WCAG AA for normal text (4.5:1), not the 3:1 for UI components. A
    // pastel that works on near-black is invisible on near-white, which is the
    // failure this guards against.
    const GuiContext gui;
    constexpr double kMinContrast = 4.5;

    for (const auto mode : {theme::Mode::Dark, theme::Mode::Light}) {
        theme::apply(mode);
        const theme::Palette& palette = theme::palette();
        const ImVec4 background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];

        const struct {
            const char* name;
            ImVec4 color;
        } entries[] = {
            {"text", ImGui::GetStyle().Colors[ImGuiCol_Text]},
            {"good", palette.good},
            {"warning", palette.warning},
            {"bad", palette.bad},
            {"critical", palette.critical},
            {"muted", palette.muted},
            {"accent", palette.accent},
        };

        for (const auto& entry : entries) {
            const double ratio = contrastRatio(entry.color, background);
            if (ratio < kMinContrast) {
                ::test::reportFailure(__FILE__, __LINE__,
                                      std::string{theme::toString(mode)} + " '" + entry.name +
                                          "' contrast " + std::to_string(ratio) +
                                          " is below " + std::to_string(kMinContrast));
            }
        }
    }
}

TEST_CASE("apply retunes the ImGui and ImPlot colour tables together") {
    const GuiContext gui;

    theme::apply(theme::Mode::Dark);
    const ImVec4 darkWindow = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const ImVec4 darkPlot = ImPlot::GetStyle().Colors[ImPlotCol_PlotBg];
    const ImVec4 darkFrame = ImPlot::GetStyle().Colors[ImPlotCol_FrameBg];

    theme::apply(theme::Mode::Light);
    const ImVec4 lightWindow = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const ImVec4 lightPlot = ImPlot::GetStyle().Colors[ImPlotCol_PlotBg];
    const ImVec4 lightFrame = ImPlot::GetStyle().Colors[ImPlotCol_FrameBg];

    CHECK(!sameColor(darkWindow, lightWindow));
    // A plot left on a dark background inside a light window is the most
    // visible way for a theme switch to be half-applied.
    CHECK(!sameColor(darkPlot, lightPlot));
    CHECK(!sameColor(darkFrame, lightFrame));
    CHECK(luminance(darkPlot) < 0.1);
    CHECK(luminance(lightPlot) > 0.5);
}

TEST_CASE("switching modes does not move the layout") {
    // Only colours change: if geometry differed, every panel would reflow on a
    // theme switch.
    const GuiContext gui;

    theme::apply(theme::Mode::Dark);
    const ImGuiStyle dark = ImGui::GetStyle();

    theme::apply(theme::Mode::Light);
    const ImGuiStyle light = ImGui::GetStyle();

    CHECK_NEAR(dark.FrameRounding, light.FrameRounding, 1e-6);
    CHECK_NEAR(dark.WindowPadding.x, light.WindowPadding.x, 1e-6);
    CHECK_NEAR(dark.WindowPadding.y, light.WindowPadding.y, 1e-6);
    CHECK_NEAR(dark.ItemSpacing.x, light.ItemSpacing.x, 1e-6);
    CHECK_NEAR(dark.CellPadding.y, light.CellPadding.y, 1e-6);
    CHECK_NEAR(dark.ScrollbarSize, light.ScrollbarSize, 1e-6);
    CHECK_NEAR(dark.TabRounding, light.TabRounding, 1e-6);
}

TEST_CASE("apply wires the palette accent into the ImGui widget colours") {
    const GuiContext gui;

    for (const auto mode : {theme::Mode::Dark, theme::Mode::Light}) {
        theme::apply(mode);
        const theme::Palette& palette = theme::palette();
        const ImVec4* colors = ImGui::GetStyle().Colors;

        CHECK(sameColor(colors[ImGuiCol_CheckMark], palette.accent));
        CHECK(sameColor(colors[ImGuiCol_SliderGrab], palette.accent));
        CHECK(sameColor(colors[ImGuiCol_TextDisabled], palette.muted));
    }
}

TEST_CASE("budgetColor steps through the palette at the right thresholds") {
    const GuiContext gui;
    theme::apply(theme::Mode::Dark);
    const theme::Palette& palette = theme::palette();

    CHECK(sameColor(theme::budgetColor(0.0, 100.0), palette.good));
    CHECK(sameColor(theme::budgetColor(75.0, 100.0), palette.good));
    CHECK(sameColor(theme::budgetColor(75.1, 100.0), palette.warning));
    CHECK(sameColor(theme::budgetColor(100.0, 100.0), palette.warning));
    CHECK(sameColor(theme::budgetColor(100.1, 100.0), palette.bad));
    CHECK(sameColor(theme::budgetColor(200.0, 100.0), palette.bad));
    CHECK(sameColor(theme::budgetColor(200.1, 100.0), palette.critical));

    // A zero or negative budget means "no budget known", not "infinitely over".
    CHECK(sameColor(theme::budgetColor(50.0, 0.0), palette.muted));
    CHECK(sameColor(theme::budgetColor(50.0, -1.0), palette.muted));
}

TEST_CASE("frameTimeColor steps at multiples of the refresh period") {
    const GuiContext gui;
    theme::apply(theme::Mode::Dark);
    const theme::Palette& palette = theme::palette();

    constexpr double kPeriod = 16.667;
    CHECK(sameColor(theme::frameTimeColor(8.0, kPeriod), palette.good));
    CHECK(sameColor(theme::frameTimeColor(16.6, kPeriod), palette.good));
    CHECK(sameColor(theme::frameTimeColor(20.0, kPeriod), palette.warning));
    CHECK(sameColor(theme::frameTimeColor(40.0, kPeriod), palette.bad));
    CHECK(sameColor(theme::frameTimeColor(200.0, kPeriod), palette.critical));
    CHECK(sameColor(theme::frameTimeColor(8.0, 0.0), palette.muted));
}

TEST_CASE("budgetColor follows the active mode") {
    const GuiContext gui;

    theme::apply(theme::Mode::Dark);
    const ImVec4 darkGood = theme::budgetColor(0.0, 100.0);

    theme::apply(theme::Mode::Light);
    const ImVec4 lightGood = theme::budgetColor(0.0, 100.0);

    CHECK(!sameColor(darkGood, lightGood));
    CHECK(sameColor(lightGood, theme::palette().good));
}
