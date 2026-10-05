#pragma once

// UI fonts: ImGui's default text font with Font Awesome 6 Free (Solid) merged
// into it, so any string can mix text and icons:
//
//     #include <IconsFontAwesome6.h>
//     ImGui::Button(ICON_FA_DOWNLOAD " Download");
//
// Every ICON_FA_* in that header has a glyph in the embedded font -- a unit
// test holds the two to that, because a header from one Font Awesome release
// paired with a font from another compiles cleanly and draws empty boxes.

namespace em::fonts {

// Call once after ImGui::CreateContext() and before the first NewFrame().
void loadUiFonts();

}  // namespace em::fonts
