#include "ui/panels/LayoutInspectorPanel.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <functional>

#include <imgui_stdlib.h>

#include <GLFW/glfw3.h>  // the platform GL 1.x declarations, as in Screenshot.cpp

#include "adb/DeviceManager.h"
#include "app/Theme.h"
#include "collect/FileSystemBrowser.h"
#include "collect/ScreenMirror.h"
#include "core/IniFile.h"
#include "core/PngWriter.h"
#include "core/StringUtil.h"
#include "core/TaskQueue.h"
#include "ui/Notifications.h"
#include "ui/Widgets.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F  // GL 1.2; Windows' gl.h stops at 1.1
#endif

namespace em {
namespace {

constexpr double kLiveSeconds = 1.0;  // pause between live captures
constexpr const char* kSourceKey = "layout.source";
constexpr const char* kShowHiddenKey = "layout.show_hidden";
constexpr const char* kShowTreeKey = "layout.show_tree";
constexpr const char* kMode3DKey = "layout.3d";

constexpr const char* kSourceNames[] = {"Merged", "Views only", "Accessibility only"};

ImU32 color(ImVec4 c, float alpha = 1.0F) {
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    return format("%04d%02d%02d-%02d%02d%02d", parts.tm_year + 1900, parts.tm_mon + 1,
                  parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
}

std::string nodeKey(const LayoutNode& n) {
    if (!n.hash.empty()) return n.hash;
    return format("%s|%s|%d,%d,%d,%d", n.className.c_str(), n.resourceId.c_str(), n.bounds.left,
                  n.bounds.top, n.bounds.right, n.bounds.bottom);
}

std::string shortened(std::string_view s, std::size_t max) {
    std::string out;
    for (const char c : s) {
        out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    if (out.size() > max) {
        std::size_t cut = max;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;  // UTF-8
        out.resize(cut);
        out += "...";
    }
    return out;
}

const char* visibilityName(ViewVisibility v) {
    switch (v) {
        case ViewVisibility::Visible:
            return "VISIBLE";
        case ViewVisibility::Invisible:
            return "INVISIBLE";
        case ViewVisibility::Gone:
            return "GONE";
    }
    return "";
}

std::string flagList(const LayoutNode& n) {
    std::vector<std::string_view> on;
    if (n.clickable) on.emplace_back("clickable");
    if (n.longClickable) on.emplace_back("long-clickable");
    if (n.contextClickable) on.emplace_back("context-clickable");
    if (n.focusable) on.emplace_back("focusable");
    if (n.focused) on.emplace_back("focused");
    if (n.scrollsVertically) on.emplace_back("scrolls vertically");
    if (n.scrollsHorizontally) on.emplace_back("scrolls horizontally");
    if (n.scrollable) on.emplace_back("scrollable");
    if (n.checkable) on.emplace_back(n.checked ? "checked" : "checkable");
    if (n.selected) on.emplace_back("selected");
    if (n.activated) on.emplace_back("activated");
    if (n.pressed) on.emplace_back("pressed");
    if (n.hovered) on.emplace_back("hovered");
    if (n.password) on.emplace_back("password");
    if (!n.enabled) on.emplace_back("disabled");
    if (n.draws) on.emplace_back("draws");
    if (n.layoutRequested) on.emplace_back("layout requested");
    if (n.dirty) on.emplace_back("dirty");
    std::string out;
    for (const auto f : on) {
        if (!out.empty()) out += ", ";
        out += f;
    }
    return out.empty() ? std::string{"none"} : out;
}

void jsonString(std::string& out, std::string_view s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += format("\\u%04x", static_cast<unsigned>(c));
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void jsonNode(std::string& out, const LayoutSnapshot& s, int index, int indent) {
    const LayoutNode& n = s.nodes[static_cast<std::size_t>(index)];
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
    out += pad + "{\"class\": ";
    jsonString(out, n.className);
    if (!n.resourceId.empty()) {
        out += ", \"id\": ";
        jsonString(out, n.resourceId);
    }
    out += format(", \"bounds\": [%d, %d, %d, %d], \"visibility\": \"%s\"", n.bounds.left,
                  n.bounds.top, n.bounds.right, n.bounds.bottom, visibilityName(n.visibility));
    if (n.origin == LayoutNode::Origin::Accessibility) out += ", \"accessibilityOnly\": true";
    if (!n.text.empty()) {
        out += ", \"text\": ";
        jsonString(out, n.text);
    }
    if (!n.contentDescription.empty()) {
        out += ", \"contentDescription\": ";
        jsonString(out, n.contentDescription);
    }
    if (n.clickable) out += ", \"clickable\": true";
    if (n.focusable) out += ", \"focusable\": true";
    if (!n.enabled) out += ", \"enabled\": false";
    if (n.children.empty()) {
        out += "}";
        return;
    }
    out += ", \"children\": [\n";
    for (std::size_t i = 0; i < n.children.size(); ++i) {
        jsonNode(out, s, n.children[i], indent + 1);
        out += i + 1 < n.children.size() ? ",\n" : "\n";
    }
    out += pad + "]}";
}

bool writeText(const std::filesystem::path& file, std::string_view text) {
    std::ofstream out{file, std::ios::binary};
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

float cross(ImVec2 a, ImVec2 b, ImVec2 p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

bool insideQuad(const ImVec2 (&q)[4], ImVec2 p) {
    bool positive = false;
    bool negative = false;
    for (int i = 0; i < 4; ++i) {
        const float c = cross(q[i], q[(i + 1) % 4], p);
        positive |= c > 0.0F;
        negative |= c < 0.0F;
    }
    return !(positive && negative);
}

// A distance label on a redline: "24 px · 8 dp".
void measureLabel(ImDrawList* draw, ImVec2 at, int px, const LayoutSnapshot& s,
                  ImU32 background) {
    const std::string text = format("%d px  %.0f dp", px, static_cast<double>(s.dp(px)));
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 min{at.x - size.x * 0.5F - 4.0F, at.y - size.y * 0.5F - 2.0F};
    const ImVec2 max{at.x + size.x * 0.5F + 4.0F, at.y + size.y * 0.5F + 2.0F};
    draw->AddRectFilled(min, max, background, 3.0F);
    draw->AddText({min.x + 4.0F, min.y + 2.0F}, IM_COL32_WHITE, text.c_str());
}

// Distances between `a` (the selection) and `b` (under the pointer), drawn the
// way design tools do: gaps when apart, insets when one contains the other.
void drawRedlines(ImDrawList* draw, const LayoutRect& a, const LayoutRect& b,
                  const LayoutSnapshot& s, const std::function<ImVec2(float, float)>& toScreen,
                  ImU32 lineColor) {
    const auto line = [&](float x1, float y1, float x2, float y2, int px) {
        if (px == 0) return;
        const ImVec2 p1 = toScreen(x1, y1);
        const ImVec2 p2 = toScreen(x2, y2);
        draw->AddLine(p1, p2, lineColor, 1.5F);
        const bool horizontal = y1 == y2;
        const ImVec2 tick = horizontal ? ImVec2{0.0F, 4.0F} : ImVec2{4.0F, 0.0F};
        draw->AddLine({p1.x - tick.x, p1.y - tick.y}, {p1.x + tick.x, p1.y + tick.y}, lineColor,
                      1.5F);
        draw->AddLine({p2.x - tick.x, p2.y - tick.y}, {p2.x + tick.x, p2.y + tick.y}, lineColor,
                      1.5F);
        measureLabel(draw, {(p1.x + p2.x) * 0.5F, (p1.y + p2.y) * 0.5F}, std::abs(px), s,
                     lineColor);
    };
    const auto mid = [](int lo1, int hi1, int lo2, int hi2, int fallbackLo, int fallbackHi) {
        const int lo = std::max(lo1, lo2);
        const int hi = std::min(hi1, hi2);
        return static_cast<float>(lo < hi ? (lo + hi) / 2 : (fallbackLo + fallbackHi) / 2);
    };

    const float cy = mid(a.top, a.bottom, b.top, b.bottom, a.top, a.bottom);
    if (a.right <= b.left) {
        line(static_cast<float>(a.right), cy, static_cast<float>(b.left), cy, b.left - a.right);
    } else if (b.right <= a.left) {
        line(static_cast<float>(b.right), cy, static_cast<float>(a.left), cy, a.left - b.right);
    } else {
        line(static_cast<float>(std::min(a.left, b.left)), cy,
             static_cast<float>(std::max(a.left, b.left)), cy, a.left - b.left);
        line(static_cast<float>(std::min(a.right, b.right)), cy,
             static_cast<float>(std::max(a.right, b.right)), cy, a.right - b.right);
    }

    const float cx = mid(a.left, a.right, b.left, b.right, a.left, a.right);
    if (a.bottom <= b.top) {
        line(cx, static_cast<float>(a.bottom), cx, static_cast<float>(b.top), b.top - a.bottom);
    } else if (b.bottom <= a.top) {
        line(cx, static_cast<float>(b.bottom), cx, static_cast<float>(a.top), a.top - b.bottom);
    } else {
        line(cx, static_cast<float>(std::min(a.top, b.top)), cx,
             static_cast<float>(std::max(a.top, b.top)), a.top - b.top);
        line(cx, static_cast<float>(std::min(a.bottom, b.bottom)), cx,
             static_cast<float>(std::max(a.bottom, b.bottom)), a.bottom - b.bottom);
    }
}

// "1080 x 2460" sized label above a rect.
void sizeTag(ImDrawList* draw, ImVec2 topLeft, const LayoutRect& r, const LayoutSnapshot& s,
             ImU32 background) {
    const std::string text = format("%.0f x %.0f dp", static_cast<double>(s.dp(r.width())),
                                    static_cast<double>(s.dp(r.height())));
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    ImVec2 min{topLeft.x, topLeft.y - size.y - 4.0F};
    if (min.y < ImGui::GetWindowPos().y) min.y = topLeft.y + 2.0F;
    draw->AddRectFilled(min, {min.x + size.x + 8.0F, min.y + size.y + 4.0F}, background, 3.0F);
    draw->AddText({min.x + 4.0F, min.y + 2.0F}, IM_COL32_WHITE, text.c_str());
}

}  // namespace

// --- capture -------------------------------------------------------------------------

bool LayoutInspectorPanel::requestCapture(AppContext& context, LayoutSource source) {
    if (!context.hasDevice()) return false;
    LayoutCaptureRequest request;
    request.source = source;
    // The Screen panel's live frame saves a 10 MB screencap transfer.
    if (context.mirror.running()) request.frame = context.mirror.latestFrame();
    LayoutInspector& layout = context.layout;
    return layout.capture(context.pool, context.dispatcher, std::move(request),
                          [&layout](std::shared_ptr<const LayoutSnapshot> snapshot) {
                              if (!snapshot->error.empty()) {
                                  notify::error("Layout capture failed", snapshot->error);
                              }
                              layout.adopt(std::move(snapshot));
                          });
}

void LayoutInspectorPanel::onDeviceChanged(AppContext& context) {
    context.layout.focus() = {};
    live_ = false;
    autoCaptured_ = false;
    snapshot_.reset();
    roots_.clear();
    rows_.clear();
    issues_.clear();
    collapsed_.clear();
}

void LayoutInspectorPanel::onShutdown(AppContext& context) {
    (void)context;
    releaseTexture();  // the GL context is still alive here
}

// --- state ------------------------------------------------------------------------------

void LayoutInspectorPanel::syncSnapshot() {
    // Called with the focus' snapshot/window already compared by draw().
    roots_ = snapshot_ ? snapshot_->rootsFor(window_) : std::vector<int>{};
    issues_ = snapshot_ ? auditLayout(*snapshot_, roots_) : std::vector<LayoutIssue>{};
    rowsDirty_ = true;
    if (snapshot_ && snapshot_->screenshot != textureFrame_) {
        textureFrame_ = snapshot_->screenshot;
        uploadScreenshot();
    }
}

void LayoutInspectorPanel::uploadScreenshot() {
    if (!textureFrame_ || textureFrame_->rgba.empty()) return;
    GLuint id = static_cast<GLuint>(texture_);
    if (id == 0) {
        glGenTextures(1, &id);
        texture_ = id;
    }
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(textureFrame_->width),
                 static_cast<GLsizei>(textureFrame_->height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 textureFrame_->rgba.data());
}

void LayoutInspectorPanel::releaseTexture() {
    if (texture_ != 0) {
        const auto id = static_cast<GLuint>(texture_);
        glDeleteTextures(1, &id);
        texture_ = 0;
    }
    textureFrame_.reset();
}

void LayoutInspectorPanel::rebuildRows() {
    rowsDirty_ = false;
    rows_.clear();
    if (!snapshot_) return;
    const auto& nodes = snapshot_->nodes;
    const bool filtering = !trim(filter_).empty();
    const std::string_view needle = trim(filter_);
    const auto matches = [&](const LayoutNode& n) {
        return containsIgnoreCase(n.className, needle) ||
               containsIgnoreCase(n.resourceId, needle) || containsIgnoreCase(n.text, needle) ||
               containsIgnoreCase(n.contentDescription, needle);
    };
    const std::function<bool(int, int)> visit = [&](int i, int depth) {
        const LayoutNode& n = nodes[static_cast<std::size_t>(i)];
        if (!showHidden_ && !n.shown) return false;
        const bool self = filtering && matches(n);
        const std::size_t at = rows_.size();
        rows_.push_back({i, depth, self});
        bool any = false;
        if (filtering || !collapsed_.contains(nodeKey(n))) {
            for (const int c : n.children) any |= visit(c, depth + 1);
        }
        if (filtering && !self && !any) {
            rows_.resize(at);
            return false;
        }
        return true;
    };
    for (const int r : roots_) visit(r, 0);

    // The rows are drawn by hand, so ImGui cannot tell how far they reach. Measure
    // the widest one here, for the horizontal scroll range.
    const float indent = ImGui::GetFontSize() * 0.9F;
    const float arrowW = ImGui::GetFontSize();
    treeWidth_ = 0.0F;
    for (const Row& row : rows_) {
        const LayoutNode& n = nodes[static_cast<std::size_t>(row.node)];
        const std::string_view name =
            n.simpleName().empty() ? std::string_view{"(view)"} : n.simpleName();
        const std::string id =
            n.resourceId.empty() ? std::string{} : "  #" + std::string{n.idName()};
        const std::string& label = !n.text.empty() ? n.text : n.contentDescription;
        const std::string quoted =
            label.empty() ? std::string{} : "  \"" + shortened(label, 40) + "\"";
        float w = static_cast<float>(row.depth) * indent + arrowW;
        if (n.origin == LayoutNode::Origin::Accessibility) {
            w += ImGui::CalcTextSize(ICON_FA_UNIVERSAL_ACCESS).x + 4.0F;
        }
        w += ImGui::CalcTextSize(name.data(), name.data() + name.size()).x;
        w += ImGui::CalcTextSize(id.c_str()).x;
        w += ImGui::CalcTextSize(quoted.c_str()).x;
        if (n.visibility != ViewVisibility::Visible) w += ImGui::CalcTextSize("  INVISIBLE").x;
        treeWidth_ = std::max(treeWidth_, w);
    }
}

bool LayoutInspectorPanel::visibleInTree(int node) const {
    return std::any_of(rows_.begin(), rows_.end(),
                       [node](const Row& r) { return r.node == node; });
}

void LayoutInspectorPanel::select(int node, bool reveal) {
    // Called with the focus already pointing at snapshot_.
    (void)reveal;
    if (!snapshot_ || node < 0) return;
    // Open every collapsed ancestor so the row can be shown.
    bool changed = false;
    for (int p = snapshot_->nodes[static_cast<std::size_t>(node)].parent; p >= 0;
         p = snapshot_->nodes[static_cast<std::size_t>(p)].parent) {
        changed |= collapsed_.erase(nodeKey(snapshot_->nodes[static_cast<std::size_t>(p)])) > 0;
    }
    if (changed) rowsDirty_ = true;
}

// --- draw -----------------------------------------------------------------------------------

void LayoutInspectorPanel::draw(AppContext& context) {
    if (!prefsLoaded_) {
        prefsLoaded_ = true;
        source_ = static_cast<LayoutSource>(
            std::clamp<long long>(context.prefs.getInt(kSourceKey, 0), 0, 2));
        showHidden_ = context.prefs.getBool(kShowHiddenKey, false);
        showTree_ = context.prefs.getBool(kShowTreeKey, true);
        mode3D_ = context.prefs.getBool(kMode3DKey, false);
    }
    if (!ImGui::Begin(windowTitle(), visibleFlag())) {
        ImGui::End();
        return;
    }

    LayoutInspector::Focus& focus = context.layout.focus();
    if (focus.snapshot != snapshot_ || focus.window != window_) {
        const bool fresh = focus.snapshot != snapshot_;
        snapshot_ = focus.snapshot;
        window_ = focus.window;
        syncSnapshot();
        if (fresh) nextLive_ = ImGui::GetTime() + kLiveSeconds;
        if (focus.selected >= 0) select(focus.selected, false);
    }
    if (focus.revealSelected && focus.selected >= 0) select(focus.selected, true);

    // The first time the panel is opened with a device, look right away.
    if (!autoCaptured_ && !focus.snapshot && context.hasDevice() && !context.layout.busy()) {
        autoCaptured_ = requestCapture(context, source_);
    }
    if (live_ && !context.layout.busy() && ImGui::GetTime() >= nextLive_) {
        requestCapture(context, source_);
    }
    if (ImGui::Shortcut(ImGuiKey_F5,
                        ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteUnlessBgFocused) &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        requestCapture(context, source_);
    }

    drawToolbar(context);
    ImGui::Separator();

    if (!snapshot_) {
        ImGui::Spacing();
        if (context.layout.busy()) {
            ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  Capturing the layout...");
        } else if (!context.hasDevice()) {
            ImGui::TextDisabled("Connect a device to inspect its layout.");
        } else {
            ImGui::TextDisabled(
                ICON_FA_SITEMAP
                "  Press Capture (F5) to inspect what is on the phone's screen.");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::palette().muted);
            ImGui::TextWrapped(
                "Works on any app, release builds included -- nothing to install, no "
                "debuggable "
                "flag. Views come from `dumpsys activity top`; text and Compose nodes from the "
                "accessibility tree. In the canvas: hover and click to pick a view, hold Alt "
                "to "
                "measure from the selection to the view under the pointer, scroll to zoom, "
                "right-drag to pan. The Screen panel's Inspect button picks views on the live "
                "screen.");
            ImGui::PopStyleColor();
        }
        ImGui::End();
        return;
    }
    if (rowsDirty_) rebuildRows();

    // The Layout panel owns hover while the pointer is over it; the Screen
    // panel shows the hovered view as an overlay.
    if (!focus.pickOnScreen) focus.hovered = -1;

    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("##layout", showTree_ ? 3 : 2, flags,
                          ImGui::GetContentRegionAvail())) {
        if (showTree_) {
            ImGui::TableSetupColumn("tree", ImGuiTableColumnFlags_WidthStretch, 0.30F);
        }
        ImGui::TableSetupColumn("canvas", ImGuiTableColumnFlags_WidthStretch,
                                showTree_ ? 0.44F : 0.60F);
        ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthStretch,
                                showTree_ ? 0.26F : 0.40F);
        ImGui::TableNextRow();
        if (showTree_) {
            ImGui::TableNextColumn();
            drawTree(context);
        }
        ImGui::TableNextColumn();
        drawCanvas(context);
        ImGui::TableNextColumn();
        if (ImGui::BeginTabBar("##details")) {
            if (ImGui::BeginTabItem(ICON_FA_SLIDERS "  Properties")) {
                drawProperties(context);
                ImGui::EndTabItem();
            }
            int warnings = 0;
            for (const auto& issue : issues_)
                warnings += issue.severity == LayoutIssue::Severity::Warning;
            const std::string auditLabel =
                issues_.empty()
                    ? std::string{ICON_FA_LIST_CHECK "  Audit###audit"}
                    : format(ICON_FA_LIST_CHECK "  Audit (%zu)###audit", issues_.size());
            if (warnings > 0) ImGui::PushStyleColor(ImGuiCol_Text, theme::palette().warning);
            const bool open = ImGui::BeginTabItem(auditLabel.c_str());
            if (warnings > 0) ImGui::PopStyleColor();
            if (open) {
                drawAudit(context);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndTable();
    }
    focus.revealSelected = false;
    ImGui::End();
}

void LayoutInspectorPanel::drawToolbar(AppContext& context) {
    const auto& palette = theme::palette();
    LayoutInspector::Focus& focus = context.layout.focus();
    const bool busy = context.layout.busy();

    ImGui::BeginDisabled(busy || !context.hasDevice());
    if (ImGui::Button(busy ? ICON_FA_HOURGLASS_HALF "  Capturing###capture"
                           : ICON_FA_CAMERA "  Capture###capture")) {
        requestCapture(context, source_);
    }
    ImGui::EndDisabled();
    widgets::termTooltip("Capture  (F5)",
                         "Read the View hierarchy, the accessibility tree and a screenshot of "
                         "what is on screen, in one adb round trip.");

    widgets::sameLineOrWrap(widgets::iconButtonWidth(ICON_FA_TOWER_BROADCAST));
    ImGui::BeginDisabled(!context.hasDevice());
    if (widgets::toggleButton(
            ICON_FA_TOWER_BROADCAST, &live_,
            "Live: capture again a second after each capture lands, keeping the "
            "selection. \"Views only\" is quickest (well under a second); merged "
            "waits 2-3 s for the accessibility tree.") &&
        live_) {
        nextLive_ = 0.0;
    }
    ImGui::EndDisabled();

    widgets::sameLineOrWrap(150.0F);
    ImGui::SetNextItemWidth(150.0F);
    int source = static_cast<int>(source_);
    if (ImGui::Combo("##source", &source, kSourceNames, IM_ARRAYSIZE(kSourceNames))) {
        source_ = static_cast<LayoutSource>(source);
        context.prefs.setInt(kSourceKey, source);
    }
    widgets::termTooltip(
        "Source",
        "Merged: every View, with the text, descriptions and Compose/WebView nodes "
        "of the accessibility tree laid onto it (2-3 s).\nViews only: `dumpsys "
        "activity top`, fast, but no text and Compose is one opaque view.\n"
        "Accessibility only: `uiautomator dump`; also covers dialogs and other "
        "apps' windows.");

    if (snapshot_) {
        // Which tree to show: each visible activity, and the accessibility tree.
        std::string current = "Accessibility tree";
        if (window_ >= 0 && window_ < static_cast<int>(snapshot_->windows.size())) {
            current = snapshot_->windows[static_cast<std::size_t>(window_)].title;
        }
        widgets::sameLineOrWrap(190.0F);
        ImGui::SetNextItemWidth(190.0F);
        if (ImGui::BeginCombo("##window", current.c_str())) {
            for (int w = 0; w < static_cast<int>(snapshot_->windows.size()); ++w) {
                const LayoutWindow& win = snapshot_->windows[static_cast<std::size_t>(w)];
                const std::string label = (win.resumed ? ICON_FA_CIRCLE_CHECK "  " : "      ") +
                                          win.title + "##" + std::to_string(w);
                if (ImGui::Selectable(label.c_str(), w == window_)) {
                    focus.window = w;
                    focus.selected = -1;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", win.component.c_str());
            }
            if (!snapshot_->accessibilityRoots.empty() &&
                ImGui::Selectable(ICON_FA_UNIVERSAL_ACCESS "  Accessibility tree",
                                  window_ < 0)) {
                focus.window = -1;
                focus.selected = -1;
            }
            ImGui::EndCombo();
        }
        widgets::termTooltip(
            "Window",
            "Every visible activity has its own tree; the resumed one is ticked. "
            "\"Accessibility tree\" is the focused window as uiautomator sees it -- "
            "the one to use for dialogs, popups and system UI.");
    }

    const float iconW = widgets::iconButtonWidth(ICON_FA_CUBES);
    widgets::sameLineOrWrap(iconW * 4.0F);
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (widgets::toggleButton(ICON_FA_CUBES, &mode3D_,
                              "3D: explode the hierarchy into layers -- drag to rotate. Shows "
                              "what overlaps what and where overdraw piles up.")) {
        context.prefs.setBool(kMode3DKey, mode3D_);
    }
    ImGui::SameLine();
    if (widgets::toggleButton(ICON_FA_EYE_SLASH, &showHidden_,
                              "Show INVISIBLE and GONE views too (dimmed).")) {
        rowsDirty_ = true;
        syncSnapshot();
        context.prefs.setBool(kShowHiddenKey, showHidden_);
    }
    ImGui::SameLine();
    widgets::toggleButton(ICON_FA_BORDER_ALL, &wireframe_, "Outline every view.");
    ImGui::SameLine();
    widgets::toggleButton(ICON_FA_IMAGE, &showScreenshot_, "Show the screenshot.");
    ImGui::SameLine();
    if (widgets::toggleButton(ICON_FA_LIST_UL, &showTree_, "Show the view tree.")) {
        context.prefs.setBool(kShowTreeKey, showTree_);
    }

    if (mode3D_) {
        widgets::sameLineOrWrap(120.0F);
        ImGui::SetNextItemWidth(120.0F);
        ImGui::SliderFloat("##spacing", &spacing_, 4.0F, 120.0F, "spacing %.0f");
    }
    widgets::sameLineOrWrap(iconW * 2.0F);
    if (ImGui::Button(ICON_FA_EXPAND)) {
        zoom_ = 1.0F;
        pan_ = {0.0F, 0.0F};
        yaw_ = mode3D_ ? -0.55F : 0.0F;
        pitch_ = mode3D_ ? 0.25F : 0.0F;
    }
    widgets::termTooltip("Reset view",
                         "Fit the screen in the canvas again (double-click works too).");
    ImGui::SameLine();
    ImGui::BeginDisabled(!snapshot_);
    if (ImGui::Button(ICON_FA_FILE_EXPORT)) exportSnapshot(context);
    ImGui::EndDisabled();
    widgets::termTooltip(
        "Export",
        "Save this capture to <workspace>/layouts: the screenshot, the tree as "
        "JSON, and the raw dumps.");

    if (snapshot_) {
        int views = 0;
        int labelled = 0;
        int accessibility = 0;
        const std::function<void(int)> count = [&](int i) {
            const LayoutNode& n = snapshot_->nodes[static_cast<std::size_t>(i)];
            ++views;
            labelled += n.hasAccessibility;
            accessibility += n.origin == LayoutNode::Origin::Accessibility;
            for (const int c : n.children) count(c);
        };
        for (const int r : roots_) count(r);
        const std::string status =
            format("%d nodes  %.0f dpi  %.1f s", views,
                   static_cast<double>(snapshot_->densityDpi), snapshot_->captureMs / 1000.0);
        widgets::sameLineOrWrap(ImGui::CalcTextSize(status.c_str()).x);
        ImGui::TextDisabled("%s", status.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%d with accessibility data, %d only in the accessibility tree "
                "(Compose, WebView, virtual views).\nCaptured from %s.",
                labelled, accessibility, snapshot_->serial.c_str());
        }
    }

    if (snapshot_) {
        for (const auto& notice : snapshot_->notices) {
            ImGui::PushStyleColor(ImGuiCol_Text, palette.warning);
            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION "  %s", notice.c_str());
            ImGui::PopStyleColor();
        }
    }
}

// --- tree -----------------------------------------------------------------------------------

void LayoutInspectorPanel::drawTree(AppContext& context) {
    const auto& palette = theme::palette();
    LayoutInspector::Focus& focus = context.layout.focus();
    const auto& nodes = snapshot_->nodes;

    if (focusFilter_) {
        ImGui::SetKeyboardFocusHere();
        focusFilter_ = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##filter",
                                 ICON_FA_MAGNIFYING_GLASS "  Filter: class, id, text  (Ctrl+F)",
                                 &filter_)) {
        rebuildRows();
    }

    ImGui::BeginChild("##tree", {0.0F, 0.0F}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, ImGuiInputFlags_RouteFocused))
        focusFilter_ = true;

    // Keyboard: arrows walk the tree like a file browser.
    int current = -1;
    for (int r = 0; r < static_cast<int>(rows_.size()); ++r) {
        if (rows_[static_cast<std::size_t>(r)].node == focus.selected) current = r;
    }
    if (ImGui::IsWindowFocused() && !rows_.empty()) {
        int next = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            next = std::min(current + 1, static_cast<int>(rows_.size()) - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) next = std::max(current - 1, 0);
        if (current >= 0) {
            const LayoutNode& n = nodes[static_cast<std::size_t>(focus.selected)];
            const bool open = !collapsed_.contains(nodeKey(n));
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                if (open && !n.children.empty()) {
                    collapsed_.insert(nodeKey(n));
                    rebuildRows();
                } else if (n.parent >= 0 && visibleInTree(n.parent)) {
                    focus.selected = n.parent;
                    focus.revealSelected = true;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !n.children.empty()) {
                if (!open) {
                    collapsed_.erase(nodeKey(n));
                    rebuildRows();
                } else {
                    next = current + 1;
                }
            }
        }
        if (next >= 0 && next < static_cast<int>(rows_.size())) {
            focus.selected = rows_[static_cast<std::size_t>(next)].node;
            focus.revealSelected = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) focus.selected = -1;
    }

    // Gives the scroll range its width; a zero-height item moves nothing down.
    ImGui::Dummy({treeWidth_, 0.0F});

    int revealRow = -1;
    if (focus.revealSelected) {
        for (int r = 0; r < static_cast<int>(rows_.size()); ++r) {
            if (rows_[static_cast<std::size_t>(r)].node == focus.selected) revealRow = r;
        }
    }

    const float indent = ImGui::GetFontSize() * 0.9F;
    const float arrowW = ImGui::GetFontSize();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 mutedColor = color(palette.muted);
    const ImU32 idColor = color(palette.accent);
    const ImU32 textValueColor = color(palette.good);
    const ImU32 matchColor = color(palette.warning, 0.25F);
    bool toggled = false;

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows_.size()));
    if (revealRow >= 0) clipper.IncludeItemByIndex(revealRow);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const Row& row = rows_[static_cast<std::size_t>(r)];
            const LayoutNode& n = nodes[static_cast<std::size_t>(row.node)];
            ImGui::PushID(row.node);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const bool selected = focus.selected == row.node;
            if (ImGui::Selectable("##row", selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                const float arrowX = start.x + static_cast<float>(row.depth) * indent;
                const bool onArrow = ImGui::GetIO().MousePos.x >= arrowX &&
                                     ImGui::GetIO().MousePos.x < arrowX + arrowW;
                if ((onArrow || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) &&
                    !n.children.empty()) {
                    const std::string key = nodeKey(n);
                    if (!collapsed_.erase(key)) collapsed_.insert(key);
                    toggled = true;
                }
                focus.selected = row.node;
            }
            if (ImGui::IsItemHovered()) {
                focus.hovered = row.node;
                if (!n.resourceId.empty() || !n.text.empty()) {
                    ImGui::SetItemTooltip("%s%s%s%s%s", n.className.c_str(),
                                          n.resourceId.empty() ? "" : "\n",
                                          n.resourceId.c_str(), n.text.empty() ? "" : "\n",
                                          shortened(n.text, 200).c_str());
                }
            }
            if (ImGui::BeginPopupContextItem("##ctx")) {
                focus.selected = row.node;
                if (ImGui::MenuItem(ICON_FA_COPY "  Copy as Espresso"))
                    ImGui::SetClipboardText(espressoMatcher(n).c_str());
                if (ImGui::MenuItem(ICON_FA_COPY "  Copy as UiAutomator"))
                    ImGui::SetClipboardText(uiAutomatorSelector(n).c_str());
                if (ImGui::MenuItem(ICON_FA_COPY "  Copy adb tap"))
                    ImGui::SetClipboardText(adbTapCommand(n).c_str());
                if (!n.resourceId.empty() &&
                    ImGui::MenuItem(ICON_FA_COPY "  Copy resource id")) {
                    ImGui::SetClipboardText(n.resourceId.c_str());
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_HAND_POINTER "  Tap on device"))
                    tapOnDevice(context, row.node);
                ImGui::EndPopup();
            }
            if (r == revealRow) ImGui::SetScrollHereY(0.4F);

            // Contents, drawn over the selectable.
            const float lineH = ImGui::GetTextLineHeight();
            float x = start.x + static_cast<float>(row.depth) * indent;
            const float y = start.y;
            if (row.match) {
                draw->AddRectFilled({x, y}, {x + ImGui::GetContentRegionAvail().x, y + lineH},
                                    matchColor);
            }
            if (!n.children.empty()) {
                const bool open = !trim(filter_).empty() || !collapsed_.contains(nodeKey(n));
                draw->AddText({x, y}, mutedColor,
                              open ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT);
            }
            x += arrowW;
            const bool dim = !n.shown;
            if (n.origin == LayoutNode::Origin::Accessibility) {
                draw->AddText({x, y}, idColor, ICON_FA_UNIVERSAL_ACCESS);
                x += ImGui::CalcTextSize(ICON_FA_UNIVERSAL_ACCESS).x + 4.0F;
            }
            const std::string_view name =
                n.simpleName().empty() ? std::string_view{"(view)"} : n.simpleName();
            draw->AddText(nullptr, 0.0F, {x, y}, dim ? mutedColor : textColor, name.data(),
                          name.data() + name.size());
            x += ImGui::CalcTextSize(name.data(), name.data() + name.size()).x;
            if (!n.resourceId.empty()) {
                const std::string id = "  #" + std::string{n.idName()};
                draw->AddText({x, y}, dim ? mutedColor : idColor, id.c_str());
                x += ImGui::CalcTextSize(id.c_str()).x;
            }
            const std::string& label = !n.text.empty() ? n.text : n.contentDescription;
            if (!label.empty()) {
                const std::string quoted = "  \"" + shortened(label, 40) + "\"";
                draw->AddText({x, y}, dim ? mutedColor : textValueColor, quoted.c_str());
                x += ImGui::CalcTextSize(quoted.c_str()).x;
            }
            if (n.visibility != ViewVisibility::Visible) {
                draw->AddText({x, y}, mutedColor,
                              n.visibility == ViewVisibility::Gone ? "  GONE" : "  INVISIBLE");
            }
            ImGui::PopID();
        }
    }
    if (rows_.empty())
        ImGui::TextDisabled(trim(filter_).empty() ? "No views." : "Nothing matches.");
    ImGui::EndChild();
    if (toggled) rebuildRows();
}

// --- canvas ---------------------------------------------------------------------------------

void LayoutInspectorPanel::drawCanvas(AppContext& context) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 20.0F || size.y < 20.0F) return;
    ImGui::BeginChild("##canvas", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y},
                        ImGui::GetColorU32(ImGuiCol_FrameBg));
    if (mode3D_) {
        drawCanvas3D(context, origin, size);
    } else {
        drawCanvas2D(context, origin, size);
    }
    ImGui::EndChild();
}

void LayoutInspectorPanel::drawCanvas2D(AppContext& context, ImVec2 origin, ImVec2 size) {
    const auto& palette = theme::palette();
    LayoutInspector::Focus& focus = context.layout.focus();
    const LayoutSnapshot& s = *snapshot_;
    const float screenW = static_cast<float>(s.screenWidth > 0 ? s.screenWidth : 1080);
    const float screenH = static_cast<float>(s.screenHeight > 0 ? s.screenHeight : 1920);
    const float fit = std::min(size.x / screenW, size.y / screenH) * 0.94F;
    const float scale = fit * zoom_;
    const ImVec2 base{origin.x + size.x * 0.5F + pan_.x - screenW * scale * 0.5F,
                      origin.y + size.y * 0.5F + pan_.y - screenH * scale * 0.5F};
    const auto toScreen = [&](float x, float y) {
        return ImVec2{base.x + x * scale, base.y + y * scale};
    };

    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##surface", size,
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && io.MouseWheel != 0.0F) {
        // Zoom about the pointer.
        const float before = zoom_;
        zoom_ = std::clamp(zoom_ * std::pow(1.15F, io.MouseWheel), 0.2F, 30.0F);
        const float k = zoom_ / before;
        const ImVec2 center{origin.x + size.x * 0.5F + pan_.x,
                            origin.y + size.y * 0.5F + pan_.y};
        pan_.x += (io.MousePos.x - center.x) * (1.0F - k);
        pan_.y += (io.MousePos.y - center.y) * (1.0F - k);
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        pan_.x += io.MouseDelta.x;
        pan_.y += io.MouseDelta.y;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        zoom_ = 1.0F;
        pan_ = {0.0F, 0.0F};
    }

    int hit = -1;
    if (hovered) {
        const int x = static_cast<int>(std::floor((io.MousePos.x - base.x) / scale));
        const int y = static_cast<int>(std::floor((io.MousePos.y - base.y) / scale));
        hit = hitTest(s.nodes, roots_, x, y, showHidden_);
        if (hit >= 0) focus.hovered = hit;
        if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <
                io.MouseDragThreshold * io.MouseDragThreshold) {
            focus.selected = hit;
            focus.revealSelected = hit >= 0;
        }
        ImGui::SetMouseCursor(hit >= 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow);
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    const ImVec2 screenMin = toScreen(0.0F, 0.0F);
    const ImVec2 screenMax = toScreen(screenW, screenH);
    if (showScreenshot_ && texture_ != 0 && s.screenshot) {
        draw->AddImage(ImTextureRef{static_cast<ImTextureID>(texture_)}, screenMin, screenMax);
    } else {
        draw->AddRectFilled(screenMin, screenMax, ImGui::GetColorU32(ImGuiCol_WindowBg));
    }
    draw->AddRect(screenMin, screenMax, ImGui::GetColorU32(ImGuiCol_Border));

    const auto rectOf = [&](const LayoutRect& r) {
        return std::pair{toScreen(static_cast<float>(r.left), static_cast<float>(r.top)),
                         toScreen(static_cast<float>(r.right), static_cast<float>(r.bottom))};
    };
    if (wireframe_) {
        // Off-screen views (other pager pages, scrolled-away rows) would only
        // be clutter around the phone.
        draw->PushClipRect(screenMin, screenMax, true);
        const ImU32 wire = color(palette.muted, 0.55F);
        const ImU32 wireHidden = color(palette.muted, 0.2F);
        const std::function<void(int)> outline = [&](int i) {
            const LayoutNode& n = s.nodes[static_cast<std::size_t>(i)];
            if (!showHidden_ && !n.shown) return;
            if (!n.bounds.empty()) {
                const auto [a, b] = rectOf(n.bounds);
                draw->AddRect(a, b, n.shown ? wire : wireHidden);
            }
            for (const int c : n.children) outline(c);
        };
        for (const int r : roots_) outline(r);
        draw->PopClipRect();
    }

    const ImU32 accent = color(palette.accent);
    const auto valid = [&](int i) { return i >= 0 && i < static_cast<int>(s.nodes.size()); };
    if (valid(focus.hovered) && focus.hovered != focus.selected) {
        const auto [a, b] = rectOf(s.nodes[static_cast<std::size_t>(focus.hovered)].bounds);
        draw->AddRectFilled(a, b, color(palette.accent, 0.18F));
        draw->AddRect(a, b, color(palette.accent, 0.8F), 0.0F, 0, 1.5F);
    }
    if (valid(focus.selected)) {
        const LayoutNode& sel = s.nodes[static_cast<std::size_t>(focus.selected)];
        const auto [a, b] = rectOf(sel.bounds);
        draw->AddRectFilled(a, b, color(palette.accent, 0.12F));
        draw->AddRect(a, b, accent, 0.0F, 0, 2.5F);
        const bool measuring = io.KeyAlt && hovered && valid(hit) && hit != focus.selected;
        if (measuring) {
            const LayoutRect& other = s.nodes[static_cast<std::size_t>(hit)].bounds;
            const auto [c, d] = rectOf(other);
            const ImU32 red = color(palette.bad);
            draw->AddRect(c, d, red, 0.0F, 0, 1.5F);
            drawRedlines(draw, sel.bounds, other, s, toScreen, red);
        } else {
            sizeTag(draw, a, sel.bounds, s, accent);
        }
    }
    draw->PopClipRect();

    if (hovered && valid(focus.selected) && !io.KeyAlt) {
        draw->AddText({origin.x + 8.0F, origin.y + size.y - ImGui::GetTextLineHeight() - 6.0F},
                      color(palette.muted), "Alt: measure to the view under the pointer");
    }
}

void LayoutInspectorPanel::drawCanvas3D(AppContext& context, ImVec2 origin, ImVec2 size) {
    const auto& palette = theme::palette();
    LayoutInspector::Focus& focus = context.layout.focus();
    const LayoutSnapshot& s = *snapshot_;
    const float screenW = static_cast<float>(s.screenWidth > 0 ? s.screenWidth : 1080);
    const float screenH = static_cast<float>(s.screenHeight > 0 ? s.screenHeight : 1920);

    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##surface3d", size,
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && io.MouseWheel != 0.0F) {
        zoom_ = std::clamp(zoom_ * std::pow(1.15F, io.MouseWheel), 0.2F, 30.0F);
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        yaw_ = std::clamp(yaw_ + io.MouseDelta.x * 0.008F, -1.45F, 1.45F);
        pitch_ = std::clamp(pitch_ - io.MouseDelta.y * 0.008F, -1.45F, 1.45F);
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        pan_.x += io.MouseDelta.x;
        pan_.y += io.MouseDelta.y;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        zoom_ = 1.0F;
        pan_ = {0.0F, 0.0F};
        yaw_ = -0.55F;
        pitch_ = 0.25F;
    }

    // Layers: each node at its depth below the root of the tree being shown.
    struct Layer {
        int node;
        int level;
    };
    std::vector<Layer> layers;
    int maxLevel = 0;
    const std::function<void(int, int)> collect = [&](int i, int level) {
        const LayoutNode& n = s.nodes[static_cast<std::size_t>(i)];
        if (!showHidden_ && !n.shown) return;
        if (!n.bounds.empty()) {
            layers.push_back({i, level});
            maxLevel = std::max(maxLevel, level);
        }
        for (const int c : n.children) collect(c, level + 1);
    };
    for (const int r : roots_) collect(r, 0);
    std::stable_sort(layers.begin(), layers.end(),
                     [](const Layer& a, const Layer& b) { return a.level < b.level; });

    const float fit = std::min(size.x / screenW, size.y / screenH) * 0.7F;
    const float scale = fit * zoom_;
    const float cy = std::cos(yaw_);
    const float sy = std::sin(yaw_);
    const float cp = std::cos(pitch_);
    const float sp = std::sin(pitch_);
    const ImVec2 center{origin.x + size.x * 0.5F + pan_.x, origin.y + size.y * 0.5F + pan_.y};
    // The stack is centred in depth so it rotates about its middle.
    const float midLevel = static_cast<float>(maxLevel) * 0.5F;
    const auto project = [&](float x, float y, int level) {
        const float X = (x - screenW * 0.5F) * scale;
        const float Y = (y - screenH * 0.5F) * scale;
        const float Z = (static_cast<float>(level) - midLevel) * spacing_ * zoom_;
        const float x1 = X * cy + Z * sy;
        const float z1 = -X * sy + Z * cy;
        const float y2 = Y * cp - z1 * sp;
        return ImVec2{center.x + x1, center.y + y2};
    };
    const auto quadOf = [&](const LayoutRect& r, int level, ImVec2(&q)[4]) {
        const auto l = static_cast<float>(r.left);
        const auto t = static_cast<float>(r.top);
        const auto rr = static_cast<float>(r.right);
        const auto b = static_cast<float>(r.bottom);
        q[0] = project(l, t, level);
        q[1] = project(rr, t, level);
        q[2] = project(rr, b, level);
        q[3] = project(l, b, level);
    };

    // Hit test from the front.
    int hit = -1;
    if (hovered) {
        for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
            ImVec2 q[4];
            quadOf(s.nodes[static_cast<std::size_t>(it->node)].bounds, it->level, q);
            if (insideQuad(q, io.MousePos)) {
                hit = it->node;
                break;
            }
        }
        if (hit >= 0) focus.hovered = hit;
        if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <
                io.MouseDragThreshold * io.MouseDragThreshold) {
            focus.selected = hit;
            focus.revealSelected = hit >= 0;
        }
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    const bool hasImage = showScreenshot_ && texture_ != 0 && s.screenshot;
    const ImTextureRef image{static_cast<ImTextureID>(texture_)};
    const ImU32 border = color(palette.muted, 0.6F);
    const ImU32 fill = color(palette.muted, 0.06F);
    for (const Layer& layer : layers) {
        const LayoutNode& n = s.nodes[static_cast<std::size_t>(layer.node)];
        ImVec2 q[4];
        quadOf(n.bounds, layer.level, q);
        const bool leaf = std::none_of(n.children.begin(), n.children.end(), [&](int c) {
            return s.nodes[static_cast<std::size_t>(c)].shown || showHidden_;
        });
        if (hasImage && n.shown && (layer.level == 0 || leaf || n.draws)) {
            // The part of the screenshot this view covers, at its own depth: the
            // root carries the whole screen, the rest float their own pixels.
            const ImVec2 uv0{
                std::clamp(static_cast<float>(n.bounds.left) / screenW, 0.0F, 1.0F),
                std::clamp(static_cast<float>(n.bounds.top) / screenH, 0.0F, 1.0F)};
            const ImVec2 uv1{
                std::clamp(static_cast<float>(n.bounds.right) / screenW, 0.0F, 1.0F),
                std::clamp(static_cast<float>(n.bounds.bottom) / screenH, 0.0F, 1.0F)};
            const ImU32 tint =
                layer.level == 0 || leaf ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 110);
            draw->AddImageQuad(image, q[0], q[1], q[2], q[3], uv0, {uv1.x, uv0.y}, uv1,
                               {uv0.x, uv1.y}, tint);
        } else {
            draw->AddQuadFilled(q[0], q[1], q[2], q[3], fill);
        }
        if (wireframe_ || layer.level == 0) {
            draw->AddQuad(q[0], q[1], q[2], q[3],
                          n.shown ? border : color(palette.muted, 0.25F));
        }
        if (layer.node == focus.hovered && layer.node != focus.selected) {
            draw->AddQuadFilled(q[0], q[1], q[2], q[3], color(palette.accent, 0.22F));
            draw->AddQuad(q[0], q[1], q[2], q[3], color(palette.accent), 1.5F);
        }
        if (layer.node == focus.selected) {
            draw->AddQuadFilled(q[0], q[1], q[2], q[3], color(palette.accent, 0.18F));
            draw->AddQuad(q[0], q[1], q[2], q[3], color(palette.accent), 3.0F);
        }
    }
    draw->PopClipRect();
    draw->AddText({origin.x + 8.0F, origin.y + size.y - ImGui::GetTextLineHeight() - 6.0F},
                  color(palette.muted),
                  "Drag to rotate, right-drag to pan, scroll to zoom, double-click to reset");
}

// --- details ---------------------------------------------------------------------------------

void LayoutInspectorPanel::drawProperties(AppContext& context) {
    const auto& palette = theme::palette();
    LayoutInspector::Focus& focus = context.layout.focus();
    const LayoutSnapshot& s = *snapshot_;
    ImGui::BeginChild("##props");
    if (focus.selected < 0 || focus.selected >= static_cast<int>(s.nodes.size())) {
        ImGui::Spacing();
        ImGui::TextDisabled("Select a view in the tree or on the canvas.");
        ImGui::EndChild();
        return;
    }
    const int index = focus.selected;
    const LayoutNode& n = s.nodes[static_cast<std::size_t>(index)];

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2F);
    ImGui::TextUnformatted(n.simpleName().data(),
                           n.simpleName().data() + n.simpleName().size());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
    ImGui::TextWrapped("%s", n.className.c_str());
    if (n.origin == LayoutNode::Origin::Accessibility) {
        ImGui::TextWrapped(
            ICON_FA_UNIVERSAL_ACCESS
            "  Only in the accessibility tree: a Compose node, a "
            "WebView element or a virtual view. Its class is the accessibility role.");
    }
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (ImGui::Button(ICON_FA_COPY "  Copy as")) ImGui::OpenPopup("##copyas");
    if (ImGui::BeginPopup("##copyas")) {
        const auto item = [](const char* label, const std::string& text) {
            if (ImGui::MenuItem(label)) {
                ImGui::SetClipboardText(text.c_str());
                notify::success("Copied", shortened(text, 80));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.c_str());
        };
        item("Espresso", espressoMatcher(n));
        item("UiAutomator", uiAutomatorSelector(n));
        item("adb tap", adbTapCommand(n));
        if (!n.resourceId.empty()) item("Resource id", n.resourceId);
        item("Bounds", format("[%d,%d][%d,%d]", n.bounds.left, n.bounds.top, n.bounds.right,
                              n.bounds.bottom));
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.hasDevice() || s.serial != context.selectedSerial);
    if (ImGui::Button(ICON_FA_HAND_POINTER "  Tap")) tapOnDevice(context, index);
    ImGui::EndDisabled();
    widgets::termTooltip("Tap on device",
                         "Tap the middle of this view on the phone (`input tap`).");
    ImGui::SameLine();
    ImGui::BeginDisabled(n.parent < 0);
    if (ImGui::Button(ICON_FA_ARROW_UP)) {
        focus.selected = n.parent;
        focus.revealSelected = true;
    }
    ImGui::EndDisabled();
    widgets::termTooltip("Parent", "Select the parent view.");

    ImGui::Spacing();
    if (ImGui::BeginTable("##props", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 6.5F);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        const auto row = [&](const char* key, const std::string& value,
                             ImVec4 tint = ImVec4{}) {
            if (value.empty()) return;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", key);
            ImGui::TableNextColumn();
            if (tint.w > 0.0F) ImGui::PushStyleColor(ImGuiCol_Text, tint);
            ImGui::TextWrapped("%s", value.c_str());
            if (tint.w > 0.0F) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Click to copy");
            }
            if (ImGui::IsItemClicked()) {
                ImGui::SetClipboardText(value.c_str());
                notify::success("Copied", shortened(value, 80));
            }
        };
        const LayoutRect& b = n.bounds;
        row("id", n.resourceId, palette.accent);
        row("text", n.password ? std::string{"(password)"} : n.text, palette.good);
        row("description", n.contentDescription, palette.good);
        row("size", format("%d x %d px\n%.1f x %.1f dp", b.width(), b.height(),
                           static_cast<double>(s.dp(b.width())),
                           static_cast<double>(s.dp(b.height()))));
        row("position",
            format("%d, %d px\n%.1f, %.1f dp", b.left, b.top, static_cast<double>(s.dp(b.left)),
                   static_cast<double>(s.dp(b.top))));
        row("bounds", format("[%d,%d][%d,%d]", b.left, b.top, b.right, b.bottom));
        if (n.parent >= 0 && n.origin == LayoutNode::Origin::View) {
            row("in parent", format("%d, %d px", n.local.left, n.local.top));
        }
        row("visibility", visibilityName(n.visibility),
            n.visibility == ViewVisibility::Visible ? ImVec4{} : palette.warning);
        if (!n.shown && n.visibility == ViewVisibility::Visible) {
            row("shown", "no -- an ancestor is not VISIBLE", palette.warning);
        }
        row("flags", flagList(n));
        if (n.shiftX != 0 || n.shiftY != 0) {
            row("scrolled",
                format("%+d, %+d px from its laid-out position (scroll or translation)",
                       n.shiftX, n.shiftY));
        }
        row("package", n.packageName);
        row("depth", std::to_string(n.depth));
        if (!n.children.empty()) row("children", std::to_string(n.children.size()));
        row("hash", n.hash);
        row("accessibility",
            n.origin == LayoutNode::Origin::View
                ? (n.hasAccessibility ? std::string{"matched"} : std::string{"not exposed"})
                : std::string{});
        ImGui::EndTable();
    }

    // Issues on this view.
    bool header = false;
    for (const auto& issue : issues_) {
        if (issue.node != index) continue;
        if (!header) {
            ImGui::Spacing();
            ImGui::SeparatorText("Audit");
            header = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, issue.severity == LayoutIssue::Severity::Warning
                                                 ? palette.warning
                                                 : palette.muted);
        ImGui::TextWrapped("%s  %s: %s",
                           issue.severity == LayoutIssue::Severity::Warning
                               ? ICON_FA_TRIANGLE_EXCLAMATION
                               : ICON_FA_CIRCLE_INFO,
                           ruleName(issue.rule), issue.message.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

void LayoutInspectorPanel::drawAudit(AppContext& context) {
    const auto& palette = theme::palette();
    LayoutInspector::Focus& focus = context.layout.focus();
    const LayoutSnapshot& s = *snapshot_;
    if (issues_.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, palette.good);
        ImGui::TextWrapped(ICON_FA_CIRCLE_CHECK
                           "  Nothing to flag: touch targets, labels, layout depth "
                           "and hidden subtrees all look fine.");
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
    ImGui::TextWrapped(
        "Checked: touch targets of 48 dp, labels for screen readers, wrapper layouts, GONE "
        "subtrees and depth. Click an issue to select its view.");
    ImGui::PopStyleColor();
    ImGui::BeginChild("##issues");
    for (std::size_t i = 0; i < issues_.size(); ++i) {
        const LayoutIssue& issue = issues_[i];
        const LayoutNode& n = s.nodes[static_cast<std::size_t>(issue.node)];
        ImGui::PushID(static_cast<int>(i));
        const bool warning = issue.severity == LayoutIssue::Severity::Warning;
        const std::string title =
            format("%s  %s", warning ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_CIRCLE_INFO,
                   ruleName(issue.rule));
        ImGui::PushStyleColor(ImGuiCol_Text, warning ? palette.warning : palette.muted);
        if (ImGui::Selectable(title.c_str(), focus.selected == issue.node)) {
            focus.selected = issue.node;
            focus.revealSelected = true;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) focus.hovered = issue.node;
        ImGui::Indent();
        const std::string who =
            std::string{n.simpleName()} +
            (n.resourceId.empty() ? std::string{} : "  #" + std::string{n.idName()}) +
            (n.text.empty() ? std::string{} : "  \"" + shortened(n.text, 30) + "\"");
        ImGui::TextUnformatted(who.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, palette.muted);
        ImGui::TextWrapped("%s", issue.message.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent();
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// --- actions --------------------------------------------------------------------------------

void LayoutInspectorPanel::tapOnDevice(AppContext& context, int node) {
    if (!snapshot_ || node < 0 || !context.hasDevice()) return;
    const LayoutRect& b = snapshot_->nodes[static_cast<std::size_t>(node)].bounds;
    const int x = (b.left + b.right) / 2;
    const int y = (b.top + b.bottom) / 2;
    const std::string serial = context.selectedSerial;
    DeviceManager& devices = context.devices;
    context.pool.submit([&devices, serial, x, y] {
        auto result = devices.adb().shell(serial, format("input tap %d %d", x, y),
                                          std::chrono::seconds{5});
        if (!result) notify::error("Tap failed", result.error().message);
    });
    // In live mode, look again once the app has reacted.
    nextLive_ = ImGui::GetTime() + 0.6;
}

void LayoutInspectorPanel::exportSnapshot(AppContext& context) {
    if (!snapshot_) return;
    std::string serial = snapshot_->serial;
    for (char& c : serial) {
        if (c == ':' || c == '/' || c == '\\') c = '_';  // network serials are host:port
    }
    const auto dir = context.workspaceDir / "layouts" / (serial + "-" + timestamp());
    auto snapshot = snapshot_;
    const std::vector<int> roots = roots_;
    Dispatcher& dispatcher = context.dispatcher;
    ThreadPool& pool = context.pool;
    pool.submit([snapshot, roots, dir, &dispatcher, &pool] {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        bool ok = !ec;
        std::string json = "{\n  \"serial\": ";
        jsonString(json, snapshot->serial);
        json += format(",\n  \"screen\": [%d, %d],\n  \"densityDpi\": %d,\n  \"roots\": [\n",
                       snapshot->screenWidth, snapshot->screenHeight, snapshot->densityDpi);
        for (std::size_t i = 0; i < roots.size(); ++i) {
            jsonNode(json, *snapshot, roots[i], 2);
            json += i + 1 < roots.size() ? ",\n" : "\n";
        }
        json += "  ]\n}\n";
        ok = ok && writeText(dir / "tree.json", json);
        if (!snapshot->rawViews.empty())
            ok = ok && writeText(dir / "views.txt", snapshot->rawViews);
        if (!snapshot->rawWindows.empty())
            ok = ok && writeText(dir / "windows.txt", snapshot->rawWindows);
        if (!snapshot->rawAccessibility.empty()) {
            ok = ok && writeText(dir / "accessibility.xml", snapshot->rawAccessibility);
        }
        if (snapshot->screenshot) {
            const auto& f = *snapshot->screenshot;
            ok = ok &&
                 static_cast<bool>(writePng(dir / "screenshot.png", f.rgba, f.width, f.height));
        }
        dispatcher.post([ok, dir, &pool] {
            if (!ok) {
                notify::error("Export failed", "Could not write to " + dir.string());
                return;
            }
            notify::postWithAction(
                notify::Kind::Success, "Layout exported", dir.filename().string(),
                ICON_FA_FOLDER_OPEN "  Show",
                [&pool, dir] { FileSystemBrowser::revealOnHost(pool, dir); });
        });
    });
}

}  // namespace em
