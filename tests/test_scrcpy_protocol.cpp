#include "TestHarness.h"

#include <string>

#include "collect/ScrcpyProtocol.h"

using namespace em;
using namespace em::scrcpy;

namespace {
std::string bytes(std::initializer_list<int> list) {
    std::string out;
    for (const int b : list) out.push_back(static_cast<char>(b));
    return out;
}
}  // namespace

// The inputs and expected bytes are copied from scrcpy v3.3.4's own
// serializer tests (app/tests/test_control_msg_serialize.c), so a layout drift
// on either side fails here rather than as ignored input on a phone.

TEST_CASE("touchMessage matches scrcpy's INJECT_TOUCH_EVENT layout") {
    // test_serialize_inject_touch_event: DOWN, pointer 0x1234567887654321,
    // (100, 200) on 1080x1920, pressure 1.0, action button and buttons PRIMARY.
    const std::string msg = touchMessage(TouchAction::Down, 0x1234567887654321ULL, 100, 200,
                                         1080, 1920, 1.0F, 1, 1);
    const std::string expected = bytes({
        0x02, 0x00,                                      // type, action
        0x12, 0x34, 0x56, 0x78, 0x87, 0x65, 0x43, 0x21,  // pointer id
        0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0xc8,  // 100 200
        0x04, 0x38, 0x07, 0x80,                          // 1080 1920
        0xff, 0xff,                                      // pressure 1.0
        0x00, 0x00, 0x00, 0x01,                          // action button PRIMARY
        0x00, 0x00, 0x00, 0x01,                          // buttons PRIMARY
    });
    CHECK_EQ(msg.size(), std::size_t{32});
    CHECK(msg == expected);
}

TEST_CASE("keycodeMessage matches scrcpy's INJECT_KEYCODE layout") {
    // test_serialize_inject_keycode: UP, ENTER (66), repeat 5,
    // AMETA_SHIFT_ON | AMETA_SHIFT_LEFT_ON (0x41).
    const std::string msg = keycodeMessage(KeyAction::Up, 66, 5, 0x41);
    const std::string expected = bytes({0x00, 0x01, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00,
                                        0x05, 0x00, 0x00, 0x00, 0x41});
    CHECK(msg == expected);
}

TEST_CASE("textMessage is length-prefixed and truncates on a UTF-8 boundary") {
    CHECK(textMessage("hello, world!") ==
          bytes({0x01, 0x00, 0x00, 0x00, 0x0d}) + std::string{"hello, world!"});

    // 299 ASCII bytes + a 2-byte character: the character would cross the
    // 300-byte limit, so it is dropped whole, not split.
    const std::string longText = std::string(299, 'a') + "\xc3\xa9";
    const std::string msg = textMessage(longText);
    CHECK_EQ(msg.size(), std::size_t{5 + 299});
}

TEST_CASE("scrollMessage matches scrcpy's INJECT_SCROLL_EVENT layout") {
    // test_serialize_inject_scroll_event: (260, 1026) on 1080x1920, hscroll 16,
    // vscroll -16, buttons 1.
    const std::string msg = scrollMessage(260, 1026, 1080, 1920, 16.0F, -16.0F, 1);
    const std::string expected = bytes({0x03, 0x00, 0x00, 0x01, 0x04, 0x00, 0x00, 0x04, 0x02,
                                        0x04, 0x38, 0x07, 0x80, 0x7f, 0xff, 0x80, 0x00,
                                        0x00, 0x00, 0x00, 0x01});
    CHECK(msg == expected);
}

TEST_CASE("frame header: config and key-frame flags in the top bits, then the size") {
    const FrameHeader key = parseFrameHeader(bytes({0x40, 0, 0, 0, 0, 0, 0x30, 0x39, 0, 0, 0x10, 0}));
    CHECK(!key.config);
    CHECK(key.keyFrame);
    CHECK_EQ(key.pts, std::int64_t{12345});
    CHECK_EQ(key.size, std::uint32_t{4096});

    const FrameHeader config = parseFrameHeader(bytes({0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x20}));
    CHECK(config.config);
    CHECK_EQ(config.size, std::uint32_t{32});
}

TEST_CASE("codec header: h264 and size, or the server's disable/error codes") {
    const auto meta = parseCodecMeta(bytes({0x68, 0x32, 0x36, 0x34, 0, 0, 0x02, 0xc0, 0, 0, 0x06, 0x40}));
    REQUIRE(meta.hasValue());
    CHECK_EQ(meta.value().codecId, kCodecH264);
    CHECK_EQ(meta.value().width, std::uint32_t{704});
    CHECK_EQ(meta.value().height, std::uint32_t{1600});
    CHECK(!parseCodecMeta(bytes({0, 0, 0, 0})).hasValue());
    CHECK(!parseCodecMeta(bytes({0, 0, 0, 1})).hasValue());
}

TEST_CASE("server command, socket name, forward port and device name") {
    ServerOptions o;
    o.version = "3.3.4";
    o.scid = 0x1234abcd;
    const std::string cmd = serverCommand(o);
    CHECK(cmd.starts_with("CLASSPATH=/data/local/tmp/emeron-scrcpy-server.jar app_process / "
                          "com.genymobile.scrcpy.Server 3.3.4 scid=1234abcd"));
    CHECK(cmd.find("audio=false") != std::string::npos);
    CHECK(cmd.find("tunnel_forward=true") != std::string::npos);
    CHECK(cmd.find("max_size=1600") != std::string::npos);
    CHECK_EQ(socketName(0x1234abcd), std::string{"scrcpy_1234abcd"});
    CHECK_EQ(socketName(0xffffffff), std::string{"scrcpy_7fffffff"});  // 31-bit

    CHECK_EQ(parseForwardPort("54321\n").value_or(0), std::uint16_t{54321});
    CHECK(!parseForwardPort("error: no devices").has_value());
    CHECK_EQ(parseDeviceName(std::string{"Pixel 7\0\0\0", 10}), std::string{"Pixel 7"});
}

TEST_CASE("resetVideoMessage is the bare type byte 17") {
    CHECK(resetVideoMessage() == bytes({17}));
}

TEST_CASE("keycodeFromName knows the mirror's keys") {
    CHECK_EQ(keycodeFromName("KEYCODE_BACK").value_or(0), std::uint32_t{4});
    CHECK_EQ(keycodeFromName("KEYCODE_APP_SWITCH").value_or(0), std::uint32_t{187});
    CHECK(!keycodeFromName("KEYCODE_NOPE").has_value());
}
