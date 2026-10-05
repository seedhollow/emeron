#include "collect/ScrcpyProtocol.h"

#include <algorithm>
#include <cmath>

#include "core/StringUtil.h"

namespace em::scrcpy {
namespace {

void put8(std::string& out, std::uint8_t v) { out.push_back(static_cast<char>(v)); }
void put16(std::string& out, std::uint16_t v) {
    put8(out, static_cast<std::uint8_t>(v >> 8));
    put8(out, static_cast<std::uint8_t>(v));
}
void put32(std::string& out, std::uint32_t v) {
    put16(out, static_cast<std::uint16_t>(v >> 16));
    put16(out, static_cast<std::uint16_t>(v));
}
void put64(std::string& out, std::uint64_t v) {
    put32(out, static_cast<std::uint32_t>(v >> 32));
    put32(out, static_cast<std::uint32_t>(v));
}

std::uint32_t be32(std::string_view b, std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) << 24) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 3]));
}

void putPosition(std::string& out, std::int32_t x, std::int32_t y, std::uint16_t w, std::uint16_t h) {
    put32(out, static_cast<std::uint32_t>(x));
    put32(out, static_cast<std::uint32_t>(y));
    put16(out, w);
    put16(out, h);
}

// sc_float_to_u16fp: [0, 1] -> 0..0xffff.
std::uint16_t toU16Fixed(float f) {
    f = std::clamp(f, 0.0F, 1.0F);
    const auto u = static_cast<std::uint32_t>(f * 65536.0F);
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(u, 0xFFFF));
}

// sc_float_to_i16fp: [-1, 1] -> -0x8000..0x7fff.
std::int16_t toI16Fixed(float f) {
    f = std::clamp(f, -1.0F, 1.0F);
    const auto i = static_cast<std::int32_t>(f * 32768.0F);
    return static_cast<std::int16_t>(std::clamp(i, -0x8000, 0x7FFF));
}

// Longest prefix of at most `max` bytes that does not split a UTF-8 sequence.
std::size_t utf8Truncation(std::string_view s, std::size_t max) {
    if (s.size() <= max) return s.size();
    std::size_t len = max;
    // Back up over continuation bytes (10xxxxxx) to a character start.
    while (len > 0 && (static_cast<std::uint8_t>(s[len]) & 0xC0) == 0x80) --len;
    return len;
}

}  // namespace

std::string socketName(std::uint32_t scid) {
    return format("scrcpy_%08x", static_cast<unsigned>(scid & 0x7FFFFFFF));
}

std::string serverCommand(const ServerOptions& o) {
    // Same shape as scrcpy's own client (app/src/server.c). No audio; a
    // forward tunnel, so this side connects; clipboard sync off so the control
    // socket carries nothing unasked for; power_on=false so starting the
    // mirror does not wake a phone that is off.
    std::string cmd = std::string{"CLASSPATH="} + kDeviceServerPath +
                      " app_process / com.genymobile.scrcpy.Server " + o.version;
    cmd += format(" scid=%08x", static_cast<unsigned>(o.scid & 0x7FFFFFFF));
    cmd += " log_level=info audio=false tunnel_forward=true clipboard_autosync=false power_on=false";
    if (o.maxSize > 0) cmd += " max_size=" + std::to_string(o.maxSize);
    if (o.maxFps > 0) cmd += " max_fps=" + std::to_string(o.maxFps);
    if (o.videoBitRate > 0) cmd += " video_bit_rate=" + std::to_string(o.videoBitRate);
    return cmd;
}

std::optional<std::uint16_t> parseForwardPort(std::string_view output) {
    const auto port = parseNumber<int>(trim(output));
    if (!port || *port <= 0 || *port > 65535) return std::nullopt;
    return static_cast<std::uint16_t>(*port);
}

Result<CodecMeta> parseCodecMeta(std::string_view bytes) {
    if (bytes.size() < 4) return makeError(ErrorKind::ParseFailure, "no codec header");
    CodecMeta meta;
    meta.codecId = be32(bytes, 0);
    if (meta.codecId == 0) return makeError(ErrorKind::Unsupported, "the device disabled the video stream");
    if (meta.codecId == 1) return makeError(ErrorKind::Io, "the scrcpy server reported a configuration error");
    if (bytes.size() < kCodecMetaLength) return makeError(ErrorKind::ParseFailure, "short codec header");
    meta.width = be32(bytes, 4);
    meta.height = be32(bytes, 8);
    return meta;
}

FrameHeader parseFrameHeader(std::string_view bytes) {
    FrameHeader h;
    if (bytes.size() < kFrameHeaderLength) return h;
    const std::uint64_t ptsAndFlags = (static_cast<std::uint64_t>(be32(bytes, 0)) << 32) | be32(bytes, 4);
    h.config = (ptsAndFlags >> 63) & 1U;
    h.keyFrame = (ptsAndFlags >> 62) & 1U;
    h.pts = static_cast<std::int64_t>(ptsAndFlags & ((std::uint64_t{1} << 62) - 1));
    h.size = be32(bytes, 8);
    return h;
}

std::string parseDeviceName(std::string_view bytes) {
    const auto end = bytes.find('\0');
    return std::string{bytes.substr(0, end)};
}

std::string touchMessage(TouchAction action, std::uint64_t pointerId, std::int32_t x, std::int32_t y,
                         std::uint16_t screenWidth, std::uint16_t screenHeight, float pressure,
                         std::uint32_t actionButton, std::uint32_t buttons) {
    std::string out;
    put8(out, 2);  // SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT
    put8(out, static_cast<std::uint8_t>(action));
    put64(out, pointerId);
    putPosition(out, x, y, screenWidth, screenHeight);
    put16(out, toU16Fixed(pressure));
    put32(out, actionButton);  // 0 for a finger: it has no buttons
    put32(out, buttons);
    return out;                // 32 bytes
}

std::string keycodeMessage(KeyAction action, std::uint32_t keycode, std::uint32_t repeat,
                           std::uint32_t metaState) {
    std::string out;
    put8(out, 0);  // SC_CONTROL_MSG_TYPE_INJECT_KEYCODE
    put8(out, static_cast<std::uint8_t>(action));
    put32(out, keycode);
    put32(out, repeat);
    put32(out, metaState);
    return out;  // 14 bytes
}

std::string textMessage(std::string_view utf8) {
    constexpr std::size_t kMaxText = 300;  // SC_CONTROL_MSG_INJECT_TEXT_MAX_LENGTH
    const std::size_t len = utf8Truncation(utf8, kMaxText);
    std::string out;
    put8(out, 1);  // SC_CONTROL_MSG_TYPE_INJECT_TEXT
    put32(out, static_cast<std::uint32_t>(len));
    out.append(utf8.substr(0, len));
    return out;
}

std::string scrollMessage(std::int32_t x, std::int32_t y, std::uint16_t screenWidth,
                          std::uint16_t screenHeight, float hscroll, float vscroll,
                          std::uint32_t buttons) {
    std::string out;
    put8(out, 3);  // SC_CONTROL_MSG_TYPE_INJECT_SCROLL_EVENT
    putPosition(out, x, y, screenWidth, screenHeight);
    // The client normalises [-16, 16] to [-1, 1] before the fixed-point step.
    put16(out, static_cast<std::uint16_t>(toI16Fixed(hscroll / 16.0F)));
    put16(out, static_cast<std::uint16_t>(toI16Fixed(vscroll / 16.0F)));
    put32(out, buttons);
    return out;  // 21 bytes
}

std::string resetVideoMessage() { return std::string(1, static_cast<char>(17)); }  // TYPE_RESET_VIDEO

std::optional<std::uint32_t> keycodeFromName(std::string_view name) {
    static constexpr std::pair<std::string_view, std::uint32_t> kCodes[] = {
        {"KEYCODE_HOME", 3},          {"KEYCODE_BACK", 4},
        {"KEYCODE_DPAD_UP", 19},      {"KEYCODE_DPAD_DOWN", 20},
        {"KEYCODE_DPAD_LEFT", 21},    {"KEYCODE_DPAD_RIGHT", 22},
        {"KEYCODE_VOLUME_UP", 24},    {"KEYCODE_VOLUME_DOWN", 25},
        {"KEYCODE_POWER", 26},        {"KEYCODE_TAB", 61},
        {"KEYCODE_ENTER", 66},        {"KEYCODE_DEL", 67},
        {"KEYCODE_MENU", 82},         {"KEYCODE_FORWARD_DEL", 112},
        {"KEYCODE_APP_SWITCH", 187},  {"KEYCODE_WAKEUP", 224},
    };
    for (const auto& [n, code] : kCodes) {
        if (n == name) return code;
    }
    return std::nullopt;
}

}  // namespace em::scrcpy
