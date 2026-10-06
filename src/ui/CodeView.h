#pragma once

// A read-only code viewer: syntax colours (Java, smali, XML), line numbers,
// selection and copy, find (Ctrl+F), occurrences of the word under the caret,
// and -- given jadx's code annotations -- go to declaration and find usages.
//
// Only the visible lines are drawn, so a 50 000-line class costs the same per
// frame as a 50-line one. Positions are byte offsets into the UTF-8 text, the
// same unit jadx's annotations arrive in.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "collect/JadxSession.h"

namespace em {

class CodeView {
public:
    enum class Language : std::uint8_t { Java, Smali, Xml, Plain };

    void setText(std::string text, Language language, std::vector<CodeRef> refs = {});
    void clear() { setText({}, Language::Plain); }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] Language language() const noexcept { return language_; }
    [[nodiscard]] const std::vector<CodeRef>& refs() const noexcept { return refs_; }

    struct Action {
        enum class Kind : std::uint8_t { None, GoTo, FindUsages };
        Kind kind = Kind::None;
        std::uint32_t pos = 0;  // start of the identifier
    };
    // Fills the rest of the current window. Returns what the user asked for.
    Action draw(const char* id);

    // Puts the caret at `pos`, scrolls it into view and flashes its line.
    void scrollTo(std::uint32_t pos);
    void openFind();

    [[nodiscard]] std::uint32_t caret() const noexcept { return caret_; }
    [[nodiscard]] int lineOf(std::uint32_t pos) const;
    [[nodiscard]] int lineCount() const noexcept {
        return static_cast<int>(lineStarts_.size());
    }
    // The annotated identifier containing `pos`, if jadx annotated one.
    [[nodiscard]] const CodeRef* refAt(std::uint32_t pos) const;
    // [start, end) of the identifier containing `pos`; empty if none.
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> wordAt(std::uint32_t pos) const;

private:
    enum Color : std::uint8_t {
        Default,
        Keyword,
        String,
        Number,
        Comment,
        Annotation,
        Type,
        Method,
        Field,
        Variable,
        Tag,
        Attribute,
        ColorCount
    };
    struct Span {
        std::uint32_t start;
        std::uint32_t end;
        Color color;
    };

    void highlight();
    void highlightJava();
    void highlightSmali();
    void highlightXml();
    void addSpan(std::uint32_t start, std::uint32_t end, Color color);
    [[nodiscard]] Color refColor(std::uint32_t pos) const;
    void findMatches();
    void selectMatch(int index);
    [[nodiscard]] std::uint32_t lineEnd(int line) const;
    [[nodiscard]] float xOf(int line, std::uint32_t pos) const;
    [[nodiscard]] std::uint32_t posAt(int line, float x) const;
    void copySelection() const;
    void drawFindBar();

    std::string text_;
    Language language_ = Language::Plain;
    std::vector<CodeRef> refs_;
    std::vector<std::uint32_t> lineStarts_{0};
    std::vector<Span> spans_;                      // sorted; none crosses a line break
    std::vector<std::uint32_t> lineFirstSpan_{0};  // index into spans_ per line
    std::size_t longestLine_ = 0;

    std::uint32_t anchor_ = 0;
    std::uint32_t caret_ = 0;
    bool dragging_ = false;

    std::optional<std::uint32_t> scrollRequest_;
    int flashLine_ = -1;
    double flashUntil_ = 0.0;

    bool findOpen_ = false;
    bool focusFind_ = false;
    bool findCase_ = false;
    std::string find_;
    std::vector<std::uint32_t> matches_;
    int matchIndex_ = -1;
};

}  // namespace em
