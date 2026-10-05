#include "TestHarness.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "collect/ScreenMirror.h"
#include "core/Gzip.h"
#include "core/PngWriter.h"

using namespace em;

namespace {

std::string readFixture(const char* name) {
    std::ifstream in{std::string{EMERON_TEST_DATA_DIR} + "/" + name, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::string header(std::uint32_t w, std::uint32_t h, std::uint32_t format, bool withColorSpace) {
    std::string out(withColorSpace ? 16 : 12, '\0');
    std::memcpy(&out[0], &w, 4);
    std::memcpy(&out[4], &h, 4);
    std::memcpy(&out[8], &format, 4);
    if (withColorSpace) {
        const std::uint32_t srgb = 1;
        std::memcpy(&out[12], &srgb, 4);
    }
    return out;
}

}  // namespace

// The fixtures: a 4x3 RGBA frame whose pixel i is (10i, 10i+1, 10i+2, alpha 0),
// raw and gzip-compressed by Python (tests/data/screencap_4x3*).

TEST_CASE("gunzip matches Python's gzip, with and without a file name in the header") {
    const std::string raw = readFixture("screencap_4x3.raw");
    REQUIRE(raw.size() == 16 + 4 * 3 * 4);

    for (const char* name : {"screencap_4x3.raw.gz", "screencap_4x3_named.raw.gz"}) {
        const std::string gz = readFixture(name);
        CHECK(looksLikeGzip(gz));
        const auto out = gunzip(gz);
        CHECK(out.hasValue());
        if (out) CHECK(out.value() == raw);
    }
}

TEST_CASE("gunzip rejects corrupt and truncated streams instead of returning garbage") {
    std::string gz = readFixture("screencap_4x3.raw.gz");
    REQUIRE(gz.size() > 30);
    CHECK(!gunzip(gz.substr(0, gz.size() - 5)).hasValue());  // truncated trailer
    std::string flipped = gz;
    flipped[flipped.size() - 8] = static_cast<char>(flipped[flipped.size() - 8] ^ 0x01);  // CRC
    CHECK(!gunzip(flipped).hasValue());
    CHECK(!looksLikeGzip("screencap: not found"));
    CHECK(!gunzip("screencap: not found").hasValue());
}

TEST_CASE("parseScreencap reads the 16-byte Android 9+ header and forces alpha opaque") {
    const auto frame = parseScreencap(readFixture("screencap_4x3.raw"));
    REQUIRE(frame.hasValue());
    CHECK_EQ(frame.value().width, std::uint32_t{4});
    CHECK_EQ(frame.value().height, std::uint32_t{3});
    REQUIRE(frame.value().rgba.size() == std::size_t{48});
    // Pixel 5: (50, 51, 52), alpha forced from 0 to 255.
    CHECK_EQ(int{frame.value().rgba[20]}, 50);
    CHECK_EQ(int{frame.value().rgba[21]}, 51);
    CHECK_EQ(int{frame.value().rgba[22]}, 52);
    CHECK_EQ(int{frame.value().rgba[23]}, 255);
}

TEST_CASE("parseScreencap reads the older 12-byte header") {
    const std::string raw = header(2, 1, 1, false) + std::string{"\x01\x02\x03\x04\x05\x06\x07\x08", 8};
    const auto frame = parseScreencap(raw);
    REQUIRE(frame.hasValue());
    CHECK_EQ(int{frame.value().rgba[4]}, 5);
}

TEST_CASE("parseScreencap swaps BGRA and expands RGB 565") {
    const auto bgra = parseScreencap(header(1, 1, 5, true) + std::string{"\x10\x20\x30\x00", 4});
    REQUIRE(bgra.hasValue());
    CHECK_EQ(int{bgra.value().rgba[0]}, 0x30);
    CHECK_EQ(int{bgra.value().rgba[2]}, 0x10);

    // 0xF800 is pure red in 565.
    const auto rgb565 = parseScreencap(header(1, 1, 4, true) + std::string{"\x00\xF8", 2});
    REQUIRE(rgb565.hasValue());
    CHECK_EQ(int{rgb565.value().rgba[0]}, 255);
    CHECK_EQ(int{rgb565.value().rgba[1]}, 0);
    CHECK_EQ(int{rgb565.value().rgba[2]}, 0);
    CHECK_EQ(int{rgb565.value().rgba[3]}, 255);
}

TEST_CASE("parseScreencap reports screencap's error text and size mismatches") {
    const auto text = parseScreencap("/system/bin/sh: screencap: inaccessible or not found\n");
    REQUIRE(!text.hasValue());
    CHECK(text.error().message.find("not found") != std::string::npos);

    const auto shortFrame = parseScreencap(header(4, 3, 1, true) + std::string(10, '\0'));
    CHECK(!shortFrame.hasValue());
    CHECK(!parseScreencap(header(4, 3, 99, true) + std::string(48, '\0')).hasValue());
}

TEST_CASE("captureLoopCommand pipes through gzip only when asked, and -w0 keeps one line per frame") {
    CHECK(captureLoopCommand(true).find("gzip -1") != std::string::npos);
    CHECK(captureLoopCommand(true).find("base64 -w0") != std::string::npos);
    CHECK(captureLoopCommand(false).find("gzip") == std::string::npos);
    CHECK(captureLoopCommand(false).find("base64 -w0") != std::string::npos);
    // Starts a loop that never exits on its own: the caller tears it down.
    CHECK(captureLoopCommand(true).find("while true") != std::string::npos);
}

TEST_CASE("inputTextArgument quotes for the shell and spells spaces as %s") {
    CHECK_EQ(inputTextArgument("hello world"), std::string{"'hello%sworld'"});
    CHECK_EQ(inputTextArgument("it's"), std::string{"'it'\\''s'"});
    CHECK_EQ(inputTextArgument("a;rm -rf /"), std::string{"'a;rm%s-rf%s/'"});
    CHECK(inputTextArgument("").empty());
    CHECK(inputTextArgument("caf\xc3\xa9").empty());  // non-ASCII cannot go through `input text`
    CHECK(inputTextArgument("line\nbreak").empty());
}

TEST_CASE("writePng compresses, and the result is a PNG") {
    // A flat 200x200 image: stored blocks would be ~160 KB, deflate far less.
    std::vector<std::uint8_t> rgba(200 * 200 * 4, 0x80);
    const auto file = std::filesystem::temp_directory_path() / "emeron-test-mirror.png";
    REQUIRE(writePng(file, rgba, 200, 200).hasValue());
    std::ifstream in{file, std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    CHECK(bytes.starts_with("\x89PNG\r\n\x1a\n"));
    CHECK(bytes.size() < 10'000);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// ---------------------------------------------------------------------------
// Gestures: from a mouse press on the mirror to an `input` command
// ---------------------------------------------------------------------------

TEST_CASE("screenToDevice maps through the image's position and scale, clamped to the frame") {
    // A 1080x2460 frame drawn at (100, 50), half size.
    const DevicePoint centre = screenToDevice(100 + 270, 50 + 615, 100, 50, 0.5F, 1080, 2460);
    CHECK_EQ(centre.x, 540.0F);
    CHECK_EQ(centre.y, 1230.0F);
    const DevicePoint outside = screenToDevice(0, 5000, 100, 50, 0.5F, 1080, 2460);
    CHECK_EQ(outside.x, 0.0F);
    CHECK_EQ(outside.y, 2459.0F);
    const DevicePoint none = screenToDevice(10, 10, 0, 0, 0.0F, 1080, 2460);
    CHECK_EQ(none.x, 0.0F);
}

TEST_CASE("classifyGesture: a short, still press is a tap") {
    const Gesture g = classifyGesture({540, 1230}, {542, 1231}, 2.0F, 0.12);
    CHECK(g.kind == Gesture::Kind::Tap);
    CHECK_EQ(g.x1, 540);
    CHECK_EQ(g.y1, 1230);
}

TEST_CASE("classifyGesture: a long, still press is a long press") {
    CHECK(classifyGesture({10, 10}, {10, 10}, 0.0F, 0.8).kind == Gesture::Kind::LongPress);
}

TEST_CASE("classifyGesture: movement is judged on the computer's screen, not in device pixels") {
    // At a small scale 4 screen pixels can be 20 device pixels: still a tap.
    CHECK(classifyGesture({100, 100}, {120, 100}, 4.0F, 0.1).kind == Gesture::Kind::Tap);
}

TEST_CASE("classifyGesture: a drag is a swipe that lasts as long as the drag, within limits") {
    const Gesture g = classifyGesture({540, 1800}, {540, 600}, 300.0F, 0.35);
    CHECK(g.kind == Gesture::Kind::Swipe);
    CHECK_EQ(g.y1, 1800);
    CHECK_EQ(g.y2, 600);
    CHECK_EQ(g.durationMs, 350);
    CHECK_EQ(classifyGesture({0, 0}, {0, 500}, 100.0F, 0.01).durationMs, 80);   // a flick
    CHECK_EQ(classifyGesture({0, 0}, {0, 500}, 100.0F, 9.0).durationMs, 2000);  // a slow drag
}

#include <chrono>
#include <mutex>
#include <thread>

#include "adb/AdbClient.h"
#include "adb/DeviceManager.h"

namespace {

class InputRecorder final : public IAdbTransport {
public:
    [[nodiscard]] bool available() const override { return true; }
    [[nodiscard]] std::string describe() const override { return "fake"; }
    Result<CommandOutput> host(std::span<const std::string>, std::chrono::milliseconds) override {
        return CommandOutput{};
    }
    Result<CommandOutput> device(std::string_view, std::span<const std::string>,
                                 std::chrono::milliseconds) override {
        return CommandOutput{};
    }
    Result<CommandOutput> shell(std::string_view, std::string_view command,
                                std::chrono::milliseconds) override {
        const std::lock_guard lock{mutex_};
        commands_.emplace_back(command);
        return CommandOutput{};
    }
    Result<std::unique_ptr<Subprocess>> stream(std::string_view, std::span<const std::string>,
                                               bool) override {
        return makeError(ErrorKind::Unsupported, "fake");
    }
    std::vector<std::string> waitFor(std::size_t count) {
        for (int i = 0; i < 500; ++i) {
            {
                const std::lock_guard lock{mutex_};
                if (commands_.size() >= count) return commands_;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        const std::lock_guard lock{mutex_};
        return commands_;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> commands_;
};

}  // namespace

TEST_CASE("ScreenMirror sends input in order, and refuses what it cannot send safely") {
    auto transport = std::make_shared<InputRecorder>();
    DeviceManager devices{std::make_shared<AdbClient>(transport)};
    devices.selectSerial("FAKE01");
    ScreenMirror mirror{devices};

    mirror.tap(540, 1230);
    mirror.longPress(10, 20);
    mirror.swipe(540, 1800, 540, 600, 350);
    mirror.key("KEYCODE_BACK");
    mirror.key("KEYCODE_HOME; reboot");  // refused: not a keycode
    mirror.text("hi there");
    mirror.text("caf\xc3\xa9");          // refused: not ASCII

    const auto commands = transport->waitFor(5);
    CHECK_EQ(commands.size(), std::size_t{5});
    if (commands.size() != 5) return;
    CHECK_EQ(commands[0], std::string{"input tap 540 1230"});
    CHECK_EQ(commands[1], std::string{"input swipe 10 20 10 20 700"});  // long press
    CHECK_EQ(commands[2], std::string{"input swipe 540 1800 540 600 350"});
    CHECK_EQ(commands[3], std::string{"input keyevent KEYCODE_BACK"});
    CHECK_EQ(commands[4], std::string{"input text 'hi%sthere'"});
}
