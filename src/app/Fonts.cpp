#include "app/Fonts.h"

#include <cstddef>
#include <cstdio>

#include <IconsFontAwesome6.h>
#include <imgui.h>

namespace em::embedded {
// Generated at build time from vendor/fontawesome and vendor/fonts.
extern const unsigned char kFontAwesomeSolid[];
extern const std::size_t kFontAwesomeSolidSize;
extern const unsigned char kInterRegular[];
extern const std::size_t kInterRegularSize;
extern const unsigned char kInterSemiBold[];
extern const std::size_t kInterSemiBoldSize;
extern const unsigned char kJetBrainsMonoRegular[];
extern const std::size_t kJetBrainsMonoRegularSize;
}  // namespace em::embedded

namespace em::fonts {
namespace {

// Inter is drawn smaller per point than ImGui's old bitmap font; 14 reads like
// the body text of a desktop IDE.
constexpr float kTextSize = 14.0F;
constexpr float kMonoSize = 13.0F;

// Font Awesome glyphs fill the whole em box, so at the text size they look a
// size too large beside the text; a little smaller sits on the same visual
// line. Every icon gets the same advance so icon columns line up.
constexpr float kIconScale = 0.86F;

// Only the icon block. Font Awesome 6 also maps 47 icons onto plain ASCII
// (ICON_FA_PLUS is "+", ICON_FA_A is "A"); a merged font has one glyph per
// codepoint, so those must come from the text font or every digit and capital
// in the UI would become an icon. Those macros therefore draw the ordinary
// character. tests/test_fonts.cpp holds the exception list to exactly that.
constexpr ImWchar kIconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};

ImFont* gUi = nullptr;
ImFont* gBold = nullptr;
ImFont* gMono = nullptr;

ImFont* addFont(const unsigned char* data, std::size_t size, float pixels, const char* name) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig text;
    text.SizePixels = pixels;
    // Static data in the binary: since 1.92 the atlas would otherwise try to
    // free it.
    text.FontDataOwnedByAtlas = false;
    std::snprintf(text.Name, sizeof text.Name, "%s", name);
    ImFont* font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data),
                                                  static_cast<int>(size), pixels, &text);

    ImFontConfig icons;
    icons.MergeMode = true;
    icons.PixelSnapH = true;
    icons.GlyphMinAdvanceX = pixels;
    // Centre the icons on the text's x-height rather than its baseline.
    icons.GlyphOffset = ImVec2{0.0F, pixels * 0.04F};
    icons.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(embedded::kFontAwesomeSolid),
                                   static_cast<int>(embedded::kFontAwesomeSolidSize),
                                   pixels * kIconScale, &icons, kIconRanges);
    return font;
}

}  // namespace

void loadUiFonts() {
    // The first font added is ImGui's default.
    gUi = addFont(embedded::kInterRegular, embedded::kInterRegularSize, kTextSize, "Inter");
    gBold = addFont(embedded::kInterSemiBold, embedded::kInterSemiBoldSize, kTextSize,
                    "Inter SemiBold");
    gMono = addFont(embedded::kJetBrainsMonoRegular, embedded::kJetBrainsMonoRegularSize,
                    kMonoSize, "JetBrains Mono");
}

ImFont* ui() {
    return gUi;
}
ImFont* bold() {
    return gBold != nullptr ? gBold : gUi;
}
ImFont* mono() {
    return gMono != nullptr ? gMono : gUi;
}
float uiSize() {
    return kTextSize;
}
float monoSize() {
    return kMonoSize;
}

}  // namespace em::fonts
