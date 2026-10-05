#include "TestHarness.h"

#include <string>

#include <IconsFontAwesome6.h>
#include <imgui.h>
#include <imgui_internal.h>  // ImHashStr

#include "collect/DeviceInfo.h"
#include "ui/Icons.h"
#include "ui/Panel.h"

using namespace em;

TEST_CASE("battery icon steps with the charge level") {
    CHECK_EQ(std::string{icons::battery(100.0)}, std::string{ICON_FA_BATTERY_FULL});
    CHECK_EQ(std::string{icons::battery(90.0)}, std::string{ICON_FA_BATTERY_FULL});
    CHECK_EQ(std::string{icons::battery(75.0)}, std::string{ICON_FA_BATTERY_THREE_QUARTERS});
    CHECK_EQ(std::string{icons::battery(50.0)}, std::string{ICON_FA_BATTERY_HALF});
    CHECK_EQ(std::string{icons::battery(20.0)}, std::string{ICON_FA_BATTERY_QUARTER});
    CHECK_EQ(std::string{icons::battery(5.0)}, std::string{ICON_FA_BATTERY_EMPTY});
    // The collectors report -1 for "not reported".
    CHECK_EQ(std::string{icons::battery(-1.0)}, std::string{ICON_FA_BATTERY_EMPTY});
}

TEST_CASE("sensor icons follow the Android sensor type") {
    CHECK_EQ(std::string{icons::sensor(1)}, std::string{ICON_FA_UP_DOWN_LEFT_RIGHT});  // accel
    CHECK_EQ(std::string{icons::sensor(4)}, std::string{ICON_FA_ROTATE});              // gyro
    CHECK_EQ(std::string{icons::sensor(16)}, std::string{ICON_FA_ROTATE});             // gyro uncal
    CHECK_EQ(std::string{icons::sensor(2)}, std::string{ICON_FA_MAGNET});
    CHECK_EQ(std::string{icons::sensor(5)}, std::string{ICON_FA_LIGHTBULB});
    CHECK_EQ(std::string{icons::sensor(19)}, std::string{ICON_FA_SHOE_PRINTS});
    // Vendor-specific types (65536+) get a neutral icon, not nothing.
    CHECK_EQ(std::string{icons::sensor(65537)}, std::string{ICON_FA_SATELLITE_DISH});
}

TEST_CASE("every Device Info section title has its own icon") {
    // The panel picks icons by section title. If a title in DeviceInfo.cpp is
    // renamed, the mapping would silently fall back to the generic icon.
    PropertyMap props;
    for (const char* key : {"ro.product.model", "ro.build.version.release", "ro.soc.model",
                            "ro.opengles.version", "dalvik.vm.heapsize", "gsm.operator.alpha",
                            "ro.debuggable"}) {
        props[key] = "x";
    }
    DeviceInfoExtras extras;
    extras.screenSize = "Physical size: 1080x2400";

    const auto sections = buildInfoSections(props, extras);
    CHECK_EQ(sections.size(), std::size_t{7});
    for (const auto& section : sections) {
        if (std::string{icons::deviceInfoSection(section.title)} == ICON_FA_CIRCLE_INFO) {
            ::test::reportFailure(__FILE__, __LINE__,
                                  "section '" + section.title + "' has no specific icon");
        }
    }
}

namespace {
class FakePanel final : public Panel {
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "Device Files"; }
    [[nodiscard]] const char* icon() const noexcept override { return ICON_FA_FOLDER_OPEN; }
    void draw(AppContext&) override {}
};
}  // namespace

TEST_CASE("a panel's icon title has the same ImGui identity as its key") {
    // The scheme rests on ImGui hashing only what follows "###". If that ever
    // stopped holding, DockBuilder and imgui.ini would address the wrong window.
    FakePanel panel;
    const std::string title = panel.windowTitle();
    const std::string key = Panel::windowKey(panel.id());

    CHECK(title.starts_with(ICON_FA_FOLDER_OPEN));
    CHECK(title.find("Device Files###Device Files") != std::string::npos);
    CHECK_EQ(ImHashStr(title.c_str()), ImHashStr(key.c_str()));

    // ImGui skips the "###" marker itself, so the icon title is the very same
    // window as the plain name used before icons -- layouts already saved in
    // imgui.ini keep matching. (An earlier version assumed otherwise and forced
    // a pointless one-time layout reset; this pins the real behaviour.)
    CHECK_EQ(ImHashStr(title.c_str()), ImHashStr("Device Files"));
}
