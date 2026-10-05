#include "collect/LayoutInspector.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <map>
#include <tuple>

#include "collect/ScreenMirror.h"
#include "core/Gzip.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/StringUtil.h"

namespace em {
namespace {

constexpr const char* kCategory = "layout";
constexpr std::string_view kSentinel = "@@EMERON_LAYOUT_9c1e@@";
constexpr const char* kDumpFile = "/data/local/tmp/emeron-layout.xml";
constexpr const char* kDumpLog = "/data/local/tmp/emeron-layout.log";
constexpr int kMinTouchDp = 48;
constexpr int kHeavyGoneDescendants = 10;
constexpr int kDeepLevels = 12;

std::size_t indentOf(std::string_view line) {
    std::size_t n = 0;
    while (n < line.size() && line[n] == ' ') ++n;
    return n;
}

// Reads a (possibly negative) int at `pos`, advancing it.
bool readInt(std::string_view s, std::size_t& pos, int& out) {
    const char* first = s.data() + pos;
    const char* last = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    if (ec != std::errc{} || ptr == first) return false;
    pos += static_cast<std::size_t>(ptr - first);
    return true;
}

bool expect(std::string_view s, std::size_t& pos, char c) {
    if (pos >= s.size() || s[pos] != c) return false;
    ++pos;
    return true;
}

// "l,t-r,b" as View.toString writes it.
std::optional<LayoutRect> parseViewBounds(std::string_view token) {
    LayoutRect r;
    std::size_t pos = 0;
    if (!readInt(token, pos, r.left) || !expect(token, pos, ',') ||
        !readInt(token, pos, r.top) || !expect(token, pos, '-') ||
        !readInt(token, pos, r.right) || !expect(token, pos, ',') ||
        !readInt(token, pos, r.bottom) || pos != token.size()) {
        return std::nullopt;
    }
    return r;
}

// "[l,t][r,b]", as uiautomator and dumpsys window write it.
std::optional<LayoutRect> parseBracketBounds(std::string_view token) {
    LayoutRect r;
    std::size_t pos = 0;
    if (!expect(token, pos, '[') || !readInt(token, pos, r.left) || !expect(token, pos, ',') ||
        !readInt(token, pos, r.top) || !expect(token, pos, ']') || !expect(token, pos, '[') ||
        !readInt(token, pos, r.right) || !expect(token, pos, ',') ||
        !readInt(token, pos, r.bottom) || !expect(token, pos, ']')) {
        return std::nullopt;
    }
    return r;
}

LayoutRect offset(LayoutRect r, int dx, int dy) {
    return {r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
}

LayoutRect intersect(const LayoutRect& a, const LayoutRect& b) {
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
            std::min(a.bottom, b.bottom)};
}

// "app:id/name" -> "<package>:id/name". View.toString writes "app" for the
// app's own resources and the real package for everything else.
std::string expandResourceId(std::string_view id, std::string_view packageName) {
    if (auto rest = stripPrefix(id, "app:"); rest && !packageName.empty()) {
        return std::string{packageName} + ":" + std::string{*rest};
    }
    return std::string{id};
}

std::string_view packageOf(std::string_view component) {
    const auto slash = component.find('/');
    return slash == std::string_view::npos ? component : component.substr(0, slash);
}

void computeScreenBounds(std::vector<LayoutNode>& nodes, std::size_t from) {
    // Parents always precede their children in the vector.
    for (std::size_t i = from; i < nodes.size(); ++i) {
        LayoutNode& node = nodes[i];
        if (node.parent < 0) continue;
        const LayoutRect& parent = nodes[static_cast<std::size_t>(node.parent)].bounds;
        node.bounds = offset(node.local, parent.left, parent.top);
    }
}

std::string decodeEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const auto semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        const std::string_view name = s.substr(i + 1, semi - i - 1);
        std::uint32_t code = 0;
        bool known = true;
        if (name == "amp")
            code = '&';
        else if (name == "lt")
            code = '<';
        else if (name == "gt")
            code = '>';
        else if (name == "quot")
            code = '"';
        else if (name == "apos")
            code = '\'';
        else if (name.size() > 1 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string_view digits = name.substr(hex ? 2 : 1);
            const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(),
                                                   code, hex ? 16 : 10);
            known =
                ec == std::errc{} && ptr == digits.data() + digits.size() && code <= 0x10FFFF;
        } else {
            known = false;
        }
        if (!known) {
            out += s[i];
            continue;
        }
        // UTF-8 encode.
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        i = semi;
    }
    return out;
}

bool isBlank(std::string_view s) {
    return trim(s).empty();
}

bool subtreeHasLabel(const std::vector<LayoutNode>& nodes, int index) {
    const LayoutNode& node = nodes[static_cast<std::size_t>(index)];
    if (!isBlank(node.text) || !isBlank(node.contentDescription)) return true;
    return std::any_of(node.children.begin(), node.children.end(),
                       [&](int c) { return subtreeHasLabel(nodes, c); });
}

int countDescendants(const std::vector<LayoutNode>& nodes, int index) {
    int count = 0;
    for (const int c : nodes[static_cast<std::size_t>(index)].children) {
        count += 1 + countDescendants(nodes, c);
    }
    return count;
}

std::string javaString(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    out += '"';
    return out;
}

void walk(const std::vector<LayoutNode>& nodes, int index, const std::function<void(int)>& fn) {
    fn(index);
    for (const int c : nodes[static_cast<std::size_t>(index)].children) walk(nodes, c, fn);
}

}  // namespace

// --- LayoutNode / LayoutSnapshot ------------------------------------------------

std::string_view LayoutNode::simpleName() const noexcept {
    const std::string_view name{className};
    const auto dot = name.find_last_of(".$");
    return dot == std::string_view::npos ? name : name.substr(dot + 1);
}

std::string_view LayoutNode::idName() const noexcept {
    const std::string_view id{resourceId};
    const auto slash = id.find('/');
    return slash == std::string_view::npos ? id : id.substr(slash + 1);
}

std::vector<int> LayoutSnapshot::rootsFor(int window) const {
    if (window >= 0 && window < static_cast<int>(windows.size())) {
        const int root = windows[static_cast<std::size_t>(window)].root;
        if (root >= 0) return {root};
        return {};
    }
    return accessibilityRoots;
}

std::string LayoutSnapshot::packageName() const {
    if (primaryWindow >= 0 && primaryWindow < static_cast<int>(windows.size())) {
        return std::string{
            packageOf(windows[static_cast<std::size_t>(primaryWindow)].component)};
    }
    for (const int r : accessibilityRoots) {
        if (!nodes[static_cast<std::size_t>(r)].packageName.empty()) {
            return nodes[static_cast<std::size_t>(r)].packageName;
        }
    }
    return {};
}

// --- View dump --------------------------------------------------------------------

std::optional<LayoutNode> parseViewLine(std::string_view line) {
    line = trim(line);
    const auto brace = line.find('{');
    if (brace == std::string_view::npos || brace == 0 || line.back() != '}')
        return std::nullopt;

    LayoutNode node;
    node.className = std::string{line.substr(0, brace)};
    const auto tokens = tokenize(line.substr(brace + 1, line.size() - brace - 2));
    if (tokens.size() < 4) return std::nullopt;

    node.hash = std::string{tokens[0]};
    const std::string_view view = tokens[1];
    const std::string_view priv = tokens[2];
    if (view.size() != 9 || priv.size() != 8) return std::nullopt;

    switch (view[0]) {
        case 'V':
            node.visibility = ViewVisibility::Visible;
            break;
        case 'I':
            node.visibility = ViewVisibility::Invisible;
            break;
        case 'G':
            node.visibility = ViewVisibility::Gone;
            break;
        default:
            return std::nullopt;
    }
    node.focusable = view[1] == 'F';
    node.enabled = view[2] == 'E';
    node.draws = view[3] == 'D';
    node.scrollsHorizontally = view[4] == 'H';
    node.scrollsVertically = view[5] == 'V';
    node.clickable = view[6] == 'C';
    node.longClickable = view[7] == 'L';
    node.contextClickable = view[8] == 'X';

    node.focused = priv[1] == 'F';
    node.selected = priv[2] == 'S';
    node.pressed = priv[3] == 'P' || priv[3] == 'p';
    node.hovered = priv[4] == 'H';
    node.activated = priv[5] == 'A';
    node.layoutRequested = priv[6] == 'I';
    node.dirty = priv[7] == 'D';

    const auto bounds = parseViewBounds(tokens[3]);
    if (!bounds) return std::nullopt;
    node.local = *bounds;

    for (std::size_t i = 4; i < tokens.size(); ++i) {
        const std::string_view token = tokens[i];
        if (token.starts_with('#') || token.starts_with("aid=")) continue;
        if (token.find(":id/") != std::string_view::npos) node.resourceId = std::string{token};
    }
    return node;
}

std::string expandComponent(std::string_view component) {
    const auto slash = component.find('/');
    if (slash == std::string_view::npos || slash + 1 >= component.size() ||
        component[slash + 1] != '.') {
        return std::string{component};
    }
    return std::string{component.substr(0, slash + 1)} +
           std::string{component.substr(0, slash)} + std::string{component.substr(slash + 1)};
}

std::optional<LayoutRect> findWindowFrame(std::string_view windowFrames,
                                          std::string_view component) {
    const std::string wanted = expandComponent(component);
    bool inWanted = false;
    for (const std::string_view raw : splitLines(windowFrames)) {
        const std::string_view line = trim(raw);
        if (line.starts_with("Window #")) {
            // "Window #15 Window{1ae4e70 u0 com.pkg/com.pkg.Cls}:"
            const auto open = line.find('{');
            const auto close = line.rfind('}');
            inWanted = false;
            if (open == std::string_view::npos || close == std::string_view::npos ||
                close < open)
                continue;
            const auto parts = tokenize(line.substr(open + 1, close - open - 1));
            if (parts.size() >= 3) inWanted = expandComponent(parts.back()) == wanted;
            continue;
        }
        if (!inWanted) continue;
        for (const std::string_view key :
             {std::string_view{" frame=["}, std::string_view{"mFrame=["}}) {
            const auto at = (" " + std::string{line}).find(key);
            if (at == std::string::npos) continue;
            // `at` indexes the space-prefixed copy; map back onto `line`.
            const std::size_t start = at + key.size() - 1 - 1;
            const auto end = line.find(' ', start);
            if (auto rect = parseBracketBounds(line.substr(start, end == std::string_view::npos
                                                                      ? std::string_view::npos
                                                                      : end - start))) {
                return rect;
            }
        }
    }
    return std::nullopt;
}

ViewDump parseActivityTop(std::string_view activityTop, std::string_view windowFrames) {
    ViewDump dump;
    std::string component;
    int pid = -1;
    bool resumed = false;
    bool sawResumed = false;  // fragments print their own mResumed= later

    const auto lines = splitLines(activityTop);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        const std::string_view trimmed = trim(line);

        if (auto rest = stripPrefix(trimmed, "ACTIVITY ")) {
            const auto parts = tokenize(*rest);
            component = parts.empty() ? std::string{} : std::string{parts[0]};
            pid = -1;
            resumed = false;
            sawResumed = false;
            for (const auto part : parts) {
                if (auto p = stripPrefix(part, "pid=")) pid = parseNumber<int>(*p).value_or(-1);
            }
            continue;
        }
        if (!sawResumed && trimmed.starts_with("mResumed=")) {
            resumed = trimmed.starts_with("mResumed=true");
            sawResumed = true;
            continue;
        }
        if (trimmed != "View Hierarchy:") continue;

        const std::size_t headerIndent = indentOf(line);
        const std::string packageName{packageOf(component)};
        LayoutWindow window;
        window.component = expandComponent(component);
        window.pid = pid;
        window.resumed = resumed;

        std::vector<std::pair<std::size_t, int>> stack;  // indent, node index
        const std::size_t firstNode = dump.nodes.size();
        for (++i; i < lines.size(); ++i) {
            const std::string_view nodeLine = lines[i];
            if (trim(nodeLine).empty()) continue;
            const std::size_t indent = indentOf(nodeLine);
            if (indent <= headerIndent) break;

            const std::string_view text = trim(nodeLine);
            LayoutNode node;
            if (window.root < 0) {
                // "DecorView@bfcdbf5[HomeActivity]" -- no bounds; the window has them.
                const auto at = text.find('@');
                const auto open = text.find('[');
                node.className = "com.android.internal.policy." +
                                 std::string{text.substr(
                                     0, at == std::string_view::npos ? text.size() : at)};
                if (at != std::string_view::npos) {
                    node.hash = std::string{text.substr(at + 1, open == std::string_view::npos
                                                                    ? std::string_view::npos
                                                                    : open - at - 1)};
                }
                if (open != std::string_view::npos && text.back() == ']') {
                    window.title = std::string{text.substr(open + 1, text.size() - open - 2)};
                }
            } else if (auto parsed = parseViewLine(text)) {
                node = std::move(*parsed);
                node.resourceId = expandResourceId(node.resourceId, packageName);
            } else {
                // A view whose toString() is overridden: keep it, so the tree
                // below it still hangs off the right parent.
                node.className =
                    std::string{text.substr(0, std::min<std::size_t>(text.find('{'), 120))};
            }

            while (!stack.empty() && stack.back().first >= indent) stack.pop_back();
            const int index = static_cast<int>(dump.nodes.size());
            node.parent = stack.empty() ? -1 : stack.back().second;
            if (node.parent >= 0) {
                dump.nodes[static_cast<std::size_t>(node.parent)].children.push_back(index);
            } else if (window.root < 0) {
                window.root = index;
            } else {
                // A second top-level line: not part of this tree.
                break;
            }
            dump.nodes.push_back(std::move(node));
            stack.emplace_back(indent, index);
        }
        --i;  // the line that ended the section is looked at by the outer loop

        if (window.root < 0) continue;
        LayoutNode& root = dump.nodes[static_cast<std::size_t>(window.root)];
        if (auto frame = findWindowFrame(windowFrames, window.component)) {
            window.frame = *frame;
        } else {
            // No frame: assume the window starts at 0,0 and spans its children.
            for (const int c : root.children) {
                const LayoutRect& r = dump.nodes[static_cast<std::size_t>(c)].local;
                window.frame.right = std::max(window.frame.right, r.right);
                window.frame.bottom = std::max(window.frame.bottom, r.bottom);
            }
        }
        root.bounds = window.frame;
        root.local = {0, 0, window.frame.width(), window.frame.height()};
        computeScreenBounds(dump.nodes, firstNode);
        dump.windows.push_back(std::move(window));
    }

    std::vector<int> roots;
    for (const auto& w : dump.windows) roots.push_back(w.root);
    finalizeTree(dump.nodes, roots);
    return dump;
}

std::optional<std::pair<int, int>> parseDisplaySize(std::string_view displays) {
    std::optional<std::pair<int, int>> first;
    int displayId = -1;
    for (const std::string_view raw : splitLines(displays)) {
        const std::string_view line = trim(raw);
        if (auto id = fieldAfter(line, "mDisplayId")) {
            displayId = parseNumber<int>(*id).value_or(-1);
            continue;
        }
        for (const auto token : tokenize(line)) {
            auto size = stripPrefix(token, "cur=");
            if (!size) continue;
            const auto x = size->find('x');
            if (x == std::string_view::npos) continue;
            const auto w = parseNumber<int>(size->substr(0, x));
            const auto h = parseNumber<int>(size->substr(x + 1));
            if (!w || !h || *w <= 0 || *h <= 0) continue;
            if (displayId == 0) return std::pair{*w, *h};
            if (!first) first = std::pair{*w, *h};
        }
    }
    return first;
}

std::optional<int> parseDensity(std::string_view wmDensity) {
    std::optional<int> physical;
    for (const std::string_view raw : splitLines(wmDensity)) {
        const std::string_view line = trim(raw);
        if (auto v = stripPrefix(line, "Override density:")) {
            if (auto n = parseNumber<int>(*v); n && *n > 0) return n;
        }
        if (auto v = stripPrefix(line, "Physical density:")) physical = parseNumber<int>(*v);
    }
    return physical;
}

// --- accessibility XML -------------------------------------------------------------

Result<std::vector<int>> parseAccessibilityXml(std::string_view xml,
                                               std::vector<LayoutNode>& out) {
    std::vector<int> roots;
    std::vector<int> stack;
    bool sawHierarchy = false;
    std::size_t pos = 0;

    while (true) {
        const auto lt = xml.find('<', pos);
        if (lt == std::string_view::npos) break;
        if (xml.substr(lt, 2) == "<?" || xml.substr(lt, 2) == "<!") {
            const auto end = xml.find('>', lt);
            if (end == std::string_view::npos) break;
            pos = end + 1;
            continue;
        }
        if (xml.substr(lt, 2) == "</") {
            const auto end = xml.find('>', lt);
            if (end == std::string_view::npos) break;
            if (xml.substr(lt + 2, 4) == "node" && !stack.empty()) stack.pop_back();
            pos = end + 1;
            continue;
        }

        // Tag name.
        std::size_t p = lt + 1;
        while (p < xml.size() && xml[p] != ' ' && xml[p] != '>' && xml[p] != '/' &&
               xml[p] != '\n')
            ++p;
        const std::string_view tag = xml.substr(lt + 1, p - lt - 1);

        // Attributes, up to '>' or '/>'; values may contain '>' so scan quotes.
        std::vector<std::pair<std::string_view, std::string_view>> attrs;
        bool selfClosing = false;
        bool closed = false;
        while (p < xml.size()) {
            while (p < xml.size() &&
                   (xml[p] == ' ' || xml[p] == '\n' || xml[p] == '\r' || xml[p] == '\t'))
                ++p;
            if (p >= xml.size()) break;
            if (xml[p] == '>') {
                closed = true;
                ++p;
                break;
            }
            if (xml[p] == '/' && p + 1 < xml.size() && xml[p + 1] == '>') {
                selfClosing = closed = true;
                p += 2;
                break;
            }
            const std::size_t nameStart = p;
            while (p < xml.size() && xml[p] != '=' && xml[p] != ' ' && xml[p] != '>') ++p;
            const std::string_view name = xml.substr(nameStart, p - nameStart);
            if (p >= xml.size() || xml[p] != '=') continue;
            ++p;
            if (p >= xml.size() || (xml[p] != '"' && xml[p] != '\'')) {
                return makeError(ErrorKind::ParseFailure, "malformed accessibility XML");
            }
            const char quote = xml[p++];
            const auto endQuote = xml.find(quote, p);
            if (endQuote == std::string_view::npos) {
                return makeError(ErrorKind::ParseFailure, "accessibility XML is truncated");
            }
            attrs.emplace_back(name, xml.substr(p, endQuote - p));
            p = endQuote + 1;
        }
        if (!closed)
            return makeError(ErrorKind::ParseFailure, "accessibility XML is truncated");
        pos = p;

        if (tag == "hierarchy") {
            sawHierarchy = true;
            continue;
        }
        if (tag != "node") continue;

        LayoutNode node;
        node.origin = LayoutNode::Origin::Accessibility;
        node.hasAccessibility = true;
        node.draws = true;
        for (const auto& [name, rawValue] : attrs) {
            const std::string value = decodeEntities(rawValue);
            const bool on = value == "true";
            if (name == "class")
                node.className = value;
            else if (name == "text")
                node.text = value;
            else if (name == "resource-id")
                node.resourceId = value;
            else if (name == "content-desc")
                node.contentDescription = value;
            else if (name == "package")
                node.packageName = value;
            else if (name == "checkable")
                node.checkable = on;
            else if (name == "checked")
                node.checked = on;
            else if (name == "clickable")
                node.clickable = on;
            else if (name == "enabled")
                node.enabled = on;
            else if (name == "focusable")
                node.focusable = on;
            else if (name == "focused")
                node.focused = on;
            else if (name == "scrollable")
                node.scrollable = on;
            else if (name == "long-clickable")
                node.longClickable = on;
            else if (name == "password")
                node.password = on;
            else if (name == "selected")
                node.selected = on;
            else if (name == "bounds") {
                if (auto r = parseBracketBounds(value)) node.bounds = *r;
            }
        }
        const int index = static_cast<int>(out.size());
        node.parent = stack.empty() ? -1 : stack.back();
        if (node.parent >= 0) {
            out[static_cast<std::size_t>(node.parent)].children.push_back(index);
            node.local =
                offset(node.bounds, -out[static_cast<std::size_t>(node.parent)].bounds.left,
                       -out[static_cast<std::size_t>(node.parent)].bounds.top);
        } else {
            roots.push_back(index);
            node.local = node.bounds;
        }
        out.push_back(std::move(node));
        if (!selfClosing) stack.push_back(index);
    }

    if (!sawHierarchy) {
        const std::string_view text = trim(xml);
        return makeError(ErrorKind::ParseFailure,
                         text.empty()
                             ? std::string{"uiautomator returned nothing"}
                             : "uiautomator: " + std::string{splitLines(text).front()});
    }
    finalizeTree(out, roots);
    return roots;
}

// --- merge ------------------------------------------------------------------------

int mergeAccessibility(std::vector<LayoutNode>& nodes, int root,
                       const std::vector<LayoutNode>& accessibility,
                       const std::vector<int>& accessibilityRoots) {
    if (root < 0 || accessibility.empty()) return 0;

    // Accessibility nodes in preorder, grouped by bounds: a chain of nested
    // wrappers with identical bounds appears in the same order in both trees.
    std::vector<int> order;
    for (const int r : accessibilityRoots)
        walk(accessibility, r, [&](int i) { order.push_back(i); });
    std::map<std::tuple<int, int, int, int>, std::vector<int>> byBounds;
    for (const int i : order) {
        const LayoutRect& b = accessibility[static_cast<std::size_t>(i)].bounds;
        byBounds[{b.left, b.top, b.right, b.bottom}].push_back(i);
    }
    std::vector<int> toView(accessibility.size(), -1);
    std::vector<bool> used(accessibility.size(), false);

    const auto adopt = [&](int view, int a) {
        LayoutNode& node = nodes[static_cast<std::size_t>(view)];
        const LayoutNode& src = accessibility[static_cast<std::size_t>(a)];
        node.hasAccessibility = true;
        node.text = src.text;
        node.contentDescription = src.contentDescription;
        node.packageName = src.packageName;
        node.checkable = src.checkable;
        node.checked = src.checked;
        node.scrollable = src.scrollable;
        node.password = src.password;
        used[static_cast<std::size_t>(a)] = true;
        toView[static_cast<std::size_t>(a)] = view;
    };

    int matched = 0;
    // Accessibility bounds are clipped to every ancestor, so a view is looked
    // up by the part of it its ancestors let through.
    const std::function<void(int, int, int, LayoutRect)> place = [&](int v, int dx, int dy,
                                                                     LayoutRect clip) {
        LayoutNode& node = nodes[static_cast<std::size_t>(v)];
        if (dx != 0 || dy != 0) {
            node.bounds = offset(node.bounds, dx, dy);
            node.shiftX = dx;
            node.shiftY = dy;
        }
        const LayoutRect visible = intersect(node.bounds, clip);
        if (node.shown && !visible.empty()) {
            int match = -1;
            const LayoutRect& b = node.bounds;
            if (auto it =
                    byBounds.find({visible.left, visible.top, visible.right, visible.bottom});
                it != byBounds.end()) {
                for (const int a : it->second) {
                    if (!used[static_cast<std::size_t>(a)] &&
                        accessibility[static_cast<std::size_t>(a)].resourceId ==
                            node.resourceId) {
                        match = a;
                        break;
                    }
                }
            }
            if (match < 0 && !node.resourceId.empty()) {
                // Not where the View dump puts it -- a scrolled container or a
                // translation. A unique node with the same id and size pins it.
                int candidate = -1;
                int count = 0;
                for (const int a : order) {
                    const LayoutNode& an = accessibility[static_cast<std::size_t>(a)];
                    if (used[static_cast<std::size_t>(a)] || an.resourceId != node.resourceId ||
                        an.bounds.width() != b.width() || an.bounds.height() != b.height()) {
                        continue;
                    }
                    candidate = a;
                    ++count;
                }
                if (count == 1) {
                    const LayoutRect& target =
                        accessibility[static_cast<std::size_t>(candidate)].bounds;
                    const int ddx = target.left - b.left;
                    const int ddy = target.top - b.top;
                    node.bounds = offset(node.bounds, ddx, ddy);
                    dx += ddx;
                    dy += ddy;
                    node.shiftX = dx;
                    node.shiftY = dy;
                    match = candidate;
                }
            }
            if (match >= 0) {
                adopt(v, match);
                ++matched;
            }
        }
        const std::vector<int> children = nodes[static_cast<std::size_t>(v)].children;
        const LayoutRect childClip = intersect(nodes[static_cast<std::size_t>(v)].bounds, clip);
        for (const int c : children) place(c, dx, dy, childClip);
    };
    place(root, 0, 0, nodes[static_cast<std::size_t>(root)].bounds);

    // Graft what only accessibility knows (Compose, WebView, virtual views)
    // under the view hosting it.
    const auto canHost = [&](int v) {
        const LayoutNode& host = nodes[static_cast<std::size_t>(v)];
        if (host.origin == LayoutNode::Origin::Accessibility) return true;
        if (host.className.find("ComposeView") != std::string::npos ||
            host.className.find("WebView") != std::string::npos) {
            return true;
        }
        return std::none_of(host.children.begin(), host.children.end(),
                            [&](int c) { return nodes[static_cast<std::size_t>(c)].shown; });
    };
    const std::function<void(int, int)> graft = [&](int a, int host) {
        int next = toView[static_cast<std::size_t>(a)];
        if (next < 0 && host >= 0 && canHost(host)) {
            LayoutNode copy = accessibility[static_cast<std::size_t>(a)];
            copy.children.clear();
            copy.parent = host;
            const LayoutRect& hb = nodes[static_cast<std::size_t>(host)].bounds;
            copy.local = offset(copy.bounds, -hb.left, -hb.top);
            next = static_cast<int>(nodes.size());
            nodes[static_cast<std::size_t>(host)].children.push_back(next);
            nodes.push_back(std::move(copy));
            toView[static_cast<std::size_t>(a)] = next;
        } else if (next < 0) {
            next = host;
        }
        for (const int c : accessibility[static_cast<std::size_t>(a)].children) graft(c, next);
    };
    for (const int r : accessibilityRoots) graft(r, -1);

    finalizeTree(nodes, {root});
    return matched;
}

void finalizeTree(std::vector<LayoutNode>& nodes, const std::vector<int>& roots) {
    const std::function<void(int, int, bool)> visit = [&](int i, int depth, bool parentShown) {
        LayoutNode& node = nodes[static_cast<std::size_t>(i)];
        node.depth = depth;
        node.shown = parentShown && node.visibility == ViewVisibility::Visible;
        const bool shown = node.shown;
        const std::vector<int> children = node.children;
        for (const int c : children) visit(c, depth + 1, shown);
    };
    for (const int r : roots) {
        if (r >= 0 && r < static_cast<int>(nodes.size())) visit(r, 0, true);
    }
}

// --- hit testing ----------------------------------------------------------------------

namespace {

struct Hit {
    int node = -1;
    bool substantive = false;
};

// "Substantive" = something a person would point at: it draws, takes input,
// or says something. An empty full-screen container stacked on top (a
// FragmentContainerView, an overlay host) must not swallow every click.
Hit hitNode(const std::vector<LayoutNode>& nodes, int index, int x, int y, bool includeHidden) {
    const LayoutNode& node = nodes[static_cast<std::size_t>(index)];
    if (!includeHidden && !node.shown) return {};
    if (!node.bounds.contains(x, y)) return {};
    Hit fallback;
    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        const Hit hit = hitNode(nodes, *it, x, y, includeHidden);
        if (hit.substantive) return hit;
        if (hit.node >= 0 && fallback.node < 0) fallback = hit;
    }
    const bool substantive = node.draws || node.clickable || node.longClickable ||
                             !isBlank(node.text) || !isBlank(node.contentDescription);
    // Within this branch, the deepest view under the point answers; whether
    // the branch has content decides it against its siblings.
    return {fallback.node >= 0 ? fallback.node : index, substantive || fallback.substantive};
}

}  // namespace

int hitTest(const std::vector<LayoutNode>& nodes, const std::vector<int>& roots, int x, int y,
            bool includeHidden) {
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        if (*it < 0) continue;
        const Hit hit = hitNode(nodes, *it, x, y, includeHidden);
        if (hit.node >= 0) return hit.node;
    }
    return -1;
}

int findCorresponding(const LayoutSnapshot& from, int node, const LayoutSnapshot& to) {
    if (node < 0 || node >= static_cast<int>(from.nodes.size())) return -1;
    const LayoutNode& old = from.nodes[static_cast<std::size_t>(node)];
    int fallback = -1;
    for (int i = 0; i < static_cast<int>(to.nodes.size()); ++i) {
        const LayoutNode& n = to.nodes[static_cast<std::size_t>(i)];
        if (n.origin != old.origin) continue;
        if (!old.hash.empty() && n.hash == old.hash) return i;
        if (fallback < 0 && n.className == old.className && n.resourceId == old.resourceId &&
            n.bounds == old.bounds) {
            fallback = i;
        }
    }
    return fallback;
}

// --- audit ----------------------------------------------------------------------------

const char* ruleName(LayoutIssue::Rule rule) noexcept {
    switch (rule) {
        case LayoutIssue::Rule::SmallTouchTarget:
            return "Small touch target";
        case LayoutIssue::Rule::MissingLabel:
            return "No accessible label";
        case LayoutIssue::Rule::RedundantWrapper:
            return "Redundant layout";
        case LayoutIssue::Rule::HeavyGoneSubtree:
            return "Hidden views still inflated";
        case LayoutIssue::Rule::DeepHierarchy:
            return "Deep hierarchy";
    }
    return "";
}

std::vector<LayoutIssue> auditLayout(const LayoutSnapshot& snapshot,
                                     const std::vector<int>& roots) {
    std::vector<LayoutIssue> issues;
    const auto& nodes = snapshot.nodes;
    const LayoutRect screen{0, 0, snapshot.screenWidth, snapshot.screenHeight};
    const auto onScreen = [&](const LayoutRect& r) {
        if (screen.empty()) return true;
        return r.right > screen.left && r.left < screen.right && r.bottom > screen.top &&
               r.top < screen.bottom;
    };

    int contentDepth = -1;
    int deepest = -1;
    const std::function<void(int, bool)> visit = [&](int i, bool inContent) {
        const LayoutNode& node = nodes[static_cast<std::size_t>(i)];
        if (node.resourceId == "android:id/content") {
            inContent = true;
            contentDepth = node.depth;
        }
        const bool isParentShown =
            node.parent < 0 || nodes[static_cast<std::size_t>(node.parent)].shown;

        if (node.shown && node.enabled && (node.clickable || node.longClickable) &&
            !node.bounds.empty() && onScreen(node.bounds)) {
            const float w = snapshot.dp(node.bounds.width());
            const float h = snapshot.dp(node.bounds.height());
            if (w < static_cast<float>(kMinTouchDp) - 0.5F ||
                h < static_cast<float>(kMinTouchDp) - 0.5F) {
                issues.push_back({LayoutIssue::Severity::Warning,
                                  LayoutIssue::Rule::SmallTouchTarget, i,
                                  format("%.0f x %.0f dp; aim for at least %d x %d dp",
                                         static_cast<double>(w), static_cast<double>(h),
                                         kMinTouchDp, kMinTouchDp)});
            }
            if (node.hasAccessibility && !node.password && !subtreeHasLabel(nodes, i)) {
                issues.push_back({LayoutIssue::Severity::Warning,
                                  LayoutIssue::Rule::MissingLabel, i,
                                  "clickable, but neither it nor its children have text or a "
                                  "content description for a screen reader to announce"});
            }
        }

        static constexpr std::string_view kPlainLayouts[] = {
            "android.widget.FrameLayout", "android.widget.LinearLayout",
            "android.widget.RelativeLayout",
            "androidx.constraintlayout.widget.ConstraintLayout"};
        if (inContent && node.origin == LayoutNode::Origin::View && node.shown &&
            node.children.size() == 1 && node.resourceId.empty() && !node.draws &&
            !node.clickable && !node.focusable &&
            std::find(std::begin(kPlainLayouts), std::end(kPlainLayouts), node.className) !=
                std::end(kPlainLayouts)) {
            const LayoutNode& child = nodes[static_cast<std::size_t>(node.children.front())];
            if (child.shown && child.bounds == node.bounds) {
                issues.push_back(
                    {LayoutIssue::Severity::Info, LayoutIssue::Rule::RedundantWrapper, i,
                     "has one child with the same bounds and draws nothing itself; "
                     "it costs a measure/layout pass and can likely be removed"});
            }
        }

        if (node.visibility == ViewVisibility::Gone && isParentShown) {
            const int hidden = countDescendants(nodes, i);
            if (hidden >= kHeavyGoneDescendants) {
                issues.push_back(
                    {LayoutIssue::Severity::Info, LayoutIssue::Rule::HeavyGoneSubtree, i,
                     format("GONE, yet %d views below it were inflated; a ViewStub or "
                            "inflating on demand would skip them",
                            hidden)});
            }
        }

        if (inContent && node.shown && node.children.empty() &&
            (deepest < 0 || node.depth > nodes[static_cast<std::size_t>(deepest)].depth)) {
            deepest = i;
        }
        for (const int c : node.children) visit(c, inContent);
    };
    for (const int r : roots) {
        if (r >= 0) visit(r, false);
    }

    if (deepest >= 0 && contentDepth >= 0) {
        const int levels = nodes[static_cast<std::size_t>(deepest)].depth - contentDepth;
        if (levels > kDeepLevels) {
            issues.push_back({LayoutIssue::Severity::Warning, LayoutIssue::Rule::DeepHierarchy,
                              deepest,
                              format("%d levels below the content view; deep trees make every "
                                     "measure and layout pass slower",
                                     levels)});
        }
    }

    std::stable_sort(
        issues.begin(), issues.end(),
        [](const LayoutIssue& a, const LayoutIssue& b) { return a.severity > b.severity; });
    return issues;
}

// --- copy as ---------------------------------------------------------------------------

std::string espressoMatcher(const LayoutNode& node) {
    if (!node.resourceId.empty()) {
        const std::string_view id{node.resourceId};
        const std::string prefix = id.starts_with("android:") ? "android.R.id." : "R.id.";
        return "onView(withId(" + prefix + std::string{node.idName()} + "))";
    }
    if (!isBlank(node.text)) return "onView(withText(" + javaString(node.text) + "))";
    if (!isBlank(node.contentDescription)) {
        return "onView(withContentDescription(" + javaString(node.contentDescription) + "))";
    }
    return "onView(withClassName(endsWith(" + javaString(node.simpleName()) + ")))";
}

std::string uiAutomatorSelector(const LayoutNode& node) {
    if (!node.resourceId.empty())
        return "device.findObject(By.res(" + javaString(node.resourceId) + "))";
    if (!isBlank(node.text)) return "device.findObject(By.text(" + javaString(node.text) + "))";
    if (!isBlank(node.contentDescription)) {
        return "device.findObject(By.desc(" + javaString(node.contentDescription) + "))";
    }
    return "device.findObject(By.clazz(" + javaString(node.className) + "))";
}

std::string adbTapCommand(const LayoutNode& node) {
    return format("adb shell input tap %d %d", (node.bounds.left + node.bounds.right) / 2,
                  (node.bounds.top + node.bounds.bottom) / 2);
}

// --- capture -----------------------------------------------------------------------------

LayoutInspector::LayoutInspector(DeviceManager& devices) : devices_(devices) {}

void LayoutInspector::adopt(std::shared_ptr<const LayoutSnapshot> snapshot) {
    if (!snapshot || !snapshot->error.empty()) return;
    Focus& f = focus_;
    const auto previous = f.snapshot;
    const bool sameDevice = previous && previous->serial == snapshot->serial;
    int window = snapshot->primaryWindow;
    if (sameDevice && f.window >= 0 && f.window < static_cast<int>(previous->windows.size())) {
        // Stay on the window the user picked, if that activity is still there.
        const std::string& component =
            previous->windows[static_cast<std::size_t>(f.window)].component;
        for (int w = 0; w < static_cast<int>(snapshot->windows.size()); ++w) {
            if (snapshot->windows[static_cast<std::size_t>(w)].component == component)
                window = w;
        }
    } else if (sameDevice && f.window < 0 && previous->primaryWindow >= 0) {
        window = -1;  // the user was looking at the accessibility tree
    }
    if (window < 0 && snapshot->accessibilityRoots.empty()) window = snapshot->primaryWindow;
    f.selected = sameDevice ? findCorresponding(*previous, f.selected, *snapshot) : -1;
    f.hovered = -1;
    f.window = window;
    f.snapshot = std::move(snapshot);
}

bool LayoutInspector::capture(
    ThreadPool& pool, Dispatcher& dispatcher, LayoutCaptureRequest request,
    std::function<void(std::shared_ptr<const LayoutSnapshot>)> onDone) {
    if (busy_->exchange(true)) return false;
    const std::string serial = devices_.selectedSerial();
    const std::uint64_t sequence = ++sequence_;

    pool.submit([this, serial, sequence, request = std::move(request),
                 onDone = std::move(onDone), busy = busy_, &dispatcher] {
        const auto started = std::chrono::steady_clock::now();
        auto snapshot = std::make_shared<LayoutSnapshot>();
        snapshot->serial = serial;
        snapshot->source = request.source;
        snapshot->sequence = sequence;

        const bool wantViews = request.source != LayoutSource::Accessibility;
        const bool wantA11y = request.source != LayoutSource::Views;
        const bool wantScreencap = request.screenshot && !request.frame;

        // One round trip. uiautomator is the slow part (2-3 s), so it runs in
        // the background while the rest is collected, and is waited for last.
        std::vector<std::string> commands;
        std::string script;
        const auto add = [&](std::string command) {
            if (!commands.empty()) {
                script += "echo ";
                script += kSentinel;
                script += "; ";
            }
            script += command;
            script += "; ";
            commands.push_back(std::move(command));
        };
        if (wantA11y) {
            script += format("rm -f %s %s; uiautomator dump %s >%s 2>&1 & ", kDumpFile,
                             kDumpLog, kDumpFile, kDumpLog);
        }
        add(wantViews ? "dumpsys activity top 2>/dev/null" : "true");
        add("dumpsys window windows 2>/dev/null | grep -E '^  Window #|Frames:|mFrame='");
        add("dumpsys window displays 2>/dev/null | grep -E 'Display: mDisplayId|cur='");
        add("wm density 2>/dev/null");
        add(wantScreencap
                ? "if command -v gzip >/dev/null; then screencap 2>/dev/null | gzip -1 | "
                  "base64 -w0; else screencap 2>/dev/null | base64 -w0; fi"
                : "true");
        add(wantA11y ? format("wait; cat %s 2>/dev/null || cat %s; rm -f %s %s", kDumpFile,
                              kDumpLog, kDumpFile, kDumpLog)
                     : "true");

        auto output =
            serial.empty()
                ? Result<std::string>{makeError(ErrorKind::NotFound, "no device selected")}
                : devices_.adb().shell(serial, script, std::chrono::seconds{30});
        if (!output) {
            snapshot->error = output.error().message;
        } else {
            const auto parts = splitBatchOutput(output.value(), kSentinel, commands.size());
            snapshot->rawWindows = parts[1];
            if (auto size = parseDisplaySize(parts[2])) {
                snapshot->screenWidth = size->first;
                snapshot->screenHeight = size->second;
            }
            snapshot->densityDpi = parseDensity(parts[3]).value_or(160);

            if (wantViews) {
                snapshot->rawViews = parts[0];
                ViewDump dump = parseActivityTop(parts[0], parts[1]);
                snapshot->nodes = std::move(dump.nodes);
                snapshot->windows = std::move(dump.windows);
                for (int w = 0; w < static_cast<int>(snapshot->windows.size()); ++w) {
                    if (snapshot->windows[static_cast<std::size_t>(w)].resumed)
                        snapshot->primaryWindow = w;
                }
                if (snapshot->primaryWindow < 0 && !snapshot->windows.empty()) {
                    snapshot->primaryWindow = static_cast<int>(snapshot->windows.size()) - 1;
                    snapshot->notices.emplace_back(
                        "No activity is resumed -- the screen is probably off or locked, so "
                        "this is "
                        "the last layout the app drew. Wake the phone and capture again.");
                }
                if (snapshot->windows.empty()) {
                    snapshot->notices.emplace_back(
                        "dumpsys activity top printed no View hierarchy");
                }
            }

            if (wantA11y) {
                snapshot->rawAccessibility = parts[5];
                std::vector<LayoutNode> a11y;
                auto roots = parseAccessibilityXml(parts[5], a11y);
                if (!roots) {
                    snapshot->notices.push_back(
                        "No accessibility tree, so no text and no Compose nodes (" +
                        roots.error().message +
                        "). uiautomator fails while the screen animates, or when another "
                        "automation tool holds it; capture again.");
                } else {
                    int matched = 0;
                    if (snapshot->primaryWindow >= 0) {
                        matched = mergeAccessibility(
                            snapshot->nodes,
                            snapshot->windows[static_cast<std::size_t>(snapshot->primaryWindow)]
                                .root,
                            a11y, roots.value());
                    }
                    // The full accessibility tree is kept too: it covers the
                    // focused window even when that is a dialog or popup, which
                    // the activity's View dump does not include.
                    const int base = static_cast<int>(snapshot->nodes.size());
                    for (LayoutNode node : a11y) {
                        if (node.parent >= 0) node.parent += base;
                        for (int& c : node.children) c += base;
                        snapshot->nodes.push_back(std::move(node));
                    }
                    for (const int r : roots.value())
                        snapshot->accessibilityRoots.push_back(r + base);
                    if (wantViews && !a11y.empty() &&
                        matched * 10 < static_cast<int>(a11y.size()) * 3) {
                        snapshot->notices.emplace_back(
                            "Most of the accessibility tree did not line up with the "
                            "activity's "
                            "views: the focused window is probably a dialog, popup or another "
                            "app's overlay. Its nodes are under \"Accessibility tree\".");
                        if (snapshot->primaryWindow >= 0 && matched == 0)
                            snapshot->primaryWindow = -1;
                    }
                }
            }

            if (request.frame) {
                snapshot->screenshot = request.frame;
            } else if (wantScreencap) {
                const std::string& encoded = parts[4];
                auto bytes = base64Decode(encoded);
                Result<ScreenFrame> frame = makeError(ErrorKind::ParseFailure, "no screenshot");
                if (bytes && !bytes->empty()) {
                    if (looksLikeGzip(*bytes)) {
                        auto raw = gunzip(*bytes);
                        frame = raw ? parseScreencap(raw.value())
                                    : Result<ScreenFrame>{makeError(ErrorKind::ParseFailure,
                                                                    raw.error().message)};
                    } else {
                        frame = parseScreencap(*bytes);
                    }
                }
                if (frame) {
                    snapshot->screenshot =
                        std::make_shared<const ScreenFrame>(std::move(frame).value());
                } else {
                    snapshot->notices.push_back("No screenshot (" + frame.error().message +
                                                "); apps that block screenshots show nothing.");
                }
            }

            if (snapshot->screenWidth <= 0 && snapshot->screenshot) {
                snapshot->screenWidth = static_cast<int>(snapshot->screenshot->width);
                snapshot->screenHeight = static_cast<int>(snapshot->screenshot->height);
            }
            if (!wantViews && snapshot->accessibilityRoots.empty() && snapshot->error.empty()) {
                snapshot->error = snapshot->notices.empty() ? "uiautomator returned nothing"
                                                            : snapshot->notices.front();
                snapshot->notices.clear();
            }
        }

        snapshot->captureMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
        if (!snapshot->error.empty())
            EM_LOG_WARN(kCategory, "capture failed: " + snapshot->error);
        for (const auto& notice : snapshot->notices) EM_LOG_INFO(kCategory, notice);

        std::shared_ptr<const LayoutSnapshot> done = std::move(snapshot);
        busy->store(false);
        dispatcher.post([onDone, done] { onDone(done); });
    });
    return true;
}

}  // namespace em
