#include "TestHarness.h"

#include "adb/AdbClient.h"

using namespace em;

TEST_CASE("parseDevicesOutput reads the long form") {
    constexpr const char* kOutput =
        "List of devices attached\n"
        "28301FDH2003XY         device product:cheetah model:Pixel_7_Pro "
        "device:cheetah transport_id:3\n"
        "emulator-5554          device product:sdk_gphone64_arm64 "
        "model:sdk_gphone64_arm64 device:emu64a transport_id:1\n"
        "192.168.1.50:5555      offline\n"
        "ZY22ABCDEF             unauthorized\n";

    const DeviceList list = parseDevicesOutput(kOutput);
    CHECK_EQ(list.devices.size(), std::size_t{4});
    CHECK(list.serverReachable);

    // Usable devices sort first.
    CHECK(list.devices[0].usable());
    CHECK(list.devices[1].usable());

    const DeviceRef* pixel = list.find("28301FDH2003XY");
    CHECK(pixel != nullptr);
    if (pixel != nullptr) {
        CHECK_EQ(pixel->model, std::string{"Pixel_7_Pro"});
        CHECK_EQ(pixel->product, std::string{"cheetah"});
        CHECK_EQ(pixel->transportId, std::string{"3"});
        CHECK(pixel->state == DeviceState::Online);
        CHECK(!pixel->isEmulator());
        CHECK_EQ(pixel->displayName(), std::string{"Pixel 7 Pro (28301FDH2003XY)"});
    }

    const DeviceRef* emulator = list.find("emulator-5554");
    CHECK(emulator != nullptr);
    if (emulator != nullptr) CHECK(emulator->isEmulator());

    const DeviceRef* network = list.find("192.168.1.50:5555");
    CHECK(network != nullptr);
    if (network != nullptr) {
        CHECK(network->isNetworkDevice());
        CHECK(network->state == DeviceState::Offline);
    }

    const DeviceRef* unauthorized = list.find("ZY22ABCDEF");
    CHECK(unauthorized != nullptr);
    if (unauthorized != nullptr) CHECK(unauthorized->state == DeviceState::Unauthorized);
}

TEST_CASE("parseDevicesOutput skips daemon chatter") {
    constexpr const char* kOutput =
        "* daemon not running; starting now at tcp:5037\n"
        "* daemon started successfully\n"
        "List of devices attached\n"
        "abc123 device\n";

    const DeviceList list = parseDevicesOutput(kOutput);
    CHECK_EQ(list.devices.size(), std::size_t{1});
    CHECK_EQ(list.devices[0].serial, std::string{"abc123"});
}

TEST_CASE("parseDevicesOutput on an empty list") {
    const DeviceList list = parseDevicesOutput("List of devices attached\n\n");
    CHECK(list.devices.empty());
    CHECK(list.serverReachable);
}

TEST_CASE("parseGetpropOutput reads bracketed pairs") {
    constexpr const char* kOutput =
        "[ro.product.model]: [Pixel 7 Pro]\n"
        "[ro.build.version.sdk]: [34]\n"
        "[ro.empty]: []\n"
        "[ro.odd.value]: [a [nested] b]\n"
        "garbage line\n";

    const PropertyMap props = parseGetpropOutput(kOutput);
    CHECK_EQ(props.size(), std::size_t{4});
    CHECK_EQ(props.at("ro.product.model"), std::string{"Pixel 7 Pro"});
    CHECK_EQ(props.at("ro.build.version.sdk"), std::string{"34"});
    CHECK_EQ(props.at("ro.empty"), std::string{});
    // rfind(']') means the whole bracketed remainder is kept.
    CHECK_EQ(props.at("ro.odd.value"), std::string{"a [nested] b"});
}

TEST_CASE("splitBatchOutput partitions on the sentinel") {
    const std::string text =
        "first output\nSEP\nsecond output\nmore\nSEP\n";
    const auto parts = splitBatchOutput(text, "SEP", 3);

    CHECK_EQ(parts.size(), std::size_t{3});
    CHECK_EQ(parts[0], std::string{"first output"});
    CHECK_EQ(parts[1], std::string{"second output\nmore"});
    CHECK_EQ(parts[2], std::string{});
}

TEST_CASE("splitBatchOutput pads a truncated response") {
    const auto parts = splitBatchOutput("only one", "SEP", 4);
    CHECK_EQ(parts.size(), std::size_t{4});
    CHECK_EQ(parts[0], std::string{"only one"});
    CHECK_EQ(parts[3], std::string{});
}

TEST_CASE("splitBatchOutput truncates an over-long response") {
    const auto parts = splitBatchOutput("a\nSEP\nb\nSEP\nc", "SEP", 2);
    CHECK_EQ(parts.size(), std::size_t{2});
    CHECK_EQ(parts[1], std::string{"b"});
}

TEST_CASE("parseDeviceState covers the states adb reports") {
    CHECK(parseDeviceState("device") == DeviceState::Online);
    CHECK(parseDeviceState("offline") == DeviceState::Offline);
    CHECK(parseDeviceState("unauthorized") == DeviceState::Unauthorized);
    CHECK(parseDeviceState("recovery") == DeviceState::Recovery);
    CHECK(parseDeviceState("sideload") == DeviceState::Sideload);
    CHECK(parseDeviceState("nonsense") == DeviceState::Unknown);
}
