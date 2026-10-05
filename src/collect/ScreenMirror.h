#pragma once

// A live view of the device's screen, and input sent back to it.
//
// Frames come from `screencap` (raw pixels, no PNG), piped through `gzip -1`
// on the device when it has gzip -- a 1080x2460 frame goes from 10.6 MB to
// under 100 KB on a mostly-static screen, a few MB on a busy one.
//
// Capture is a handful of PERSISTENT `adb exec-out` shells, each running a
// tight `while true; do screencap | gzip -1 | base64 -w0; echo; done` loop on
// the device, read continuously rather than spawned fresh per frame. That
// matters: on a mid-range phone, `screencap` itself costs ~170 ms (SurfaceFlinger
// compositing plus a framebuffer readback), measured with `adb shell time` so
// adb and USB are not part of it. One stream is bound by that number -- about
// 4-5 fps. `screencap` is not internally serialized, though: 3 streams on 3
// cores measured ~12 fps end to end on a real phone over USB, 4 streams ~14,
// 6 streams ~16 -- real gains with sharply diminishing returns. The worker
// count is user-facing for exactly that reason: more workers means more of
// the phone's CPU spent on the mirror instead of whatever is being profiled,
// which fights the point of a profiling tool. Default 3.
//
// Input goes through `input tap|swipe|keyevent|text` on a single worker, so
// taps arrive in the order they were made.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "adb/DeviceManager.h"
#include "collect/ScreenFrame.h"
#include "core/Result.h"
#include "core/Subprocess.h"
#include "core/TcpSocket.h"
#include "core/TaskQueue.h"

namespace em {

// How frames are captured.
//   Scrcpy:    scrcpy's server on the device streams the hardware encoder's
//              H.264, decoded here with FFmpeg. 30-60 fps, real touch input.
//   Screencap: `screencap` loops over adb shell. A few fps, but needs
//              nothing beyond adb -- the fallback when Scrcpy cannot run.
enum class MirrorEngine { Scrcpy, Screencap };

struct MirrorStats {
    bool running = false;
    MirrorEngine engine = MirrorEngine::Screencap;  // the one actually running
    bool compressed = false;   // Screencap only
    int workers = 0;           // Screencap only
    double fps = 0.0;
    std::uint64_t lastFrameBytes = 0;  // as transferred: compressed / encoded
    std::string deviceName;    // Scrcpy only: what the server reported
    std::string notice;        // e.g. why the fast engine fell back
    std::string error;
};

// Raw `screencap` output (not -p): a 12-byte header (width, height, format),
// or 16 bytes from Android 9 on (plus a color space), then the pixels.
// Handles RGBA/RGBX/BGRA 8888 and RGB 565; the result is always RGBA, opaque.
[[nodiscard]] Result<ScreenFrame> parseScreencap(std::string_view raw);

// `input text` treats %s as a space and goes through the device shell. Returns
// the argument to pass, quoted; empty when the text cannot be sent this way
// (input text only handles printable ASCII).
[[nodiscard]] std::string inputTextArgument(std::string_view text);

// The device-side loop a capture worker streams from. Exposed for tests.
[[nodiscard]] std::string captureLoopCommand(bool compressed);

// What a mouse press on the mirror becomes on the device. Pure, so the rules
// are tested without a window.
struct Gesture {
    enum class Kind { Tap, LongPress, Swipe };
    Kind kind = Kind::Tap;
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    int durationMs = 0;  // swipes only
};

inline constexpr float kTapSlopPixels = 6.0F;     // on the computer's screen
inline constexpr double kLongPressSeconds = 0.5;

// A point on the computer's screen, inside an image of the device frame drawn
// at `imageX/Y` and `scale`, in device pixels -- clamped to the frame.
struct DevicePoint {
    float x = 0.0F;
    float y = 0.0F;
};
[[nodiscard]] DevicePoint screenToDevice(float screenX, float screenY, float imageX, float imageY,
                                         float scale, std::uint32_t width, std::uint32_t height);

// Press at `from` and release at `to` (device pixels), the pointer having
// moved `screenDistance` on the computer's screen over `heldSeconds`.
[[nodiscard]] Gesture classifyGesture(DevicePoint from, DevicePoint to, float screenDistance,
                                      double heldSeconds);

class ScreenMirror {
public:
    explicit ScreenMirror(DeviceManager& devices);
    ~ScreenMirror();

    ScreenMirror(const ScreenMirror&) = delete;
    ScreenMirror& operator=(const ScreenMirror&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    // Call every UI frame the mirror is on screen. Capture idles once the
    // calls stop -- a closed or hidden panel then costs the phone nothing,
    // even if nobody called stop().
    void keepAlive() noexcept;

    // Compress on the device. Turned off automatically if it has no gzip.
    // Takes effect on the next start() -- a running capture has already
    // committed its device-side loop to one or the other.
    void setCompression(bool enabled) { compress_.store(enabled); }
    [[nodiscard]] bool compression() const noexcept { return compress_.load(); }

    // Concurrent `screencap` loops on the device. More is faster up to a
    // point (see the class comment) and costs the phone's CPU in exchange;
    // clamped to [1, kMaxWorkers]. Takes effect on the next start().
    void setWorkerCount(int count);
    [[nodiscard]] int workerCount() const noexcept { return targetWorkers_.load(); }
    static constexpr int kMaxWorkers = 6;

    // Which engine the next start() uses. Scrcpy falls back to Screencap by
    // itself, with a notice, when it cannot start.
    void setEngine(MirrorEngine engine) { engine_.store(engine); }
    [[nodiscard]] MirrorEngine engine() const noexcept { return engine_.load(); }
    // The scrcpy server jar and its version, embedded in the app. Without
    // them, or without an H.264 decoder in this build, Scrcpy is unavailable.
    void setScrcpyServer(std::string jar, std::string version);
    [[nodiscard]] bool scrcpyAvailable() const;
    [[nodiscard]] std::string scrcpyUnavailableReason() const;

    // True while a scrcpy control channel is up: input then goes straight to
    // Android's input system as real touch events, so a drag is a drag and
    // not a guessed `input swipe`.
    [[nodiscard]] bool liveTouch() const noexcept { return liveTouch_.load(); }
    enum class Touch { Down, Move, Up };
    void touch(Touch action, int x, int y);       // frame coordinates
    void scroll(int x, int y, float amount);      // wheel notches, + is up

    // The newest frame, shared rather than copied: 10 MB per frame adds up.
    [[nodiscard]] std::shared_ptr<const ScreenFrame> latestFrame() const;
    [[nodiscard]] MirrorStats stats() const;

    // Device coordinates, in the orientation of the frames.
    void tap(int x, int y);
    void longPress(int x, int y);
    void swipe(int x1, int y1, int x2, int y2, int durationMs);
    void key(std::string keycode);  // e.g. "KEYCODE_BACK"
    void text(std::string text);

private:
    void captureLoop(int worker, bool compressed);
    void scrcpySession();
    // Runs the scrcpy session; false (with `failure` set) if it never got a
    // first frame, so the caller can fall back.
    bool runScrcpy(std::string& failure);
    void startScreencapFallback(const std::string& reason);
    void publish(ScreenFrame&& frame, std::uint64_t transferredBytes, MirrorEngine engine,
                 bool compressed);
    bool sendControl(const std::string& message);
    void sendInput(std::string command);

    DeviceManager& devices_;
    std::atomic<bool> running_{false};
    std::atomic<bool> compress_{true};
    std::atomic<int> targetWorkers_{3};
    std::atomic<MirrorEngine> engine_{MirrorEngine::Scrcpy};
    std::atomic<std::int64_t> lastDemandMs_{0};
    std::vector<std::thread> threads_;
    // Screencap workers started by a failed Scrcpy session, joined by stop()
    // after threads_ (whose session thread is the only one adding to it).
    std::mutex fallbackMutex_;
    std::vector<std::thread> fallbackThreads_;

    std::string scrcpyJar_;
    std::string scrcpyVersion_;

    // The scrcpy control channel, written from the UI thread (live touch) and
    // the input worker (taps, swipes, keys).
    std::mutex controlMutex_;
    std::unique_ptr<TcpSocket> control_;
    std::atomic<bool> liveTouch_{false};
    // Size of the frames being streamed: touch positions are given relative
    // to it, and the server ignores one whose size is stale (after rotation).
    std::atomic<std::uint32_t> videoWidth_{0};
    std::atomic<std::uint32_t> videoHeight_{0};

    mutable std::mutex mutex_;
    std::shared_ptr<const ScreenFrame> frame_;
    MirrorStats stats_;
    std::uint64_t sequence_ = 0;
    // Publish times of recent frames, for a frame rate counted over a window
    // rather than from one gap (several streams can land together).
    std::deque<std::chrono::steady_clock::time_point> published_;

    // One worker, so input keeps its order. Declared last: destroyed first,
    // joining any queued input before the rest goes away.
    ThreadPool input_{1};
};

}  // namespace em
