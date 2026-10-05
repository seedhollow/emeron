#include "app/Fonts.h"

#include <cstddef>

#include <IconsFontAwesome6.h>
#include <imgui.h>

namespace em::embedded {
// Generated at build time from vendor/fontawesome/fa-solid-900.ttf.
extern const unsigned char kFontAwesomeSolid[];
extern const std::size_t kFontAwesomeSolidSize;
}  // namespace em::embedded

namespace em::fonts {
namespace {

// ImGui's bitmap default font is designed for exactly this size.
constexpr float kTextSize = 13.0F;

// Font Awesome glyphs fill the full em box, so at the text size they look a
// size too large beside 13 px text; slightly smaller sits on the same visual
// line. Every icon then gets the same advance so icon columns line up.
constexpr float kIconSize = 12.0F;

// Only the icon block. Font Awesome 6 also maps 47 icons onto plain ASCII
// (ICON_FA_PLUS is "+", ICON_FA_A is "A"); a merged font has one glyph per
// codepoint, so those must come from the text font or every digit and capital
// in the UI would become an icon. Those macros therefore draw the ordinary
// character. tests/test_fonts.cpp holds the exception list to exactly that.
constexpr ImWchar kIconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};

}  // namespace

void loadUiFonts() {
    ImGuiIO& io = ImGui::GetIO();

    ImFontConfig text;
    text.SizePixels = kTextSize;
    io.Fonts->AddFontDefaultBitmap(&text);

    ImFontConfig icons;
    icons.MergeMode = true;
    icons.PixelSnapH = true;
    icons.GlyphMinAdvanceX = kTextSize;
    // The array is static data in the binary. Since 1.92 the atlas would
    // otherwise take ownership and try to free it.
    icons.FontDataOwnedByAtlas = false;

    io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(embedded::kFontAwesomeSolid),
        static_cast<int>(embedded::kFontAwesomeSolidSize), kIconSize, &icons, kIconRanges);
}

}  // namespace em::fonts
