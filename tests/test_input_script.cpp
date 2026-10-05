#include "TestHarness.h"

#include "app/InputScript.h"

using namespace em;
using Kind = ScriptStep::Kind;

TEST_CASE("InputScript expands a click into move, press and release on separate frames") {
    auto steps = parseInputScript("# a comment\n\nclick 10 20.5\n");
    REQUIRE(static_cast<bool>(steps));
    const auto& s = steps.value();
    REQUIRE(s.size() == std::size_t{6});
    CHECK(s[0].kind == Kind::MouseMove);
    CHECK_EQ(s[0].y, 20.5F);
    CHECK(s[1].kind == Kind::Frame);
    CHECK(s[2].kind == Kind::MouseButton);
    CHECK(s[2].down);
    CHECK(s[4].kind == Kind::MouseButton);
    CHECK(!s[4].down);
}

TEST_CASE("InputScript parses chords, buttons and waits") {
    auto steps = parseInputScript(
        "key ctrl+f\nclick 1 2 right double\nwait 1.5\nhold alt\nshot out/a.png\n");
    REQUIRE(static_cast<bool>(steps));
    const auto& s = steps.value();
    CHECK(s[0].kind == Kind::Key);
    CHECK(s[0].key == ImGuiMod_Ctrl);
    CHECK(s[1].key == ImGuiKey_F);
    int rightPresses = 0;
    bool waited = false;
    bool alt = false;
    for (const auto& step : s) {
        rightPresses += step.kind == Kind::MouseButton && step.down &&
                        step.button == ImGuiMouseButton_Right;
        waited |= step.kind == Kind::Wait && step.seconds == 1.5;
        alt |= step.kind == Kind::Key && step.key == ImGuiMod_Alt && step.down;
    }
    CHECK_EQ(rightPresses, 2);
    CHECK(waited);
    CHECK(alt);
    CHECK(s.back().kind == Kind::Frame);
    CHECK(s[s.size() - 2].kind == Kind::Shot);
    CHECK_EQ(s[s.size() - 2].text, std::string{"out/a.png"});
}

TEST_CASE("InputScript reports the line of a bad step") {
    auto steps = parseInputScript("wait 1\nclick ten 20\n");
    REQUIRE(!static_cast<bool>(steps));
    CHECK(steps.error().message.find("line 2") != std::string::npos);
    CHECK(!static_cast<bool>(parseInputScript("key ctrl+nope")));
    CHECK(!static_cast<bool>(parseInputScript("key ctrl")));
    CHECK(!static_cast<bool>(parseInputScript("fly 1 2")));
}
