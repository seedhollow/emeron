#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <imgui.h>

namespace em::theme {

enum class Mode : std::uint8_t { Dark, Light };

[[nodiscard]] std::string_view toString(Mode mode) noexcept;
[[nodiscard]] std::optional<Mode> parseMode(std::string_view text) noexcept;
[[nodiscard]] Mode other(Mode mode) noexcept;

// Semantic colours. Panels use these rather than RGBA literals, which is what
// lets a theme switch reach the whole UI from one place.
//
// The two modes are not tints of each other: a pastel green that reads well on
// a near-black panel has far too little contrast on near-white, so the light
// variants are darker and more saturated rather than lighter.
struct Palette {
    ImVec4 good;      // within budget
    ImVec4 warning;   // approaching a limit
    ImVec4 bad;       // over budget / janky
    ImVec4 critical;  // severe (frozen frames, device errors)
    ImVec4 muted;     // secondary text
    ImVec4 accent;

    // Backbuffer clear for the host viewport, so the frame behind the dockspace
    // matches the theme.
    ImVec4 windowClear;
};

// Applies the ImGui and ImPlot styles for `mode` and makes its palette active.
// Safe to call mid-frame: it writes the style tables directly and pushes
// nothing, so a switch from a menu takes effect immediately.
void apply(Mode mode);

[[nodiscard]] Mode mode() noexcept;
[[nodiscard]] const Palette& palette() noexcept;

// Colour for a value against a budget: good below, amber near, red over.
[[nodiscard]] ImVec4 budgetColor(double value, double budget) noexcept;

// Frame-time colour keyed to multiples of the refresh period.
[[nodiscard]] ImVec4 frameTimeColor(double frameMs, double refreshPeriodMs) noexcept;

}  // namespace em::theme
