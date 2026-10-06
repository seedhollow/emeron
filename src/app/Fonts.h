#pragma once

// UI fonts, all embedded in the binary:
//
//   * ui()   -- Inter, the interface text. The default font.
//   * bold() -- Inter SemiBold, for headings.
//   * mono() -- JetBrains Mono, for code, logs and anything column-aligned.
//
// Font Awesome 6 Free (Solid) is merged into each, so any string can mix text
// and icons:
//
//     #include <IconsFontAwesome6.h>
//     ImGui::Button(ICON_FA_DOWNLOAD " Download");
//
// Every ICON_FA_* in that header has a glyph in the embedded font -- a unit
// test holds the two to that, because a header from one Font Awesome release
// paired with a font from another compiles cleanly and draws empty boxes.
//
// The fonts are TrueType, rasterised by ImGui 1.92 at whatever size and
// display density they are drawn at, so they stay sharp on Retina screens.

struct ImFont;

namespace em::fonts {

// Call once after ImGui::CreateContext() and before the first NewFrame().
void loadUiFonts();

[[nodiscard]] ImFont* ui();
[[nodiscard]] ImFont* bold();
[[nodiscard]] ImFont* mono();

// Base size of each, in points. Push mono() with monoSize() so code lines
// keep their height.
[[nodiscard]] float uiSize();
[[nodiscard]] float monoSize();

}  // namespace em::fonts
