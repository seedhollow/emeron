#include "TestHarness.h"

#include <string>

#include "collect/LayoutInspector.h"

using namespace em;

namespace {

// Shaped like `dumpsys activity top` on Android 14; the view lines are real.
constexpr const char* kActivityTop = R"(TASK 10:bitpit.launcher id=5 userId=0
  ACTIVITY com.other.app/.Background 1d2e3f4 pid=1111
    mResumed=false mStopped=true mFinished=false
    View Hierarchy:
      DecorView@aaaa[Background]
        android.widget.LinearLayout{bbbb V.E...... .......D 0,0-1080,2460}
    Looper (main, tid 2) {9f1e2d3}
  ACTIVITY bitpit.launcher/.ui.HomeActivity 8a1f3c2 pid=4321
    Local FragmentActivity 8a1f3c2 State:
    mResumed=true mStopped=false mFinished=false
    View Hierarchy:
      DecorView@bfcdbf5[HomeActivity]
        android.widget.LinearLayout{f5d50d7 V.E...... .......D 0,0-1080,2460}
          android.view.ViewStub{2c00beb G.E...... ......I. 0,0-0,0 #10201ca android:id/action_mode_bar_stub}
          android.widget.FrameLayout{aa01 V.E...... ........ 0,100-1080,2460 #1020002 android:id/content}
            bitpit.launcher.text.ClearFocusEditText{65270b VFED..CL. .F...... 64,21-813,148 #7f0a032f app:id/toolbar_text aid=1073741824}
            androidx.compose.ui.platform.ComposeView{c001 V.E...... ........ 0,400-1080,800 #7f0a0100 app:id/compose}
              androidx.compose.ui.platform.AndroidComposeView{c002 VFED..... ........ 0,0-1080,400}
            android.widget.ScrollView{5c01 VFED.V... ........ 0,1000-1080,2000 #7f0a0200 app:id/scroll}
              android.widget.LinearLayout{5c02 V.E...... ........ 0,0-1080,3000}
                android.widget.TextView{5c03 V.ED..C.. ........ 0,500-1080,650 #7f0a0201 app:id/row}
    Looper (main, tid 2) {1a2b3c4}
)";

constexpr const char* kWindows = R"(  Window #3 Window{1ae4e70 u0 bitpit.launcher/bitpit.launcher.ui.HomeActivity}:
    Frames: parent=[0,0][1080,2460] display=[0,0][1080,2460] frame=[0,50][1080,2460] last=[0,50][1080,2460] insetsChanged=false
  Window #4 Window{2bf5f81 u0 com.other.app/com.other.app.Background}:
    Frames: parent=[0,0][1080,2460] display=[0,0][1080,2460] frame=[0,0][1080,2460] last=[0,0][1080,2460] insetsChanged=false
)";

// The accessibility tree of the same screen: text, the Compose button the View
// dump cannot see, and the row 300 px higher because the ScrollView scrolled.
constexpr const char* kAccessibility = R"(<?xml version='1.0' encoding='UTF-8' standalone='yes' ?>
<hierarchy rotation="0">
<node index="0" text="" resource-id="" class="android.widget.FrameLayout" package="bitpit.launcher" content-desc="" checkable="false" checked="false" clickable="false" enabled="true" focusable="false" focused="false" scrollable="false" long-clickable="false" password="false" selected="false" bounds="[0,50][1080,2460]">
<node index="0" text="" resource-id="" class="android.widget.LinearLayout" package="bitpit.launcher" content-desc="" checkable="false" checked="false" clickable="false" enabled="true" focusable="false" focused="false" scrollable="false" long-clickable="false" password="false" selected="false" bounds="[0,50][1080,2460]">
<node index="0" text="" resource-id="android:id/content" class="android.widget.FrameLayout" package="bitpit.launcher" content-desc="" checkable="false" checked="false" clickable="false" enabled="true" focusable="false" focused="false" scrollable="false" long-clickable="false" password="false" selected="false" bounds="[0,150][1080,2460]">
<node index="0" text="Search apps &amp; &quot;more&quot;" resource-id="bitpit.launcher:id/toolbar_text" class="android.widget.EditText" package="bitpit.launcher" content-desc="" checkable="false" checked="false" clickable="true" enabled="true" focusable="true" focused="true" scrollable="false" long-clickable="true" password="false" selected="false" bounds="[64,171][813,298]" />
<node index="1" text="" resource-id="bitpit.launcher:id/compose" class="android.view.View" package="bitpit.launcher" content-desc="" checkable="false" checked="false" clickable="false" enabled="true" focusable="false" focused="false" scrollable="false" long-clickable="false" password="false" selected="false" bounds="[0,550][1080,950]">
<node index="0" text="" resource-id="" class="android.view.View" package="bitpit.launcher" content-desc="" checkable="false" checked="false" clickable="false" enabled="true" focusable="false" focused="false" scrollable="false" long-clickable="false" password="false" selected="false" bounds="[0,550][1080,950]">
<node index="0" text="Buy" resource-id="" class="android.widget.Button" package="bitpit.launcher" content-desc="" checkable="false" checked="false" clickable="true" enabled="true" focusable="true" focused="false" scrollable="false" long-clickable="false" password="false" selected="false" bounds="[40,600][400,700]" />
</node>
</node>
<node index="2" text="" resource-id="bitpit.launcher:id/scroll" class="android.widget.ScrollView" package="bitpit.launcher" content-desc="" checkable="false" checked="false" clickable="false" enabled="true" focusable="true" focused="false" scrollable="true" long-clickable="false" password="false" selected="false" bounds="[0,1150][1080,2150]">
<node index="0" text="Row" resource-id="bitpit.launcher:id/row" class="android.widget.TextView" package="bitpit.launcher" content-desc="" checkable="false" checked="false" clickable="true" enabled="true" focusable="true" focused="false" scrollable="false" long-clickable="false" password="false" selected="false" bounds="[0,1350][1080,1500]" />
</node>
</node>
</node>
</node>
</hierarchy>
)";

int findByClass(const std::vector<LayoutNode>& nodes, std::string_view cls, int from = 0) {
    for (int i = from; i < static_cast<int>(nodes.size()); ++i) {
        if (nodes[static_cast<std::size_t>(i)].className == cls) return i;
    }
    return -1;
}

int findById(const std::vector<LayoutNode>& nodes, std::string_view id) {
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        if (nodes[static_cast<std::size_t>(i)].resourceId == id) return i;
    }
    return -1;
}

}  // namespace

TEST_CASE("parseViewLine reads class, flags, bounds and id") {
    const auto node = parseViewLine(
        "bitpit.launcher.text.ClearFocusEditText{65270b VFED..CL. .F...... 64,21-813,148 #7f0a032f "
        "app:id/toolbar_text aid=1073741824}");
    REQUIRE(node.has_value());
    CHECK_EQ(node->className, std::string{"bitpit.launcher.text.ClearFocusEditText"});
    CHECK_EQ(node->simpleName(), std::string_view{"ClearFocusEditText"});
    CHECK_EQ(node->hash, std::string{"65270b"});
    CHECK(node->visibility == ViewVisibility::Visible);
    CHECK(node->focusable);
    CHECK(node->enabled);
    CHECK(node->draws);
    CHECK(node->clickable);
    CHECK(node->longClickable);
    CHECK(node->focused);
    CHECK(!node->scrollsVertically);
    CHECK(node->local == (LayoutRect{64, 21, 813, 148}));
    CHECK_EQ(node->resourceId, std::string{"app:id/toolbar_text"});

    const auto stub = parseViewLine(
        "android.view.ViewStub{2c00beb G.E...... ......I. 0,0-0,0 #10201ca android:id/action_mode_bar_stub}");
    REQUIRE(stub.has_value());
    CHECK(stub->visibility == ViewVisibility::Gone);
    CHECK(stub->layoutRequested);
    CHECK_EQ(stub->idName(), std::string_view{"action_mode_bar_stub"});

    const auto negative = parseViewLine("android.view.View{1 V.ED..... ........ -20,-5-100,40}");
    REQUIRE(negative.has_value());
    CHECK(negative->local == (LayoutRect{-20, -5, 100, 40}));

    CHECK(!parseViewLine("DecorView@bfcdbf5[HomeActivity]").has_value());
    CHECK(!parseViewLine("android.view.View{1 Q.ED..... ........ 0,0-1,1}").has_value());
}

TEST_CASE("expandComponent and findWindowFrame") {
    CHECK_EQ(expandComponent("bitpit.launcher/.ui.HomeActivity"),
             std::string{"bitpit.launcher/bitpit.launcher.ui.HomeActivity"});
    CHECK_EQ(expandComponent("a.b/c.D"), std::string{"a.b/c.D"});
    const auto frame = findWindowFrame(kWindows, "bitpit.launcher/.ui.HomeActivity");
    REQUIRE(frame.has_value());
    CHECK(*frame == (LayoutRect{0, 50, 1080, 2460}));
    CHECK(!findWindowFrame(kWindows, "x.y/.Z").has_value());
    const auto legacy = findWindowFrame(
        "  Window #1 Window{1 u0 a.b/a.b.C}:\n    mFrame=[0,63][1080,1920] last=[0,0][1,1]\n", "a.b/.C");
    REQUIRE(legacy.has_value());
    CHECK(*legacy == (LayoutRect{0, 63, 1080, 1920}));
}

TEST_CASE("parseActivityTop builds one tree per activity, placed in its window") {
    const ViewDump dump = parseActivityTop(kActivityTop, kWindows);
    REQUIRE(dump.windows.size() == 2);
    CHECK(!dump.windows[0].resumed);
    CHECK_EQ(dump.windows[0].pid, 1111);
    const LayoutWindow& home = dump.windows[1];
    CHECK(home.resumed);
    CHECK_EQ(home.pid, 4321);
    CHECK_EQ(home.title, std::string{"HomeActivity"});
    CHECK_EQ(home.component, std::string{"bitpit.launcher/bitpit.launcher.ui.HomeActivity"});
    CHECK(home.frame == (LayoutRect{0, 50, 1080, 2460}));

    const auto& nodes = dump.nodes;
    const LayoutNode& decor = nodes[static_cast<std::size_t>(home.root)];
    CHECK_EQ(decor.simpleName(), std::string_view{"DecorView"});
    CHECK_EQ(decor.children.size(), std::size_t{1});

    const int edit = findById(nodes, "bitpit.launcher:id/toolbar_text");
    REQUIRE(edit >= 0);
    CHECK(nodes[static_cast<std::size_t>(edit)].bounds == (LayoutRect{64, 171, 813, 298}));
    CHECK_EQ(nodes[static_cast<std::size_t>(edit)].depth, 3);

    const int stub = findById(nodes, "android:id/action_mode_bar_stub");
    REQUIRE(stub >= 0);
    CHECK(!nodes[static_cast<std::size_t>(stub)].shown);

    const int row = findById(nodes, "bitpit.launcher:id/row");
    REQUIRE(row >= 0);
    CHECK(nodes[static_cast<std::size_t>(row)].bounds == (LayoutRect{0, 1650, 1080, 1800}));
}

TEST_CASE("parseDisplaySize prefers display 0, parseDensity prefers the override") {
    const char* displays =
        "  Display: mDisplayId=9\n    init=704x1600 1dpi cur=704x1600 app=704x1600\n"
        "  Display: mDisplayId=0 (organized)\n"
        "    init=1080x2460 480dpi base=1080x2460 425dpi cur=2460x1080 app=2352x1080\n";
    const auto size = parseDisplaySize(displays);
    REQUIRE(size.has_value());
    CHECK_EQ(size->first, 2460);
    CHECK_EQ(size->second, 1080);
    CHECK(!parseDisplaySize("nothing here").has_value());

    CHECK_EQ(parseDensity("Physical density: 480\nOverride density: 425\n").value_or(0), 425);
    CHECK_EQ(parseDensity("Physical density: 480\n").value_or(0), 480);
}

TEST_CASE("parseAccessibilityXml reads nodes, entities and nesting") {
    std::vector<LayoutNode> nodes;
    const auto roots = parseAccessibilityXml(kAccessibility, nodes);
    REQUIRE(roots.hasValue());
    REQUIRE(roots.value().size() == 1);
    CHECK_EQ(nodes.size(), std::size_t{9});
    const int edit = findById(nodes, "bitpit.launcher:id/toolbar_text");
    REQUIRE(edit >= 0);
    const LayoutNode& n = nodes[static_cast<std::size_t>(edit)];
    CHECK_EQ(n.text, std::string{"Search apps & \"more\""});
    CHECK(n.focused);
    CHECK(n.bounds == (LayoutRect{64, 171, 813, 298}));
    CHECK_EQ(n.depth, 3);

    std::vector<LayoutNode> none;
    const auto failed = parseAccessibilityXml("ERROR: null root node returned by UiTestAutomationBridge.\n", none);
    REQUIRE(!failed.hasValue());
    CHECK(failed.error().message.find("null root node") != std::string::npos);
    CHECK(!parseAccessibilityXml("<hierarchy><node bounds=\"[0,0][1,1]", none).hasValue());
}

TEST_CASE("mergeAccessibility adds text, grafts Compose nodes and follows scrolling") {
    ViewDump dump = parseActivityTop(kActivityTop, kWindows);
    std::vector<LayoutNode> a11y;
    const auto roots = parseAccessibilityXml(kAccessibility, a11y);
    REQUIRE(roots.hasValue());
    const int root = dump.windows[1].root;
    const int matched = mergeAccessibility(dump.nodes, root, a11y, roots.value());
    CHECK(matched >= 7);
    const auto& nodes = dump.nodes;

    const int edit = findById(nodes, "bitpit.launcher:id/toolbar_text");
    CHECK(nodes[static_cast<std::size_t>(edit)].hasAccessibility);
    CHECK_EQ(nodes[static_cast<std::size_t>(edit)].text, std::string{"Search apps & \"more\""});

    // The Compose button only exists in the accessibility tree.
    const int composeHost = findByClass(nodes, "androidx.compose.ui.platform.AndroidComposeView");
    REQUIRE(composeHost >= 0);
    const auto& hostChildren = nodes[static_cast<std::size_t>(composeHost)].children;
    REQUIRE(hostChildren.size() == 1);
    const LayoutNode& button = nodes[static_cast<std::size_t>(hostChildren[0])];
    CHECK(button.origin == LayoutNode::Origin::Accessibility);
    CHECK_EQ(button.text, std::string{"Buy"});
    CHECK(button.bounds == (LayoutRect{40, 600, 400, 700}));
    CHECK_EQ(button.depth, nodes[static_cast<std::size_t>(composeHost)].depth + 1);

    // The row sits 300 px higher than the View dump says.
    const LayoutNode& row = nodes[static_cast<std::size_t>(findById(nodes, "bitpit.launcher:id/row"))];
    CHECK(row.bounds == (LayoutRect{0, 1350, 1080, 1500}));
    CHECK_EQ(row.shiftY, -300);
    CHECK_EQ(row.text, std::string{"Row"});
}

TEST_CASE("hitTest prefers the topmost view with content over empty containers") {
    ViewDump dump = parseActivityTop(kActivityTop, kWindows);
    const std::vector<int> roots{dump.windows[1].root};
    const int edit = findById(dump.nodes, "bitpit.launcher:id/toolbar_text");
    CHECK_EQ(hitTest(dump.nodes, roots, 100, 200, false), edit);
    // Nothing that draws under this point: the deepest container answers.
    const int content = hitTest(dump.nodes, roots, 1000, 2400, false);
    CHECK(content >= 0);
    CHECK_EQ(hitTest(dump.nodes, roots, 5000, 5000, false), -1);
    // The GONE stub at 0,0-0,0 never matches.
    CHECK(hitTest(dump.nodes, roots, 0, 50, true) != findById(dump.nodes, "android:id/action_mode_bar_stub"));
}

TEST_CASE("auditLayout flags small and unlabelled touch targets") {
    LayoutSnapshot snapshot;
    snapshot.screenWidth = 1080;
    snapshot.screenHeight = 2460;
    snapshot.densityDpi = 480;
    LayoutNode root;
    root.className = "android.widget.FrameLayout";
    root.resourceId = "android:id/content";
    root.bounds = {0, 0, 1080, 2460};
    root.children = {1};
    LayoutNode icon;
    icon.className = "android.widget.ImageButton";
    icon.parent = 0;
    icon.clickable = true;
    icon.draws = true;
    icon.hasAccessibility = true;
    icon.bounds = {10, 10, 100, 100};  // 30 dp
    snapshot.nodes = {root, icon};
    finalizeTree(snapshot.nodes, {0});

    const auto issues = auditLayout(snapshot, {0});
    bool small = false;
    bool label = false;
    for (const auto& issue : issues) {
        if (issue.node != 1) continue;
        small |= issue.rule == LayoutIssue::Rule::SmallTouchTarget;
        label |= issue.rule == LayoutIssue::Rule::MissingLabel;
    }
    CHECK(small);
    CHECK(label);
}

TEST_CASE("copy-as helpers") {
    LayoutNode node;
    node.className = "android.widget.Button";
    node.resourceId = "com.example:id/ok";
    node.bounds = {100, 200, 300, 400};
    CHECK_EQ(espressoMatcher(node), std::string{"onView(withId(R.id.ok))"});
    CHECK_EQ(uiAutomatorSelector(node), std::string{"device.findObject(By.res(\"com.example:id/ok\"))"});
    CHECK_EQ(adbTapCommand(node), std::string{"adb shell input tap 200 300"});
    node.resourceId = "android:id/button1";
    CHECK_EQ(espressoMatcher(node), std::string{"onView(withId(android.R.id.button1))"});
    node.resourceId.clear();
    node.text = "Say \"hi\"";
    CHECK_EQ(espressoMatcher(node), std::string{"onView(withText(\"Say \\\"hi\\\"\"))"});
}
