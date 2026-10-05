#pragma once

// The wire format of scrcpy's server, for the version vendored in
// vendor/scrcpy. The protocol is internal to scrcpy and changes between
// releases with no compatibility, and the server refuses a client of another
// version; tests/test_scrcpy_protocol.cpp pins the layouts below to the ones
// in scrcpy's own serializer (app/src/control_msg.c) at that version.
//
// Everything multi-byte is big-endian (the server is Java).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.h"

namespace em::scrcpy {

inline constexpr const char* kDeviceServerPath = "/data/local/tmp/emeron-scrcpy-server.jar";
inline constexpr std::size_t kDeviceNameLength = 64;
inline constexpr std::size_t kCodecMetaLength = 12;
inline constexpr std::size_t kFrameHeaderLength = 12;

inline constexpr std::uint32_t kCodecH264 = 0x68323634;  // "h264"
inline constexpr std::uint32_t kCodecH265 = 0x68323635;  // "h265"
inline constexpr std::uint32_t kCodecAv1 = 0x00617631;   // "av1"

// Socket name the server listens on, from a 31-bit session id.
[[nodiscard]] std::string socketName(std::uint32_t scid);

struct ServerOptions {
    std::string version;            // must equal the server's own version
    std::uint32_t scid = 0;
    int maxSize = 1600;             // longest side, 0 = native
    int maxFps = 60;
    int videoBitRate = 8'000'000;
};
// The `adb shell` command that starts the server.
[[nodiscard]] std::string serverCommand(const ServerOptions& options);

// `adb forward tcp:0 ...` prints the port it picked.
[[nodiscard]] std::optional<std::uint16_t> parseForwardPort(std::string_view output);

struct CodecMeta {
    std::uint32_t codecId = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
// Codec id 0 means the server disabled the stream, 1 that it failed.
[[nodiscard]] Result<CodecMeta> parseCodecMeta(std::string_view bytes);

struct FrameHeader {
    bool config = false;    // codec configuration (SPS/PPS), not a picture
    bool keyFrame = false;
    std::int64_t pts = 0;   // microseconds
    std::uint32_t size = 0;
};
[[nodiscard]] FrameHeader parseFrameHeader(std::string_view bytes);

// NUL-padded UTF-8 device name from the first socket.
[[nodiscard]] std::string parseDeviceName(std::string_view bytes);

// --- control messages ---------------------------------------------------------

enum class TouchAction : std::uint8_t { Down = 0, Up = 1, Move = 2 };
enum class KeyAction : std::uint8_t { Down = 0, Up = 1 };

// A finger, not a mouse: the server injects it as a touchscreen event.
inline constexpr std::uint64_t kPointerGenericFinger = ~std::uint64_t{0} - 1;  // -2

[[nodiscard]] std::string touchMessage(TouchAction action, std::uint64_t pointerId, std::int32_t x,
                                       std::int32_t y, std::uint16_t screenWidth,
                                       std::uint16_t screenHeight, float pressure,
                                       std::uint32_t actionButton = 0, std::uint32_t buttons = 0);
[[nodiscard]] std::string keycodeMessage(KeyAction action, std::uint32_t keycode,
                                         std::uint32_t repeat = 0, std::uint32_t metaState = 0);
// UTF-8, truncated to the server's 300-byte limit on a character boundary.
[[nodiscard]] std::string textMessage(std::string_view utf8);
// Scroll amounts in [-16, 16]; positive vscroll scrolls up, as a wheel does.
[[nodiscard]] std::string scrollMessage(std::int32_t x, std::int32_t y, std::uint16_t screenWidth,
                                        std::uint16_t screenHeight, float hscroll, float vscroll,
                                        std::uint32_t buttons = 0);

// Asks the server to restart the encoder: a fresh config packet and key frame
// follow, so decoding can resume after packets were skipped.
[[nodiscard]] std::string resetVideoMessage();

// Android keycodes for the names the mirror uses, e.g. "KEYCODE_BACK" -> 4.
[[nodiscard]] std::optional<std::uint32_t> keycodeFromName(std::string_view name);

}  // namespace em::scrcpy
