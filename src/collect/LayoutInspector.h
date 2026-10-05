#pragma once

// The on-screen View hierarchy of any app -- release builds included, no
// agent, no project, no debuggable flag.
//
// Two sources, captured together in ONE adb round trip:
//
//   * `dumpsys activity top`: every View of each visible activity, GONE ones
//     included, with its class, id, flags (visible/focusable/clickable/...)
//     and bounds relative to its parent. ~0.2 s. Knows nothing of text, and a
//     Compose UI is one opaque AndroidComposeView.
//   * `uiautomator dump`: the accessibility tree. Text, content description,
//     checked state -- and the Compose and WebView nodes the View tree cannot
//     see. 2-3 s, and fails while the screen animates.
//
// Merged, the View tree is the skeleton (it is complete and exact) and the
// accessibility nodes are laid onto it by bounds and id; nodes only
// accessibility knows about (Compose, WebView, virtual views) are grafted under
// the view that hosts them. Scrolled containers come out of the View dump at
// their unscrolled position; where an accessibility node pins a subtree down,
// the subtree is shifted to where it really is.
//
// Parsing is pure and lives here so it is unit tested against real dumps.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "adb/DeviceManager.h"
#include "collect/ScreenFrame.h"
#include "core/Result.h"
#include "core/TaskQueue.h"

namespace em {

struct LayoutRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] int width() const noexcept { return right - left; }
    [[nodiscard]] int height() const noexcept { return bottom - top; }
    [[nodiscard]] bool empty() const noexcept { return right <= left || bottom <= top; }
    [[nodiscard]] bool contains(int x, int y) const noexcept {
        return x >= left && x < right && y >= top && y < bottom;
    }
    [[nodiscard]] bool operator==(const LayoutRect&) const = default;
};

enum class ViewVisibility { Visible, Invisible, Gone };

struct LayoutNode {
    enum class Origin { View, Accessibility };
    Origin origin = Origin::View;

    int parent = -1;
    std::vector<int> children;
    int depth = 0;

    std::string className;   // fully qualified
    std::string hash;        // identity hash from View.toString, views only
    std::string resourceId;  // "com.example:id/name", package spelled out
    LayoutRect bounds;       // on screen, device pixels
    LayoutRect local;        // relative to the parent, as laid out (views only)

    ViewVisibility visibility = ViewVisibility::Visible;
    bool shown = true;  // this and every ancestor VISIBLE

    // View flags (View.toString), or the accessibility equivalents.
    bool focusable = false;
    bool enabled = true;
    bool draws = false;  // not WILL_NOT_DRAW: has a background or content
    bool scrollsHorizontally = false;
    bool scrollsVertically = false;
    bool clickable = false;
    bool longClickable = false;
    bool contextClickable = false;
    bool focused = false;
    bool selected = false;
    bool pressed = false;
    bool hovered = false;
    bool activated = false;
    bool layoutRequested = false;  // PFLAG_INVALIDATED ('I')
    bool dirty = false;            // PFLAG_DIRTY ('D')

    // From the accessibility tree.
    bool hasAccessibility = false;
    std::string text;
    std::string contentDescription;
    std::string packageName;
    bool checkable = false;
    bool checked = false;
    bool scrollable = false;
    bool password = false;

    // Pixels the merge moved this subtree by, because the View dump does not
    // include scroll offsets or translations.
    int shiftX = 0;
    int shiftY = 0;

    [[nodiscard]] std::string_view simpleName() const noexcept;
    [[nodiscard]] std::string_view idName() const noexcept;  // "name" of "pkg:id/name"
};

// One activity window from `dumpsys activity top`.
struct LayoutWindow {
    std::string component;  // "pkg/full.ClassName"
    std::string title;      // the DecorView's "[ClassName]"
    int pid = -1;
    bool resumed = false;
    int root = -1;          // index into the snapshot's nodes
    LayoutRect frame;       // where the window is on screen
};

enum class LayoutSource { Merged, Views, Accessibility };

struct LayoutSnapshot {
    std::vector<LayoutNode> nodes;
    std::vector<LayoutWindow> windows;  // views; one per visible activity
    int primaryWindow = -1;             // the resumed one, else the top one
    // For an Accessibility-only capture: the roots of the accessibility tree.
    std::vector<int> accessibilityRoots;

    LayoutSource source = LayoutSource::Merged;
    int screenWidth = 0;
    int screenHeight = 0;
    int densityDpi = 160;
    std::shared_ptr<const ScreenFrame> screenshot;

    std::string serial;
    std::string error;
    std::vector<std::string> notices;  // e.g. the accessibility dump failed
    double captureMs = 0.0;
    std::uint64_t sequence = 0;

    // The raw dumps, kept for export.
    std::string rawViews;
    std::string rawWindows;
    std::string rawAccessibility;

    [[nodiscard]] float dp(int pixels) const noexcept {
        return static_cast<float>(pixels) * 160.0F / static_cast<float>(densityDpi > 0 ? densityDpi : 160);
    }
    // Roots to show: the given window's root, or every accessibility root.
    [[nodiscard]] std::vector<int> rootsFor(int window) const;
    [[nodiscard]] std::string packageName() const;
};

// --- parsing, exposed for tests ---------------------------------------------

// One line of a View hierarchy dump, e.g.
//   android.widget.TextView{e418db3 V.ED..... ......ID 305,0-774,161 #7f0a0179 app:id/title}
// `local` gets the bounds; `bounds` is left for the caller to place on screen.
[[nodiscard]] std::optional<LayoutNode> parseViewLine(std::string_view line);

// Every "View Hierarchy:" section of `dumpsys activity top`. Window frames come
// from `dumpsys window windows` (its "Window #" and "Frames:" lines); without
// one, a window is assumed to cover the screen from 0,0.
struct ViewDump {
    std::vector<LayoutNode> nodes;
    std::vector<LayoutWindow> windows;
};
[[nodiscard]] ViewDump parseActivityTop(std::string_view activityTop, std::string_view windowFrames);

// "pkg/.Cls" -> "pkg/pkg.Cls"; anything else unchanged.
[[nodiscard]] std::string expandComponent(std::string_view component);

// Window title -> frame, from `dumpsys window windows | grep -E '^  Window #|Frames:'`.
[[nodiscard]] std::optional<LayoutRect> findWindowFrame(std::string_view windowFrames,
                                                        std::string_view component);

// `cur=WxH` of display 0 in `dumpsys window displays`, and `wm density`.
[[nodiscard]] std::optional<std::pair<int, int>> parseDisplaySize(std::string_view displays);
[[nodiscard]] std::optional<int> parseDensity(std::string_view wmDensity);

// uiautomator's XML. Nodes are appended to `out` (their parent/children
// indices point into it); returns the indices of the top-level nodes.
[[nodiscard]] Result<std::vector<int>> parseAccessibilityXml(std::string_view xml,
                                                             std::vector<LayoutNode>& out);

// Lays the accessibility nodes onto the View tree under `root` (see the file
// comment). Returns how many accessibility nodes found a view.
int mergeAccessibility(std::vector<LayoutNode>& nodes, int root,
                       const std::vector<LayoutNode>& accessibility,
                       const std::vector<int>& accessibilityRoots);

// Recomputes depth and `shown` below each root.
void finalizeTree(std::vector<LayoutNode>& nodes, const std::vector<int>& roots);

// The topmost node under (x, y) below `roots`: the deepest, and among siblings
// the one drawn last. -1 when nothing is there.
[[nodiscard]] int hitTest(const std::vector<LayoutNode>& nodes, const std::vector<int>& roots,
                          int x, int y, bool includeHidden);

// The node in `to` that is `node` of `from`: same identity hash for a view,
// else the same class, id and bounds. -1 if it is gone. Keeps the selection
// across re-captures.
[[nodiscard]] int findCorresponding(const LayoutSnapshot& from, int node, const LayoutSnapshot& to);

// --- audit -------------------------------------------------------------------

struct LayoutIssue {
    enum class Severity { Info, Warning };
    enum class Rule {
        SmallTouchTarget,   // clickable and under 48x48 dp
        MissingLabel,       // clickable, nothing for a screen reader to say
        RedundantWrapper,   // a plain layout with one child and the same bounds
        HeavyGoneSubtree,   // a GONE view still inflating many descendants
        DeepHierarchy,      // the deepest view is very deep
    };
    Severity severity = Severity::Info;
    Rule rule = Rule::SmallTouchTarget;
    int node = -1;
    std::string message;
};

[[nodiscard]] const char* ruleName(LayoutIssue::Rule rule) noexcept;
[[nodiscard]] std::vector<LayoutIssue> auditLayout(const LayoutSnapshot& snapshot,
                                                   const std::vector<int>& roots);

// --- "copy as" -----------------------------------------------------------------

[[nodiscard]] std::string espressoMatcher(const LayoutNode& node);
[[nodiscard]] std::string uiAutomatorSelector(const LayoutNode& node);
[[nodiscard]] std::string adbTapCommand(const LayoutNode& node);

// --- capture -------------------------------------------------------------------

struct LayoutCaptureRequest {
    LayoutSource source = LayoutSource::Merged;
    // Used as the screenshot when set (the Screen panel's live frame);
    // otherwise one is taken with screencap in the same round trip.
    std::shared_ptr<const ScreenFrame> frame;
    bool screenshot = true;
};

class LayoutInspector {
public:
    explicit LayoutInspector(DeviceManager& devices);

    // At most one capture runs at a time; returns false (and does nothing)
    // while one is in flight.
    bool capture(ThreadPool& pool, Dispatcher& dispatcher, LayoutCaptureRequest request,
                 std::function<void(std::shared_ptr<const LayoutSnapshot>)> onDone);
    [[nodiscard]] bool busy() const noexcept { return busy_->load(); }

    // UI-thread state shared by the Layout and Screen panels, so a view picked
    // on the live screen is selected in the tree and the other way round.
    struct Focus {
        std::shared_ptr<const LayoutSnapshot> snapshot;
        int window = -1;
        int selected = -1;
        int hovered = -1;
        bool pickOnScreen = false;  // clicks on the Screen panel pick views
        bool revealSelected = false;  // scroll the tree to the selection
    };
    [[nodiscard]] Focus& focus() noexcept { return focus_; }
    // Makes a finished capture the current one, carrying the window and the
    // selection over. UI thread. A failed capture keeps the previous tree.
    void adopt(std::shared_ptr<const LayoutSnapshot> snapshot);

private:
    DeviceManager& devices_;
    std::shared_ptr<std::atomic<bool>> busy_ = std::make_shared<std::atomic<bool>>(false);
    std::uint64_t sequence_ = 0;
    Focus focus_;
};

}  // namespace em
