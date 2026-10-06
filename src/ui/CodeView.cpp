#include "ui/CodeView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <IconsFontAwesome6.h>

#include "app/Theme.h"
#include "core/StringUtil.h"

namespace em {
namespace {

constexpr std::size_t kMaxMatches = 100000;

bool identChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
}

bool identStart(char c) {
    return identChar(c) && !(c >= '0' && c <= '9');
}

bool javaKeyword(std::string_view w) {
    static constexpr std::array<std::string_view, 53> kWords = {
        "abstract", "assert",     "boolean",  "break",     "byte",     "case",
        "catch",    "char",       "class",    "const",     "continue", "default",
        "do",       "double",     "else",     "enum",      "extends",  "final",
        "finally",  "float",      "for",      "goto",      "if",       "implements",
        "import",   "instanceof", "int",      "interface", "long",     "native",
        "new",      "package",    "private",  "protected", "public",   "return",
        "short",    "static",     "strictfp", "super",     "switch",   "synchronized",
        "this",     "throw",      "throws",   "transient", "try",      "void",
        "volatile", "while",      "true",     "false",     "null"};
    return std::find(kWords.begin(), kWords.end(), w) != kWords.end();
}

// Colours per theme, IntelliJ-like: they are what most Android developers
// read code in all day.
ImU32 colorFor(int color) {
    static constexpr std::array<ImU32, 12> kDark = {
        IM_COL32(0xD4, 0xD4, 0xD4, 0xFF),  // default
        IM_COL32(0xCC, 0x78, 0x32, 0xFF),  // keyword
        IM_COL32(0x6A, 0xAB, 0x73, 0xFF),  // string
        IM_COL32(0x68, 0x97, 0xBB, 0xFF),  // number
        IM_COL32(0x80, 0x80, 0x80, 0xFF),  // comment
        IM_COL32(0xBB, 0xB5, 0x29, 0xFF),  // annotation
        IM_COL32(0x4E, 0xC9, 0xB0, 0xFF),  // type
        IM_COL32(0xFF, 0xC6, 0x6D, 0xFF),  // method
        IM_COL32(0xB3, 0x89, 0xC5, 0xFF),  // field
        IM_COL32(0xD4, 0xD4, 0xD4, 0xFF),  // variable
        IM_COL32(0xE8, 0xBF, 0x6A, 0xFF),  // tag
        IM_COL32(0xBA, 0xBA, 0xBA, 0xFF),  // attribute
    };
    static constexpr std::array<ImU32, 12> kLight = {
        IM_COL32(0x1F, 0x1F, 0x1F, 0xFF), IM_COL32(0x00, 0x33, 0xB3, 0xFF),
        IM_COL32(0x06, 0x7D, 0x17, 0xFF), IM_COL32(0x17, 0x50, 0xEB, 0xFF),
        IM_COL32(0x8C, 0x8C, 0x8C, 0xFF), IM_COL32(0x9E, 0x88, 0x0D, 0xFF),
        IM_COL32(0x00, 0x80, 0x80, 0xFF), IM_COL32(0x00, 0x62, 0x7A, 0xFF),
        IM_COL32(0x87, 0x10, 0x94, 0xFF), IM_COL32(0x1F, 0x1F, 0x1F, 0xFF),
        IM_COL32(0x00, 0x33, 0xB3, 0xFF), IM_COL32(0x17, 0x4A, 0xD4, 0xFF),
    };
    const auto& table = theme::mode() == theme::Mode::Light ? kLight : kDark;
    return table[static_cast<std::size_t>(color)];
}

ImU32 withAlpha(ImVec4 c, float alpha) {
    return ImGui::GetColorU32({c.x, c.y, c.z, alpha});
}

}  // namespace

// --- text and highlighting ------------------------------------------------------------------

void CodeView::setText(std::string text, Language language, std::vector<CodeRef> refs) {
    // One byte per byte, so positions from jadx stay valid.
    std::replace(text.begin(), text.end(), '\t', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    text_ = std::move(text);
    language_ = language;
    refs_ = std::move(refs);
    lineStarts_.assign(1, 0);
    longestLine_ = 0;
    for (std::uint32_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') {
            longestLine_ = std::max<std::size_t>(longestLine_, i - lineStarts_.back());
            lineStarts_.push_back(i + 1);
        }
    }
    longestLine_ = std::max<std::size_t>(longestLine_, text_.size() - lineStarts_.back());
    anchor_ = caret_ = 0;
    dragging_ = false;
    scrollRequest_.reset();
    flashLine_ = -1;
    highlight();
    findMatches();
}

void CodeView::addSpan(std::uint32_t start, std::uint32_t end, Color color) {
    if (color == Default || start >= end) return;
    // Split at line breaks so drawing never looks past the line it is on.
    while (start < end) {
        const auto nl = text_.find('\n', start);
        const std::uint32_t stop =
            nl == std::string::npos
                ? end
                : std::min<std::uint32_t>(end, static_cast<std::uint32_t>(nl));
        if (stop > start) spans_.push_back({start, stop, color});
        start = stop + 1;
    }
}

CodeView::Color CodeView::refColor(std::uint32_t pos) const {
    const auto it =
        std::lower_bound(refs_.begin(), refs_.end(), pos,
                         [](const CodeRef& r, std::uint32_t p) { return r.pos < p; });
    if (it == refs_.end() || it->pos != pos) return Default;
    switch (it->kind) {
        case 'c':
        case 'C':
            return Type;
        case 'm':
        case 'M':
            return Method;
        case 'f':
        case 'F':
            return Field;
        default:
            return Variable;
    }
}

void CodeView::highlight() {
    spans_.clear();
    switch (language_) {
        case Language::Java:
            highlightJava();
            break;
        case Language::Smali:
            highlightSmali();
            break;
        case Language::Xml:
            highlightXml();
            break;
        case Language::Plain:
            break;
    }
    lineFirstSpan_.assign(lineStarts_.size(), 0);
    std::size_t s = 0;
    for (std::size_t line = 0; line < lineStarts_.size(); ++line) {
        while (s < spans_.size() && spans_[s].start < lineStarts_[line]) ++s;
        lineFirstSpan_[line] = static_cast<std::uint32_t>(s);
    }
}

void CodeView::highlightJava() {
    const std::string& t = text_;
    const auto n = static_cast<std::uint32_t>(t.size());
    std::uint32_t i = 0;
    while (i < n) {
        const char c = t[i];
        if (c == '/' && i + 1 < n && t[i + 1] == '/') {
            const auto e = t.find('\n', i);
            const std::uint32_t end =
                e == std::string::npos ? n : static_cast<std::uint32_t>(e);
            addSpan(i, end, Comment);
            i = end;
        } else if (c == '/' && i + 1 < n && t[i + 1] == '*') {
            const auto e = t.find("*/", i + 2);
            const std::uint32_t end =
                e == std::string::npos ? n : static_cast<std::uint32_t>(e + 2);
            addSpan(i, end, Comment);
            i = end;
        } else if (c == '"' || c == '\'') {
            std::uint32_t j = i + 1;
            while (j < n && t[j] != c && t[j] != '\n') j += t[j] == '\\' ? 2 : 1;
            j = std::min(j + 1, n);
            addSpan(i, j, String);
            i = j;
        } else if (c == '@' && i + 1 < n && identStart(t[i + 1])) {
            std::uint32_t j = i + 1;
            while (j < n && (identChar(t[j]) || t[j] == '.')) ++j;
            addSpan(i, j, Annotation);
            i = j;
        } else if (identStart(c)) {
            std::uint32_t j = i + 1;
            while (j < n && identChar(t[j])) ++j;
            const std::string_view word{t.data() + i, j - i};
            Color color = refColor(i);
            if (color == Default && javaKeyword(word)) color = Keyword;
            addSpan(i, j, color);
            i = j;
        } else if (c >= '0' && c <= '9') {
            std::uint32_t j = i + 1;
            while (j < n && (identChar(t[j]) || t[j] == '.')) ++j;
            addSpan(i, j, Number);
            i = j;
        } else {
            ++i;
        }
    }
}

void CodeView::highlightSmali() {
    for (std::size_t line = 0; line < lineStarts_.size(); ++line) {
        const std::uint32_t ls = lineStarts_[line];
        const std::uint32_t le = lineEnd(static_cast<int>(line));
        std::uint32_t i = ls;
        while (i < le && text_[i] == ' ') ++i;
        bool first = true;
        while (i < le) {
            const char c = text_[i];
            if (c == '#') {
                addSpan(i, le, Comment);
                break;
            }
            if (c == '"') {
                std::uint32_t j = i + 1;
                while (j < le && text_[j] != '"') j += text_[j] == '\\' ? 2 : 1;
                j = std::min(j + 1, le);
                addSpan(i, j, String);
                i = j;
                first = false;
                continue;
            }
            std::uint32_t j = i;
            while (j < le && text_[j] != ' ' && text_[j] != ',' && text_[j] != '"' &&
                   text_[j] != '{' && text_[j] != '}' && text_[j] != '(' && text_[j] != ')')
                ++j;
            if (j == i) {
                ++i;
                continue;
            }
            const std::string_view tok{text_.data() + i, j - i};
            Color color = Default;
            if (first && tok.starts_with('.'))
                color = Keyword;
            else if (first && tok.starts_with(':'))
                color = Annotation;
            else if (first)
                color = Method;
            else if (tok.starts_with(':'))
                color = Annotation;
            else if ((tok[0] == 'v' || tok[0] == 'p') && tok.size() > 1 &&
                     std::all_of(tok.begin() + 1, tok.end(),
                                 [](char d) { return d >= '0' && d <= '9'; }))
                color = Field;
            else if (tok[0] == 'L' || tok.starts_with("[L") ||
                     tok.find(";->") != std::string_view::npos)
                color = Type;
            else if ((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '-')
                color = Number;
            addSpan(i, j, color);
            first = false;
            i = j;
        }
    }
}

void CodeView::highlightXml() {
    const std::string& t = text_;
    const auto n = static_cast<std::uint32_t>(t.size());
    std::uint32_t i = 0;
    while (i < n) {
        if (t.compare(i, 4, "<!--") == 0) {
            const auto e = t.find("-->", i + 4);
            const std::uint32_t end =
                e == std::string::npos ? n : static_cast<std::uint32_t>(e + 3);
            addSpan(i, end, Comment);
            i = end;
        } else if (t[i] == '<') {
            std::uint32_t j = i + 1;
            if (j < n && (t[j] == '/' || t[j] == '?' || t[j] == '!')) ++j;
            while (j < n && (identChar(t[j]) || t[j] == ':' || t[j] == '.' || t[j] == '-')) ++j;
            addSpan(i, j, Tag);
            i = j;
            // Attributes until the tag closes.
            while (i < n && t[i] != '>') {
                if (t[i] == '"') {
                    const auto e = t.find('"', i + 1);
                    const std::uint32_t end =
                        e == std::string::npos ? n : static_cast<std::uint32_t>(e + 1);
                    addSpan(i, end, String);
                    i = end;
                } else if (identStart(t[i])) {
                    std::uint32_t k = i;
                    while (k < n &&
                           (identChar(t[k]) || t[k] == ':' || t[k] == '.' || t[k] == '-'))
                        ++k;
                    addSpan(i, k, Attribute);
                    i = k;
                } else {
                    ++i;
                }
            }
            if (i < n) {
                const std::uint32_t start = i > 0 && t[i - 1] == '/' ? i - 1 : i;
                addSpan(start, i + 1, Tag);
                ++i;
            }
        } else {
            ++i;
        }
    }
}

// --- geometry -------------------------------------------------------------------------------

int CodeView::lineOf(std::uint32_t pos) const {
    const auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), pos);
    return static_cast<int>(it - lineStarts_.begin()) - 1;
}

std::uint32_t CodeView::lineEnd(int line) const {
    const auto l = static_cast<std::size_t>(line);
    return l + 1 < lineStarts_.size() ? lineStarts_[l + 1] - 1
                                      : static_cast<std::uint32_t>(text_.size());
}

float CodeView::xOf(int line, std::uint32_t pos) const {
    const std::uint32_t ls = lineStarts_[static_cast<std::size_t>(line)];
    pos = std::clamp(pos, ls, lineEnd(line));
    return ImGui::CalcTextSize(text_.data() + ls, text_.data() + pos).x;
}

std::uint32_t CodeView::posAt(int line, float x) const {
    const std::uint32_t ls = lineStarts_[static_cast<std::size_t>(line)];
    const std::uint32_t le = lineEnd(line);
    if (x <= 0.0F) return ls;
    // Binary search on the prefix width, then pick the nearer edge.
    std::uint32_t lo = ls;
    std::uint32_t hi = le;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo + 1) / 2;
        if (xOf(line, mid) <= x)
            lo = mid;
        else
            hi = mid - 1;
    }
    if (lo < le) {
        std::uint32_t next = lo + 1;
        while (next < le && (static_cast<unsigned char>(text_[next]) & 0xC0) == 0x80) ++next;
        if (x - xOf(line, lo) > xOf(line, next) - x) lo = next;
    }
    return lo;
}

std::pair<std::uint32_t, std::uint32_t> CodeView::wordAt(std::uint32_t pos) const {
    const auto n = static_cast<std::uint32_t>(text_.size());
    if (n == 0) return {0, 0};
    pos = std::min(pos, n);
    std::uint32_t s = pos;
    while (s > 0 && identChar(text_[s - 1])) --s;
    std::uint32_t e = pos;
    while (e < n && identChar(text_[e])) ++e;
    if (s == e || !identStart(text_[s])) return {pos, pos};
    return {s, e};
}

const CodeRef* CodeView::refAt(std::uint32_t pos) const {
    const auto [s, e] = wordAt(pos);
    if (s == e) return nullptr;
    const auto it =
        std::lower_bound(refs_.begin(), refs_.end(), s,
                         [](const CodeRef& r, std::uint32_t p) { return r.pos < p; });
    return it != refs_.end() && it->pos == s ? &*it : nullptr;
}

void CodeView::scrollTo(std::uint32_t pos) {
    pos = std::min<std::uint32_t>(pos, static_cast<std::uint32_t>(text_.size()));
    anchor_ = caret_ = pos;
    scrollRequest_ = pos;
    flashLine_ = lineOf(pos);
    flashUntil_ = ImGui::GetTime() + 1.2;
}

// --- find -----------------------------------------------------------------------------------

void CodeView::openFind() {
    findOpen_ = true;
    focusFind_ = true;
    // Seed with the selection, like most editors.
    const std::uint32_t a = std::min(anchor_, caret_);
    const std::uint32_t b = std::max(anchor_, caret_);
    if (b > a && b - a < 200 && text_.find('\n', a) >= b) {
        find_ = text_.substr(a, b - a);
        findMatches();
    }
}

void CodeView::findMatches() {
    matches_.clear();
    matchIndex_ = -1;
    if (find_.empty()) return;
    if (findCase_) {
        for (auto at = text_.find(find_);
             at != std::string::npos && matches_.size() < kMaxMatches;
             at = text_.find(find_, at + 1))
            matches_.push_back(static_cast<std::uint32_t>(at));
    } else {
        const std::string hay = toLower(text_);
        const std::string needle = toLower(find_);
        for (auto at = hay.find(needle);
             at != std::string::npos && matches_.size() < kMaxMatches;
             at = hay.find(needle, at + 1))
            matches_.push_back(static_cast<std::uint32_t>(at));
    }
    // Start from the first match after the caret.
    const auto it = std::lower_bound(matches_.begin(), matches_.end(), caret_);
    if (!matches_.empty())
        matchIndex_ = static_cast<int>(it == matches_.end() ? 0 : it - matches_.begin());
}

void CodeView::selectMatch(int index) {
    if (matches_.empty()) return;
    const int count = static_cast<int>(matches_.size());
    matchIndex_ = ((index % count) + count) % count;
    const std::uint32_t at = matches_[static_cast<std::size_t>(matchIndex_)];
    scrollTo(at);
    anchor_ = at;
    caret_ = at + static_cast<std::uint32_t>(find_.size());
    flashLine_ = -1;
}

void CodeView::drawFindBar() {
    if (focusFind_) {
        ImGui::SetKeyboardFocusHere();
        focusFind_ = false;
    }
    ImGui::SetNextItemWidth(std::min(320.0F, ImGui::GetContentRegionAvail().x * 0.5F));
    const bool enter =
        ImGui::InputTextWithHint("##find", ICON_FA_MAGNIFYING_GLASS "  Find in file", &find_,
                                 ImGuiInputTextFlags_EnterReturnsTrue);
    const bool edited = ImGui::IsItemEdited();
    const bool findActive = ImGui::IsItemActive() || ImGui::IsItemFocused();
    if (edited) {
        findMatches();
        if (matchIndex_ >= 0) selectMatch(matchIndex_);
    }
    if (enter) {
        selectMatch(matchIndex_ + (ImGui::GetIO().KeyShift ? -1 : 1));
        ImGui::SetKeyboardFocusHere(-1);
    }
    if (findActive && ImGui::IsKeyPressed(ImGuiKey_Escape)) findOpen_ = false;
    ImGui::SameLine();
    if (find_.empty())
        ImGui::TextDisabled("     ");
    else if (matches_.empty())
        ImGui::TextColored(theme::palette().bad, "no match");
    else
        ImGui::TextDisabled("%d/%zu%s", matchIndex_ + 1, matches_.size(),
                            matches_.size() >= kMaxMatches ? "+" : "");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ARROW_UP)) selectMatch(matchIndex_ - 1);
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ARROW_DOWN)) selectMatch(matchIndex_ + 1);
    ImGui::SameLine();
    if (ImGui::Checkbox("Aa", &findCase_)) findMatches();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Match case");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_XMARK)) findOpen_ = false;
}

void CodeView::copySelection() const {
    const std::uint32_t a = std::min(anchor_, caret_);
    const std::uint32_t b = std::max(anchor_, caret_);
    if (b > a) ImGui::SetClipboardText(text_.substr(a, b - a).c_str());
}

// --- drawing --------------------------------------------------------------------------------

CodeView::Action CodeView::draw(const char* id) {
    Action action;
    ImGui::PushID(id);
    if (findOpen_) drawFindBar();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::BeginChild("##code", {0.0F, 0.0F}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleColor();

    const auto& palette = theme::palette();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const float lineH = std::round(ImGui::GetTextLineHeight() * 1.25F);
    const float charW = ImGui::CalcTextSize("M").x;
    const int lines = lineCount();
    const int digits = std::max(3, static_cast<int>(std::to_string(lines).size()));
    const float gutter = charW * static_cast<float>(digits + 2);
    const float textPad = charW;

    if (scrollRequest_) {
        const int line = lineOf(*scrollRequest_);
        const float y = static_cast<float>(line) * lineH;
        const float viewH = ImGui::GetWindowHeight();
        if (y < ImGui::GetScrollY() + lineH || y > ImGui::GetScrollY() + viewH - lineH * 2.0F)
            ImGui::SetScrollY(std::max(0.0F, y - viewH / 3.0F));
        const float x = xOf(line, *scrollRequest_);
        const float viewW = ImGui::GetWindowWidth() - gutter;
        if (x < ImGui::GetScrollX() || x > ImGui::GetScrollX() + viewW - charW * 4.0F)
            ImGui::SetScrollX(std::max(0.0F, x - viewW / 3.0F));
        scrollRequest_.reset();
    }

    const ImVec2 winPos = ImGui::GetWindowPos();
    const float scrollX = ImGui::GetScrollX();
    const float scrollY = ImGui::GetScrollY();
    const float viewH = ImGui::GetWindowHeight();
    const float viewW = ImGui::GetWindowWidth();
    const int first = std::clamp(static_cast<int>(scrollY / lineH), 0, std::max(0, lines - 1));
    const int last = std::min(lines, first + static_cast<int>(viewH / lineH) + 2);
    const float textX0 = winPos.x + gutter + textPad - scrollX;
    const auto lineY = [&](int line) {
        return winPos.y + static_cast<float>(line) * lineH - scrollY;
    };
    const float textYOffset = (lineH - ImGui::GetTextLineHeight()) * 0.5F;

    // The whole text as one invisible item: content size for the scrollbars,
    // and the mouse.
    const float contentW =
        gutter + textPad + static_cast<float>(longestLine_) * charW + charW * 8.0F;
    ImGui::SetCursorPos({0.0F, 0.0F});
    ImGui::InvisibleButton(
        "##text", {std::max(contentW, viewW - 20.0F), static_cast<float>(lines) * lineH},
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
            ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsWindowFocused();

    // Mouse -> position.
    std::optional<std::uint32_t> mousePos;
    if (hovered || dragging_) {
        const int line =
            std::clamp(static_cast<int>((io.MousePos.y - winPos.y + scrollY) / lineH), 0,
                       std::max(0, lines - 1));
        if (lines > 0) mousePos = posAt(line, io.MousePos.x - textX0);
    }
    const CodeRef* hoverRef = hovered && mousePos ? refAt(*mousePos) : nullptr;
    const bool navModifier = io.KeyCtrl || io.KeySuper;

    if (hovered && mousePos) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (navModifier && hoverRef != nullptr) {
                action = {Action::Kind::GoTo, wordAt(*mousePos).first};
            } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                const auto [s, e] = wordAt(*mousePos);
                anchor_ = s;
                caret_ = e;
            } else {
                caret_ = *mousePos;
                if (!io.KeyShift) anchor_ = caret_;
                dragging_ = true;
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && hoverRef != nullptr)
            action = {Action::Kind::GoTo, wordAt(*mousePos).first};
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const std::uint32_t a = std::min(anchor_, caret_);
            const std::uint32_t b = std::max(anchor_, caret_);
            if (*mousePos < a || *mousePos > b || a == b) anchor_ = caret_ = *mousePos;
            ImGui::OpenPopup("##codemenu");
        }
        if (hoverRef != nullptr && navModifier)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }
    if (dragging_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && mousePos) {
            caret_ = *mousePos;
            // Scroll while dragging past the edges.
            if (io.MousePos.y < winPos.y) ImGui::SetScrollY(scrollY - lineH);
            if (io.MousePos.y > winPos.y + viewH) ImGui::SetScrollY(scrollY + lineH);
        } else {
            dragging_ = false;
        }
    }

    // Keyboard.
    if (focused || findOpen_) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, ImGuiInputFlags_RouteFocused))
            openFind();
        if (ImGui::IsKeyPressed(ImGuiKey_F3)) selectMatch(matchIndex_ + (io.KeyShift ? -1 : 1));
    }
    if (focused && !io.WantTextInput) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, ImGuiInputFlags_RouteFocused))
            copySelection();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, ImGuiInputFlags_RouteFocused)) {
            anchor_ = 0;
            caret_ = static_cast<std::uint32_t>(text_.size());
        }
        // jadx-gui's keys: D declaration, X usages. Plus the IntelliJ ones.
        const bool plain = !io.KeyCtrl && !io.KeySuper && !io.KeyAlt && !io.KeyShift;
        if ((plain && ImGui::IsKeyPressed(ImGuiKey_D, false)) ||
            ImGui::IsKeyPressed(ImGuiKey_F12, false) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, ImGuiInputFlags_RouteFocused)) {
            if (refAt(caret_) != nullptr) action = {Action::Kind::GoTo, wordAt(caret_).first};
        }
        if ((plain && ImGui::IsKeyPressed(ImGuiKey_X, false)) ||
            (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F7, false))) {
            if (refAt(caret_) != nullptr)
                action = {Action::Kind::FindUsages, wordAt(caret_).first};
        }
        const float page = std::max(1.0F, std::floor(viewH / lineH) - 1.0F) * lineH;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ImGui::SetScrollY(scrollY + lineH);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) ImGui::SetScrollY(scrollY - lineH);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) ImGui::SetScrollY(scrollY + page);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) ImGui::SetScrollY(scrollY - page);
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) ImGui::SetScrollY(0.0F);
        if (ImGui::IsKeyPressed(ImGuiKey_End)) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    }

    if (ImGui::BeginPopup("##codemenu")) {
        const CodeRef* ref = refAt(caret_);
        if (ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Go to declaration", "D", false,
                            ref != nullptr))
            action = {Action::Kind::GoTo, wordAt(caret_).first};
        if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Find usages", "X", false,
                            ref != nullptr))
            action = {Action::Kind::FindUsages, wordAt(caret_).first};
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy", "Ctrl+C", false, anchor_ != caret_))
            copySelection();
        const auto [ws, we] = wordAt(caret_);
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy word", nullptr, false, we > ws))
            ImGui::SetClipboardText(text_.substr(ws, we - ws).c_str());
        if (ImGui::MenuItem("Select all", "Ctrl+A")) {
            anchor_ = 0;
            caret_ = static_cast<std::uint32_t>(text_.size());
        }
        if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Find in file", "Ctrl+F")) openFind();
        ImGui::EndPopup();
    }

    // --- paint ---
    const ImVec2 clipMin = winPos;
    const ImVec2 clipMax{winPos.x + viewW, winPos.y + viewH};
    const std::uint32_t selA = std::min(anchor_, caret_);
    const std::uint32_t selB = std::max(anchor_, caret_);
    const ImU32 selColor = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
    const ImU32 matchColor = withAlpha(palette.warning, 0.30F);
    const ImU32 currentMatchColor = withAlpha(palette.warning, 0.65F);
    const ImU32 wordColor = withAlpha(palette.accent, 0.22F);
    const ImU32 caretLineColor = withAlpha(palette.accent, 0.07F);
    const ImU32 mutedColor = ImGui::GetColorU32(palette.muted);

    // Occurrences of the word under the caret (no selection).
    std::string_view word;
    if (selA == selB) {
        const auto [s, e] = wordAt(caret_);
        if (e - s >= 2) word = std::string_view{text_}.substr(s, e - s);
    }
    const int caretLine = lines > 0 ? lineOf(caret_) : -1;
    const double now = ImGui::GetTime();

    draw->PushClipRect({clipMin.x + gutter, clipMin.y}, clipMax, true);
    for (int line = first; line < last; ++line) {
        const float y = lineY(line);
        const std::uint32_t ls = lineStarts_[static_cast<std::size_t>(line)];
        const std::uint32_t le = lineEnd(line);
        const float rowX0 = winPos.x + gutter;
        const float rowX1 = winPos.x + viewW;

        if (line == caretLine)
            draw->AddRectFilled({rowX0, y}, {rowX1, y + lineH}, caretLineColor);
        if (line == flashLine_ && now < flashUntil_) {
            const float a = static_cast<float>((flashUntil_ - now) / 1.2);
            draw->AddRectFilled({rowX0, y}, {rowX1, y + lineH},
                                withAlpha(palette.accent, 0.35F * a));
        }
        // Selection.
        if (selB > selA && selA <= le && selB >= ls) {
            const float x0 = textX0 + xOf(line, std::max(selA, ls));
            float x1 = textX0 + xOf(line, std::min(selB, le));
            if (selB > le) x1 += charW * 0.5F;  // the line break is selected
            draw->AddRectFilled({x0, y}, {x1, y + lineH}, selColor);
        }
        // Find matches.
        if (findOpen_ && !matches_.empty()) {
            auto it = std::lower_bound(matches_.begin(), matches_.end(), ls);
            for (; it != matches_.end() && *it < le; ++it) {
                const float x0 = textX0 + xOf(line, *it);
                const float x1 =
                    textX0 + xOf(line, *it + static_cast<std::uint32_t>(find_.size()));
                const bool current =
                    matchIndex_ >= 0 && *it == matches_[static_cast<std::size_t>(matchIndex_)];
                draw->AddRectFilled({x0, y}, {x1, y + lineH},
                                    current ? currentMatchColor : matchColor);
            }
        }
        // Word occurrences.
        if (!word.empty()) {
            const std::string_view row{text_.data() + ls, le - ls};
            for (auto at = row.find(word); at != std::string_view::npos;
                 at = row.find(word, at + 1)) {
                const bool startOk = at == 0 || !identChar(row[at - 1]);
                const bool endOk =
                    at + word.size() >= row.size() || !identChar(row[at + word.size()]);
                if (!startOk || !endOk) continue;
                const auto p = ls + static_cast<std::uint32_t>(at);
                draw->AddRectFilled(
                    {textX0 + xOf(line, p), y},
                    {textX0 + xOf(line, p + static_cast<std::uint32_t>(word.size())),
                     y + lineH},
                    wordColor);
            }
        }

        // Text, span by span.
        float x = textX0;
        const float ty = y + textYOffset;
        std::uint32_t at = ls;
        const auto drawRun = [&](std::uint32_t a, std::uint32_t b, ImU32 col) {
            if (b <= a) return;
            const char* s = text_.data() + a;
            const char* e = text_.data() + b;
            const float w = ImGui::CalcTextSize(s, e).x;
            if (x + w >= clipMin.x && x <= clipMax.x) draw->AddText({x, ty}, col, s, e);
            x += w;
        };
        for (std::size_t s = lineFirstSpan_[static_cast<std::size_t>(line)];
             s < spans_.size() && spans_[s].start < le; ++s) {
            drawRun(at, spans_[s].start, colorFor(Default));
            drawRun(spans_[s].start, spans_[s].end, colorFor(spans_[s].color));
            at = spans_[s].end;
        }
        drawRun(at, le, colorFor(Default));

        // Underline the reference under the pointer.
        if (hoverRef != nullptr && mousePos && lineOf(*mousePos) == line) {
            const auto [ws, we] = wordAt(*mousePos);
            const float ux0 = textX0 + xOf(line, ws);
            const float ux1 = textX0 + xOf(line, we);
            draw->AddLine({ux0, y + lineH - 2.0F}, {ux1, y + lineH - 2.0F},
                          navModifier ? colorFor(Type) : withAlpha(palette.muted, 0.8F), 1.0F);
        }
    }
    // Caret.
    if (focused && caretLine >= first && caretLine < last && selA == selB) {
        const float cx = textX0 + xOf(caretLine, caret_);
        const float cy = lineY(caretLine);
        if (std::fmod(now, 1.0) < 0.6)
            draw->AddLine({cx, cy + 2.0F}, {cx, cy + lineH - 2.0F}, colorFor(Default));
    }
    draw->PopClipRect();

    // Gutter, drawn last so text scrolled under it stays hidden.
    draw->AddRectFilled(clipMin, {clipMin.x + gutter, clipMax.y},
                        ImGui::GetColorU32(ImGuiCol_FrameBg));
    for (int line = first; line < last; ++line) {
        char num[16];
        std::snprintf(num, sizeof num, "%d", line + 1);
        const float w = ImGui::CalcTextSize(num).x;
        draw->AddText({winPos.x + gutter - charW - w, lineY(line) + textYOffset},
                      line == caretLine ? colorFor(Default) : mutedColor, num);
    }

    ImGui::EndChild();
    ImGui::PopID();
    return action;
}

}  // namespace em
