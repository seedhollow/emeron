#include "TestHarness.h"

#include <cstddef>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <IconsFontAwesome6.h>

// stb_truetype is compiled privately inside imgui_draw.cpp, so the test builds
// its own static copy to read the embedded font directly.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>

namespace em::embedded {
extern const unsigned char kFontAwesomeSolid[];
extern const std::size_t kFontAwesomeSolidSize;
}  // namespace em::embedded

namespace {

struct IconDefinition {
    std::string name;
    unsigned long codepoint = 0;
};

std::vector<IconDefinition> readIconHeader() {
    std::ifstream file{EMERON_FA_HEADER_PATH};
    std::stringstream text;
    text << file.rdbuf();

    std::vector<IconDefinition> icons;
    const std::regex pattern{R"(#define (ICON_FA_\w+) "[^"]*"\s*// U\+([0-9a-fA-F]+))"};
    const std::string content = text.str();
    for (auto it = std::sregex_iterator(content.begin(), content.end(), pattern);
         it != std::sregex_iterator(); ++it) {
        icons.push_back({(*it)[1].str(), std::stoul((*it)[2].str(), nullptr, 16)});
    }
    return icons;
}

}  // namespace

TEST_CASE("every ICON_FA_* in the header has a glyph in the embedded font") {
    // The failure this guards against compiles cleanly and renders boxes: a
    // header from one Font Awesome release beside a font from another. The
    // xo1337/FontAwesome repo pairs a 5 Pro header with a 4.7 font, and only
    // 24% of its icons render.
    stbtt_fontinfo font{};
    const int ok = stbtt_InitFont(&font, em::embedded::kFontAwesomeSolid,
                                  stbtt_GetFontOffsetForIndex(em::embedded::kFontAwesomeSolid, 0));
    CHECK(ok != 0);
    if (ok == 0) return;

    const auto icons = readIconHeader();
    CHECK(icons.size() > 1000);  // the header was found and parsed

    std::size_t missing = 0;
    for (const auto& icon : icons) {
        if (stbtt_FindGlyphIndex(&font, static_cast<int>(icon.codepoint)) == 0) {
            if (++missing <= 5) {
                ::test::reportFailure(__FILE__, __LINE__, icon.name + " has no glyph");
            }
        }
    }
    CHECK_EQ(missing, std::size_t{0});
}

TEST_CASE("only the ASCII-character icons fall outside the loaded glyph range") {
    // Fonts.cpp loads the icon font for [ICON_MIN_FA, ICON_MAX_16_FA] only.
    // Font Awesome 6 also maps 47 icons onto plain ASCII (ICON_FA_PLUS is "+",
    // ICON_FA_A is "A"). A merged font has one glyph per codepoint, and letting
    // Font Awesome own ASCII would turn every digit and capital in the UI into
    // an icon -- so those macros deliberately draw the text font's character.
    // Anything else outside the range would render as nothing; that must fail.
    std::size_t outside = 0;
    for (const auto& icon : readIconHeader()) {
        if (icon.codepoint >= ICON_MIN_FA && icon.codepoint <= ICON_MAX_16_FA) continue;
        ++outside;
        if (icon.codepoint < 0x20 || icon.codepoint > 0x7E) {
            ::test::reportFailure(__FILE__, __LINE__,
                                  icon.name + " is outside the loaded range and not ASCII");
        }
    }
    CHECK_EQ(outside, std::size_t{47});
}

TEST_CASE("the embedded font is the Font Awesome 6 Free Solid face") {
    CHECK(em::embedded::kFontAwesomeSolidSize > 100'000);
    // TrueType files start with the sfnt version 0x00010000.
    CHECK(em::embedded::kFontAwesomeSolid[0] == 0x00);
    CHECK(em::embedded::kFontAwesomeSolid[1] == 0x01);
}

#include "ui/FileIcons.h"

TEST_CASE("fileIcon picks by kind before extension") {
    using em::FileKind;
    // A folder called "photos.jpg" is still a folder.
    CHECK_EQ(std::string{em::fileIcon(FileKind::Directory, "photos.jpg")}, std::string{ICON_FA_FOLDER});
    CHECK_EQ(std::string{em::fileIcon(FileKind::Symlink, "sdcard")}, std::string{ICON_FA_LINK});
    CHECK_EQ(std::string{em::fileIcon(FileKind::Character, "null")}, std::string{ICON_FA_MICROCHIP});
    CHECK_EQ(std::string{em::fileIcon(FileKind::Socket, "adbd")}, std::string{ICON_FA_PLUG});
}

TEST_CASE("fileIcon matches extensions case-insensitively") {
    using em::FileKind;
    CHECK_EQ(std::string{em::fileIcon(FileKind::Regular, "IMG_0001.JPG")}, std::string{ICON_FA_FILE_IMAGE});
    CHECK_EQ(std::string{em::fileIcon(FileKind::Regular, "screen.mp4")}, std::string{ICON_FA_FILE_VIDEO});
    CHECK_EQ(std::string{em::fileIcon(FileKind::Regular, "base.apk")}, std::string{ICON_FA_FILE_ZIPPER});
    CHECK_EQ(std::string{em::fileIcon(FileKind::Regular, "libnative.so")}, std::string{ICON_FA_GEARS});
    CHECK_EQ(std::string{em::fileIcon(FileKind::Regular, "app.db-wal")}, std::string{ICON_FA_DATABASE});
    CHECK_EQ(std::string{em::fileIcon(FileKind::Regular, "run.perfetto-trace")},
             std::string{ICON_FA_WAVE_SQUARE});
}

TEST_CASE("fileIcon falls back to a plain file") {
    using em::FileKind;
    CHECK_EQ(std::string{em::fileIcon(FileKind::Regular, "Makefile")}, std::string{ICON_FA_FILE});
    // A leading dot is a hidden-file marker, not an extension.
    CHECK_EQ(std::string{em::fileIcon(FileKind::Regular, ".bashrc")}, std::string{ICON_FA_FILE});
    CHECK_EQ(std::string{em::fileIcon(FileKind::Regular, "trailing.")}, std::string{ICON_FA_FILE});
    CHECK_EQ(std::string{em::fileIcon(FileKind::Regular, "archive.unknownext")}, std::string{ICON_FA_FILE});
}
