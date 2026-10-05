#include "collect/ScreenMirror.h"

#include <algorithm>
#include <array>
#include <span>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

#include "collect/ScrcpyProtocol.h"
#include "collect/VideoDecoder.h"
#include "core/Gzip.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/StringUtil.h"

namespace em {
namespace {

constexpr const char* kCategory = "mirror";
constexpr std::chrono::milliseconds kInputTimeout{10'000};
// How long a captureLoop() worker waits for the next buffered chunk before
// checking running_/targetWorkers_ again. Short: it is what bounds how fast
// stop() and a worker-count change take effect.
constexpr int kReadPollMs = 150;

std::uint32_t le32(std::string_view b, std::size_t at) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 3])) << 24);
}

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// android::PixelFormat values screencap writes.
namespace {
constexpr std::uint32_t kRgba8888 = 1;
constexpr std::uint32_t kRgbx8888 = 2;
constexpr std::uint32_t kRgb565 = 4;
constexpr std::uint32_t kBgra8888 = 5;
}  // namespace

Result<ScreenFrame> parseScreencap(std::string_view raw) {
    if (raw.size() < 12) return makeError(ErrorKind::ParseFailure, "no frame from screencap");
    const std::uint32_t width = le32(raw, 0);
    const std::uint32_t height = le32(raw, 4);
    const std::uint32_t pixelFormat = le32(raw, 8);
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        // Not a frame header: most likely screencap's own error text.
        std::string_view text = trim(raw.substr(0, std::min<std::size_t>(raw.size(), 200)));
        return makeError(ErrorKind::ParseFailure,
                         text.empty() ? std::string{"screencap returned no image"}
                                      : "screencap: " + std::string{splitLines(text).front()});
    }
    const std::size_t bpp = pixelFormat == kRgb565 ? 2 : 4;
    if (pixelFormat != kRgba8888 && pixelFormat != kRgbx8888 && pixelFormat != kBgra8888 &&
        pixelFormat != kRgb565) {
        return makeError(ErrorKind::Unsupported, format("unsupported pixel format %u", pixelFormat));
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    // The header is 12 bytes before Android 9 and 16 after; the size tells.
    std::size_t header = 0;
    if (raw.size() == 16 + pixels * bpp) {
        header = 16;
    } else if (raw.size() == 12 + pixels * bpp) {
        header = 12;
    } else {
        return makeError(ErrorKind::ParseFailure,
                         format("frame is %zu bytes, expected %zu", raw.size(), 16 + pixels * bpp));
    }

    ScreenFrame frame;
    frame.width = width;
    frame.height = height;
    frame.rgba.resize(pixels * 4);
    const auto* src = reinterpret_cast<const std::uint8_t*>(raw.data() + header);
    std::uint8_t* dst = frame.rgba.data();

    if (pixelFormat == kRgb565) {
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::uint16_t v = static_cast<std::uint16_t>(src[i * 2] | (src[i * 2 + 1] << 8));
            const auto r = static_cast<std::uint8_t>((v >> 11) & 0x1F);
            const auto g = static_cast<std::uint8_t>((v >> 5) & 0x3F);
            const auto b = static_cast<std::uint8_t>(v & 0x1F);
            dst[i * 4 + 0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
            dst[i * 4 + 1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
            dst[i * 4 + 2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
            dst[i * 4 + 3] = 0xFF;
        }
        return frame;
    }
    const bool swapRb = pixelFormat == kBgra8888;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t* p = src + i * 4;
        dst[i * 4 + 0] = swapRb ? p[2] : p[0];
        dst[i * 4 + 1] = p[1];
        dst[i * 4 + 2] = swapRb ? p[0] : p[2];
        // Opaque regardless: the screen has no meaningful alpha, and RGBX
        // leaves it undefined.
        dst[i * 4 + 3] = 0xFF;
    }
    return frame;
}

DevicePoint screenToDevice(float screenX, float screenY, float imageX, float imageY, float scale,
                           std::uint32_t width, std::uint32_t height) {
    if (scale <= 0.0F || width == 0 || height == 0) return {};
    return DevicePoint{std::clamp((screenX - imageX) / scale, 0.0F, static_cast<float>(width - 1)),
                       std::clamp((screenY - imageY) / scale, 0.0F, static_cast<float>(height - 1))};
}

Gesture classifyGesture(DevicePoint from, DevicePoint to, float screenDistance, double heldSeconds) {
    Gesture g;
    g.x1 = static_cast<int>(from.x);
    g.y1 = static_cast<int>(from.y);
    if (screenDistance < kTapSlopPixels) {
        // A little hand jitter is still a tap: measured on the computer's
        // screen, where the hand is, not in device pixels.
        g.kind = heldSeconds >= kLongPressSeconds ? Gesture::Kind::LongPress : Gesture::Kind::Tap;
        g.x2 = g.x1;
        g.y2 = g.y1;
        return g;
    }
    g.kind = Gesture::Kind::Swipe;
    g.x2 = static_cast<int>(to.x);
    g.y2 = static_cast<int>(to.y);
    // The swipe takes as long as the drag did, so a flick stays a flick.
    g.durationMs = std::clamp(static_cast<int>(heldSeconds * 1000.0), 80, 2000);
    return g;
}

std::string inputTextArgument(std::string_view text) {
    std::string arg;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E) return {};
        if (c == ' ') {
            arg += "%s";
        } else {
            arg.push_back(c);
        }
    }
    if (arg.empty()) return {};
    // Single quotes stop the device shell; input itself still sees \ and %
    // escapes, so those are passed through as typed.
    return shellQuote(arg);
}

std::string captureLoopCommand(bool compressed) {
    // `-w0` disables base64 line-wrapping, so each frame is exactly one line:
    // the host can split on '\n' with no further framing needed. Errors go to
    // /dev/null rather than into the stream, so a hiccuped frame is just a
    // short line, not one with garbage mixed in.
    return compressed ? "while true; do screencap | gzip -1 | base64 -w0; echo; done 2>/dev/null"
                      : "while true; do screencap | base64 -w0; echo; done 2>/dev/null";
}

// ---------------------------------------------------------------------------

ScreenMirror::ScreenMirror(DeviceManager& devices) : devices_(devices) {}

ScreenMirror::~ScreenMirror() { stop(); }

void ScreenMirror::setWorkerCount(int count) {
    targetWorkers_.store(std::clamp(count, 1, kMaxWorkers));
}

void ScreenMirror::keepAlive() noexcept { lastDemandMs_.store(steadyMs()); }

void ScreenMirror::start() {
    if (running_.exchange(true)) return;
    keepAlive();
    {
        const std::lock_guard lock{mutex_};
        stats_ = MirrorStats{};
        stats_.running = true;
        published_.clear();
    }
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();

    if (engine_.load() == MirrorEngine::Scrcpy) {
        if (scrcpyAvailable()) {
            threads_.emplace_back([this] { scrcpySession(); });
            return;
        }
        const std::lock_guard lock{mutex_};
        stats_.notice = "fast mode unavailable (" + scrcpyUnavailableReason() + "); using screencap";
    }
    const bool compressed = compress_.load();
    const int workers = targetWorkers_.load();
    for (int i = 0; i < workers; ++i) {
        threads_.emplace_back([this, i, compressed] { captureLoop(i, compressed); });
    }
}

void ScreenMirror::stop() {
    running_.store(false);
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
    {
        // Only a (now finished) session thread adds to this, so no new
        // threads can appear while it is joined.
        const std::lock_guard lock{fallbackMutex_};
        for (auto& t : fallbackThreads_) {
            if (t.joinable()) t.join();
        }
        fallbackThreads_.clear();
    }
    liveTouch_.store(false);
    const std::lock_guard lock{mutex_};
    stats_.running = false;
}

void ScreenMirror::setScrcpyServer(std::string jar, std::string version) {
    scrcpyJar_ = std::move(jar);
    scrcpyVersion_ = std::move(version);
}

bool ScreenMirror::scrcpyAvailable() const {
    return !scrcpyJar_.empty() && !scrcpyVersion_.empty() && VideoDecoder::available();
}

std::string ScreenMirror::scrcpyUnavailableReason() const {
    if (!VideoDecoder::available()) return VideoDecoder::unavailableReason();
    if (scrcpyJar_.empty() || scrcpyVersion_.empty()) return "the scrcpy server is not bundled";
    return {};
}

void ScreenMirror::publish(ScreenFrame&& frame, std::uint64_t transferredBytes, MirrorEngine engine,
                           bool compressed) {
    auto shared = std::make_shared<ScreenFrame>(std::move(frame));
    videoWidth_.store(shared->width);
    videoHeight_.store(shared->height);
    const std::lock_guard lock{mutex_};
    const auto now = std::chrono::steady_clock::now();
    published_.push_back(now);
    while (published_.size() > 2 && now - published_.front() > std::chrono::seconds{3}) {
        published_.pop_front();
    }
    const double spanS = std::chrono::duration<double>(published_.back() - published_.front()).count();
    shared->sequence = ++sequence_;
    frame_ = std::move(shared);
    stats_.error.clear();
    stats_.engine = engine;
    stats_.compressed = compressed;
    stats_.workers = engine == MirrorEngine::Screencap ? targetWorkers_.load() : 0;
    stats_.lastFrameBytes = transferredBytes;
    stats_.fps = spanS > 0.0 ? static_cast<double>(published_.size() - 1) / spanS : 0.0;
}

std::shared_ptr<const ScreenFrame> ScreenMirror::latestFrame() const {
    const std::lock_guard lock{mutex_};
    return frame_;
}

MirrorStats ScreenMirror::stats() const {
    const std::lock_guard lock{mutex_};
    return stats_;
}

void ScreenMirror::captureLoop(int worker, bool compressed) {
    const std::string serial = devices_.selectedSerial();
    if (serial.empty()) {
        const std::lock_guard lock{mutex_};
        stats_.error = "no device selected";
        return;
    }

    const std::vector<std::string> args{"exec-out", captureLoopCommand(compressed)};
    auto spawned = devices_.adb().transport().stream(serial, args, /*pipeStdin=*/false);
    if (!spawned) {
        const std::lock_guard lock{mutex_};
        stats_.error = spawned.error().message;
        return;
    }
    std::unique_ptr<Subprocess> child = std::move(spawned).value();

    // Each worker reads and decodes its own frames independently; only the
    // "is this the newest one" compare-and-swap at the bottom needs the lock.
    std::string buffer;
    std::vector<char> chunk(1U << 16);
    bool toldToSwitchRaw = false;

    while (running_.load()) {
        if (steadyMs() - lastDemandMs_.load() > 1000) {
            // Nobody has called keepAlive() recently -- the panel is hidden
            // or closed. Idle without tearing the stream down, so the phone
            // is not left running a forked `adb exec-out` with nothing
            // reading it, but resuming is instant.
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            continue;
        }
        if (worker != 0 && targetWorkers_.load() <= worker) {
            // The worker count was turned down: this one stops capturing but
            // stays parked (not torn down) so turning it back up is instant
            // too, with no respawn latency.
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            continue;
        }

        bool timedOut = false;
        const auto read = child->readStdout(chunk, kReadPollMs, &timedOut);
        if (!read) {
            const std::lock_guard lock{mutex_};
            stats_.error = read.error().message;
            break;
        }
        if (timedOut) continue;
        if (read.value() == 0) break;  // EOF: the remote shell died
        buffer.append(chunk.data(), read.value());

        std::size_t start = 0;
        for (;;) {
            const auto newline = buffer.find('\n', start);
            if (newline == std::string::npos) break;
            const std::string_view line{buffer.data() + start, newline - start};
            start = newline + 1;
            if (line.empty()) continue;

            const auto decodeFrame = [&](std::string bytes) -> Result<ScreenFrame> {
                if (compressed) {
                    EM_TRY(raw, gunzip(bytes));
                    return parseScreencap(raw);
                }
                return parseScreencap(bytes);
            };
            auto decoded = base64Decode(line);
            Result<ScreenFrame> parsed =
                decoded ? decodeFrame(*std::move(decoded))
                        : Result<ScreenFrame>{makeError(ErrorKind::ParseFailure, "not valid base64")};

            if (!parsed) {
                if (compressed && !toldToSwitchRaw) {
                    // The device most likely has no usable gzip. Rather than
                    // guess and restart mid-session, surface it: the panel
                    // offers turning Compress off, which respawns clean.
                    EM_LOG_WARN(kCategory, "worker " + std::to_string(worker) +
                                              ": " + parsed.error().message +
                                              " -- try turning off Compress");
                    toldToSwitchRaw = true;
                }
                continue;
            }

            publish(std::move(parsed).value(), line.size(), MirrorEngine::Screencap, compressed);
        }
        buffer.erase(0, start);
    }

    child->terminate();
    (void)child->wait(std::chrono::milliseconds{500});
    child->killHard();
}

// ---------------------------------------------------------------------------
// Scrcpy engine
// ---------------------------------------------------------------------------

namespace {

constexpr int kSocketPollMs = 200;  // bounds how fast stop() is noticed

// Reads exactly dst.size() bytes, checking `keepGoing` between waits so a
// stop() is honoured within kSocketPollMs. Partial progress survives a
// timeout, so the stream never loses its framing.
Status readFully(TcpSocket& sock, std::span<char> dst, const std::atomic<bool>& keepGoing) {
    std::size_t got = 0;
    while (got < dst.size()) {
        if (!keepGoing.load()) return makeError(ErrorKind::Io, "stopped");
        auto n = sock.recvSome(dst.subspan(got), kSocketPollMs);
        if (!n) {
            if (n.error().kind == ErrorKind::Timeout) continue;
            return n.error();
        }
        if (n.value() == 0) return makeError(ErrorKind::Io, "the device closed the video stream");
        got += n.value();
    }
    return {};
}

// What the server printed, for error messages: its last few lines.
std::string drainOutput(Subprocess& server, std::string& log) {
    // Both pipes: the server logs errors (a version mismatch, a missing
    // encoder) to stderr, and an unread pipe that fills would block it.
    std::array<char, 4096> buf{};
    for (const bool err : {false, true}) {
        for (;;) {
            bool timedOut = false;
            auto n = err ? server.readStderr(buf, 0, &timedOut) : server.readStdout(buf, 0, &timedOut);
            if (!n || timedOut || n.value() == 0) break;
            log.append(buf.data(), n.value());
        }
    }
    if (log.size() > 8192) log.erase(0, log.size() - 8192);
    return log;
}

std::string lastServerError(const std::string& log) {
    std::string found;
    for (const auto line : splitLines(log)) {
        const auto t = trim(line);
        // "[server] ERROR: The server version (3.3.4) does not match the client (...)"
        if (const auto at = t.find("ERROR: "); at != std::string_view::npos) {
            found = std::string{t.substr(at + 7)};
        } else if (found.empty() && t.find("Exception") != std::string_view::npos) {
            found = std::string{t};
        }
    }
    return found;
}

}  // namespace

void ScreenMirror::scrcpySession() {
    std::string failure;
    if (runScrcpy(failure) || !running_.load()) return;
    EM_LOG_WARN(kCategory, "fast mirror failed: " + failure + " -- falling back to screencap");
    startScreencapFallback(failure);
}

void ScreenMirror::startScreencapFallback(const std::string& reason) {
    {
        const std::lock_guard lock{mutex_};
        stats_.notice = "fast mode failed (" + reason + "); using screencap";
        stats_.error.clear();
    }
    const bool compressed = compress_.load();
    const int workers = targetWorkers_.load();
    {
        const std::lock_guard lock{fallbackMutex_};
        for (int i = 1; i < workers; ++i) {
            fallbackThreads_.emplace_back([this, i, compressed] { captureLoop(i, compressed); });
        }
    }
    captureLoop(0, compressed);  // this thread is worker 0
}

bool ScreenMirror::runScrcpy(std::string& failure) {
    const std::string serial = devices_.selectedSerial();
    if (serial.empty()) {
        failure = "no device selected";
        return false;
    }
    IAdbTransport& transport = devices_.adb().transport();

    // 1. Push the server. It is ~90 KB, and it deletes itself once started,
    //    so it is pushed every session and nothing is left on the device.
    const auto local = std::filesystem::temp_directory_path() /
                       ("emeron-scrcpy-server-" + scrcpyVersion_ + ".jar");
    {
        std::error_code ec;
        if (std::filesystem::file_size(local, ec) != scrcpyJar_.size()) {
            std::ofstream out{local, std::ios::binary | std::ios::trunc};
            out.write(scrcpyJar_.data(), static_cast<std::streamsize>(scrcpyJar_.size()));
            if (!out) {
                failure = "cannot write " + local.string();
                return false;
            }
        }
    }
    if (auto pushed = devices_.adb().push(serial, local, scrcpy::kDeviceServerPath); !pushed) {
        failure = "push failed: " + pushed.error().message;
        return false;
    }

    // 2. A forward tunnel to a socket named after a random session id, so a
    //    scrcpy the user runs alongside does not collide with this one.
    std::random_device rd;
    const std::uint32_t scid = rd() & 0x7FFFFFFF;
    const std::string socket = "localabstract:" + scrcpy::socketName(scid);
    const std::vector<std::string> forwardArgs{"forward", "tcp:0", socket};
    auto forwarded = transport.device(serial, forwardArgs, kAdbNormalTimeout);
    const auto port = forwarded ? scrcpy::parseForwardPort(forwarded.value().out) : std::nullopt;
    if (!port) {
        failure = "adb forward failed" + (forwarded ? ": " + std::string{trim(forwarded.value().out + forwarded.value().err)}
                                                    : ": " + forwarded.error().message);
        return false;
    }
    struct ForwardGuard {
        IAdbTransport& t;
        std::string serial;
        std::uint16_t port;
        ~ForwardGuard() {
            const std::vector<std::string> args{"forward", "--remove", "tcp:" + std::to_string(port)};
            (void)t.device(serial, args, kAdbQuickTimeout);
        }
    } forwardGuard{transport, serial, *port};

    // 3. Start the server. Its output is drained as we go: a full pipe would
    //    block its logging, and with it the server.
    scrcpy::ServerOptions options;
    options.version = scrcpyVersion_;
    options.scid = scid;
    const std::vector<std::string> shellArgs{"shell", scrcpy::serverCommand(options)};
    auto spawned = transport.stream(serial, shellArgs, /*pipeStdin=*/false);
    if (!spawned) {
        failure = "cannot start the server: " + spawned.error().message;
        return false;
    }
    std::unique_ptr<Subprocess> server = std::move(spawned).value();
    std::string serverLog;
    struct ServerGuard {
        Subprocess& s;
        ~ServerGuard() {
            s.terminate();
            (void)s.wait(std::chrono::milliseconds{500});
            s.killHard();
        }
    } serverGuard{*server};

    // 4. Connect the video socket. With a forward tunnel the connect itself
    //    succeeds even before the server listens; the dummy byte is what
    //    proves the server is there, so retry until it arrives.
    TcpSocket video;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{8};
    for (;;) {
        if (!running_.load()) return true;  // stopped while starting: not a failure
        if (std::chrono::steady_clock::now() > deadline) {
            drainOutput(*server, serverLog);
            const std::string why = lastServerError(serverLog);
            failure = why.empty() ? "the server did not answer within 8 s" : why;
            return false;
        }
        if (server->tryWait()) {
            drainOutput(*server, serverLog);
            const std::string why = lastServerError(serverLog);
            failure = why.empty() ? std::string{"the server exited"} : why;
            return false;
        }
        auto connected = TcpSocket::connect("127.0.0.1", *port, 1000);
        if (connected) {
            char dummy = 0;
            if (connected.value().recvExact({&dummy, 1}, 1500)) {
                video = std::move(connected).value();
                break;
            }
        }
        drainOutput(*server, serverLog);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    // 5. The control socket: the second one opened, so no dummy byte.
    auto controlConnected = TcpSocket::connect("127.0.0.1", *port, 2000);
    if (!controlConnected) {
        failure = "control socket: " + controlConnected.error().message;
        return false;
    }

    // 6. Device name (first socket only), then the codec header.
    std::string meta(scrcpy::kDeviceNameLength, '\0');
    std::string codecBytes(scrcpy::kCodecMetaLength, '\0');
    if (auto st = readFully(video, meta, running_); !st) {
        failure = st.error().message;
        return false;
    }
    if (auto st = readFully(video, std::span<char>{codecBytes.data(), 4}, running_); !st) {
        failure = st.error().message;
        return false;
    }
    if (const auto first = scrcpy::parseCodecMeta(std::string_view{codecBytes}.substr(0, 4));
        !first && first.error().kind != ErrorKind::ParseFailure) {
        failure = first.error().message;  // disabled or configuration error
        return false;
    }
    if (auto st = readFully(video, std::span<char>{codecBytes.data() + 4, 8}, running_); !st) {
        failure = st.error().message;
        return false;
    }
    auto codec = scrcpy::parseCodecMeta(codecBytes);
    if (!codec) {
        failure = codec.error().message;
        return false;
    }
    if (codec.value().codecId != scrcpy::kCodecH264) {
        failure = "unexpected codec from the server";
        return false;
    }
    auto decoder = VideoDecoder::createH264();
    if (!decoder) {
        failure = decoder.error().message;
        return false;
    }
    {
        const std::lock_guard lock{controlMutex_};
        control_ = std::make_unique<TcpSocket>(std::move(controlConnected).value());
    }
    liveTouch_.store(true);
    {
        const std::lock_guard lock{mutex_};
        stats_.deviceName = scrcpy::parseDeviceName(meta);
        stats_.engine = MirrorEngine::Scrcpy;
        stats_.notice.clear();
    }
    EM_LOG_INFO(kCategory, "fast mirror connected to " + scrcpy::parseDeviceName(meta) + ", " +
                               std::to_string(codec.value().width) + "x" +
                               std::to_string(codec.value().height));

    // 7. Packets: 12-byte header, then the payload. A config packet (SPS/PPS)
    //    is held and prepended to the next one, as scrcpy's own client does.
    bool gotFrame = false;
    bool idle = false;
    std::string config;
    std::string header(scrcpy::kFrameHeaderLength, '\0');
    std::string packet;
    std::string error;
    while (running_.load()) {
        if (auto st = readFully(video, header, running_); !st) {
            error = st.error().message;
            break;
        }
        const auto h = scrcpy::parseFrameHeader(header);
        if (h.size > (64U << 20)) {
            error = "corrupt stream (packet of " + std::to_string(h.size) + " bytes)";
            break;
        }
        packet.resize(h.size);
        if (auto st = readFully(video, packet, running_); !st) {
            error = st.error().message;
            break;
        }
        drainOutput(*server, serverLog);

        if (h.config) {
            config = packet;
            continue;
        }
        // Nobody is looking (panel hidden or closed): keep reading so the
        // server is not blocked, but skip decoding. On return, ask for a key
        // frame -- the skipped packets were references for what follows.
        const bool wanted = steadyMs() - lastDemandMs_.load() <= 1000;
        if (!wanted) {
            idle = true;
            continue;
        }
        if (idle) {
            idle = false;
            auto fresh = VideoDecoder::createH264();
            if (fresh) decoder = std::move(fresh);
            config.clear();
            sendControl(scrcpy::resetVideoMessage());
            continue;  // decode resumes at the config packet the reset sends
        }

        const std::string_view data = config.empty() ? std::string_view{packet}
                                                     : std::string_view{config.append(packet)};
        const std::uint64_t bytes = data.size();
        auto st = decoder.value()->decode(data, h.pts, [&](ScreenFrame&& frame) {
            gotFrame = true;
            publish(std::move(frame), bytes, MirrorEngine::Scrcpy, false);
        });
        config.clear();
        if (!st) EM_LOG_DEBUG(kCategory, st.error().message);  // one bad packet is not fatal
    }

    liveTouch_.store(false);
    {
        const std::lock_guard lock{controlMutex_};
        if (control_) control_->close();
        control_.reset();
    }
    if (!running_.load()) return true;  // stopped by the user
    if (!gotFrame) {
        drainOutput(*server, serverLog);
        const std::string why = lastServerError(serverLog);
        failure = why.empty() ? error : why;
        return false;
    }
    // It worked and then the stream ended (unplugged, screen-off policy...):
    // report it rather than silently switching engines mid-session.
    const std::lock_guard lock{mutex_};
    stats_.error = "fast mirror stopped: " + error;
    return true;
}

bool ScreenMirror::sendControl(const std::string& message) {
    const std::lock_guard lock{controlMutex_};
    if (!control_) return false;
    if (auto st = control_->sendAll(message); !st) {
        EM_LOG_WARN(kCategory, "control: " + st.error().message);
        return false;
    }
    return true;
}

void ScreenMirror::touch(Touch action, int x, int y) {
    const auto w = static_cast<std::uint16_t>(videoWidth_.load());
    const auto h = static_cast<std::uint16_t>(videoHeight_.load());
    if (w == 0 || h == 0) return;
    const auto a = action == Touch::Down ? scrcpy::TouchAction::Down
                   : action == Touch::Move ? scrcpy::TouchAction::Move
                                           : scrcpy::TouchAction::Up;
    sendControl(scrcpy::touchMessage(a, scrcpy::kPointerGenericFinger, x, y, w, h,
                                     action == Touch::Up ? 0.0F : 1.0F));
}

void ScreenMirror::scroll(int x, int y, float amount) {
    const auto w = static_cast<std::uint16_t>(videoWidth_.load());
    const auto h = static_cast<std::uint16_t>(videoHeight_.load());
    if (w == 0 || h == 0) return;
    sendControl(scrcpy::scrollMessage(x, y, w, h, 0.0F, std::clamp(amount, -16.0F, 16.0F)));
}

void ScreenMirror::sendInput(std::string command) {
    const std::string serial = devices_.selectedSerial();
    if (serial.empty()) return;
    input_.submit([this, serial, command = std::move(command)] {
        auto result = devices_.adb().shellRaw(serial, command, kInputTimeout);
        if (!result) {
            EM_LOG_WARN(kCategory, command + ": " + result.error().message);
        } else if (!result.value().ok()) {
            EM_LOG_WARN(kCategory, command + ": " + std::string{result.value().firstErrLine()});
        }
    });
}

void ScreenMirror::tap(int x, int y) {
    if (liveTouch_.load()) {
        touch(Touch::Down, x, y);
        touch(Touch::Up, x, y);
        return;
    }
    sendInput("input tap " + std::to_string(x) + " " + std::to_string(y));
}

void ScreenMirror::longPress(int x, int y) {
    // A swipe that does not move is a long press.
    swipe(x, y, x, y, 700);
}

void ScreenMirror::swipe(int x1, int y1, int x2, int y2, int durationMs) {
    if (liveTouch_.load()) {
        // Played as a real finger: down, moves every ~16 ms, up -- on the
        // input worker, so the UI is not held for the duration.
        input_.submit([this, x1, y1, x2, y2, durationMs] {
            const int steps = std::max(1, durationMs / 16);
            touch(Touch::Down, x1, y1);
            for (int i = 1; i <= steps; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds{durationMs / steps});
                touch(Touch::Move, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
            }
            touch(Touch::Up, x2, y2);
        });
        return;
    }
    sendInput("input swipe " + std::to_string(x1) + " " + std::to_string(y1) + " " +
              std::to_string(x2) + " " + std::to_string(y2) + " " + std::to_string(durationMs));
}

void ScreenMirror::key(std::string keycode) {
    if (!std::all_of(keycode.begin(), keycode.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        })) {
        return;
    }
    if (liveTouch_.load()) {
        if (const auto code = scrcpy::keycodeFromName(keycode)) {
            sendControl(scrcpy::keycodeMessage(scrcpy::KeyAction::Down, *code));
            sendControl(scrcpy::keycodeMessage(scrcpy::KeyAction::Up, *code));
            return;
        }
    }
    sendInput("input keyevent " + keycode);
}

void ScreenMirror::text(std::string text) {
    if (liveTouch_.load() && !text.empty()) {
        sendControl(scrcpy::textMessage(text));
        return;
    }
    const std::string arg = inputTextArgument(text);
    if (arg.empty()) {
        EM_LOG_WARN(kCategory, "input text only sends printable ASCII; nothing sent");
        return;
    }
    sendInput("input text " + arg);
}

}  // namespace em
