#pragma once

// Frame-time and jank analysis from `dumpsys gfxinfo <pkg> framestats`.
//
// The PROFILEDATA block holds the last ~120 frames, each a row of nanosecond
// timestamps. Column sets differ between Android releases (FrameDeadline,
// GpuCompleted and DisplayPresentTime were all added over time), so the header
// is parsed into a name -> index map rather than assuming fixed positions.
//
// The collector polls faster than the 120-frame ring fills and de-duplicates by
// IntendedVsync, which lets it accumulate a long history the device itself
// cannot keep.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "adb/DeviceManager.h"
#include "core/Result.h"
#include "core/TaskQueue.h"

namespace em {

// Flags column. A non-zero flags value means the platform itself excludes the
// frame from its jank statistics, so we do too.
// android::uirenderer::FrameInfoFlags. An earlier version had 0x2 labelled
// SurfaceCanvas and excluded every flagged frame; 0x2 is RTAnimation -- ordinary
// render-thread animation frames, a third of all frames in a real capture.
enum class FrameFlag : std::uint64_t {
    WindowVisibilityChanged = 0x1,
    RTAnimation = 0x2,
    SurfaceCanvas = 0x4,
    SkippedFrame = 0x8,
};

// The flags Android itself leaves out of jank accounting. Frames with only
// WindowVisibilityChanged or RTAnimation set count -- that is what makes the
// janky count match dumpsys gfxinfo's.
inline constexpr std::uint64_t kExcludedFrameFlags =
    static_cast<std::uint64_t>(FrameFlag::SurfaceCanvas) |
    static_cast<std::uint64_t>(FrameFlag::SkippedFrame);

struct FrameRecord {
    std::uint64_t flags = 0;
    std::uint64_t intendedVsyncNs = 0;
    std::uint64_t vsyncNs = 0;
    std::uint64_t frameCompletedNs = 0;
    std::uint64_t frameDeadlineNs = 0;
    std::uint64_t gpuCompletedNs = 0;

    double startSec = 0.0;  // relative to the first frame in the snapshot
    double totalMs = 0.0;   // IntendedVsync -> FrameCompleted

    // Phase breakdown. -1 when the source columns are absent on this release.
    double inputMs = -1.0;
    double animationMs = -1.0;
    double layoutMs = -1.0;   // PerformTraversals -> Draw
    double drawMs = -1.0;     // Draw -> SyncQueued  (record display list)
    double syncMs = -1.0;     // SyncStart -> IssueDrawCommands
    double issueMs = -1.0;    // IssueDrawCommands -> SwapBuffers
    double swapMs = -1.0;     // SwapBuffers -> FrameCompleted
    double gpuMs = -1.0;      // SwapBuffers -> GpuCompleted
    double dequeueMs = -1.0;
    double queueMs = -1.0;

    bool excluded = false;  // flags & kExcludedFrameFlags

    // The window that drew it, e.g. "com.example.app/com.example.app.MainActivity".
    // gfxinfo reports one block of frames per window; all of them are kept.
    std::string window;
    bool janky = false;

    [[nodiscard]] double vsyncLatencyMs() const noexcept {
        return static_cast<double>(vsyncNs - intendedVsyncNs) / 1e6;
    }
};

// The human-readable block above PROFILEDATA, as the platform computed it.
// Worth showing next to our own numbers: if they disagree the window is wrong.
struct GfxSummary {
    long totalFrames = -1;
    long jankyFrames = -1;
    double jankyPct = -1.0;
    double p50Ms = -1.0, p90Ms = -1.0, p95Ms = -1.0, p99Ms = -1.0;
    long missedVsync = -1;
    long highInputLatency = -1;
    long slowUiThread = -1;
    long slowBitmapUploads = -1;
    long slowDrawCommands = -1;
    long deadlineMissed = -1;
    int pid = -1;
    std::string packageName;

    [[nodiscard]] bool valid() const noexcept { return totalFrames >= 0; }
};

struct JankStats {
    std::size_t frameCount = 0;
    std::size_t jankCount = 0;
    double jankPct = 0.0;

    double p50Ms = 0.0, p90Ms = 0.0, p95Ms = 0.0, p99Ms = 0.0;
    double meanMs = 0.0, maxMs = 0.0;
    double refreshPeriodMs = 16.667;

    // Severity buckets, in multiples of the refresh period. These are what a
    // user feels: one dropped frame is a hitch, five is a freeze.
    std::size_t within1 = 0;   // on time
    std::size_t within2 = 0;   // 1 frame late
    std::size_t within4 = 0;
    std::size_t beyond4 = 0;

    double estimatedFps = 0.0;  // frames / wall time covered

    // How many frames carried their own FrameDeadline (Android 12+), and so
    // were judged by it rather than by duration. Shown so the user knows which
    // definition of "janky" the number uses.
    std::size_t deadlineFrames = 0;
};

struct FrameStatsSnapshot {
    std::string packageName;
    int pid = -1;
    double refreshPeriodMs = 16.667;
    std::vector<FrameRecord> frames;
    GfxSummary summary;
    JankStats stats;
    std::vector<std::string> columns;
    std::string lastError;
    std::uint64_t pollCount = 0;
};

// --- parsers, exposed for unit tests ---------------------------------------

using ColumnIndex = std::unordered_map<std::string, std::size_t>;

// Extracts the CSV of the first ---PROFILEDATA--- block.
[[nodiscard]] std::optional<std::string_view> extractProfileData(std::string_view dump);

// Every PROFILEDATA block with the window it belongs to. Android 12+ prints a
// process-wide summary followed by one section per window ("Window: ..."), each
// with its own block -- reading only the first showed a back-stack activity's
// frames instead of the one on screen.
struct ProfileBlock {
    std::string_view window;  // empty on releases without per-window sections
    std::string_view body;
};
[[nodiscard]] std::vector<ProfileBlock> extractProfileBlocks(std::string_view dump);

[[nodiscard]] ColumnIndex parseFrameStatsHeader(std::string_view headerLine);

// `refreshPeriodMs <= 0` asks the parser to infer it from FrameDeadline.
struct ParsedFrameStats {
    std::vector<FrameRecord> frames;
    std::vector<std::string> columns;
    double refreshPeriodMs = 16.667;
};
[[nodiscard]] Result<ParsedFrameStats> parseFrameStats(std::string_view dump,
                                                       double refreshPeriodMs = -1.0);

[[nodiscard]] GfxSummary parseGfxSummary(std::string_view dump);

[[nodiscard]] JankStats computeJankStats(const std::vector<FrameRecord>& frames,
                                         double refreshPeriodMs);

// Whether a frame was janky. With a FrameDeadline (Android 12+) that means it
// completed after its deadline -- the definition dumpsys gfxinfo uses, which
// allows for the pipelining that lets a frame take longer than one refresh
// without being dropped. Without one, it took longer than one refresh.
[[nodiscard]] bool missedDeadline(const FrameRecord& frame, double refreshPeriodMs) noexcept;

// Refresh intervals the frame missed by, under the same rule: 0 = on time.
[[nodiscard]] int framesLate(const FrameRecord& frame, double refreshPeriodMs) noexcept;

// ---------------------------------------------------------------------------

class FrameStatsCollector {
public:
    explicit FrameStatsCollector(DeviceManager& devices);
    ~FrameStatsCollector();

    FrameStatsCollector(const FrameStatsCollector&) = delete;
    FrameStatsCollector& operator=(const FrameStatsCollector&) = delete;
    FrameStatsCollector(FrameStatsCollector&&) = delete;
    FrameStatsCollector& operator=(FrameStatsCollector&&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    void setInterval(std::chrono::milliseconds interval);
    [[nodiscard]] std::chrono::milliseconds interval() const;

    void setMaxFrames(std::size_t frames);
    [[nodiscard]] std::size_t maxFrames() const;

    // Overrides the inferred display refresh period. <= 0 restores inference.
    void setRefreshPeriodOverrideMs(double ms);
    [[nodiscard]] double refreshPeriodOverrideMs() const;

    // Clears the device-side ring and our accumulated history.
    void requestReset();

    template <typename Fn>
    void readSnapshot(Fn&& fn) const {
        snapshot_.read(std::forward<Fn>(fn));
    }
    [[nodiscard]] FrameStatsSnapshot snapshot() const { return snapshot_.get(); }

private:
    void pollLoop();
    void pollOnce(std::string_view serial, std::string_view packageName);

    DeviceManager& devices_;
    Shared<FrameStatsSnapshot> snapshot_;

    // Frames already stored, per window, by IntendedVsync. A set rather than a
    // high-water mark: a frame still in flight at one poll completes by the
    // next, and is then older than frames stored meanwhile -- a "newer than the
    // last one" rule dropped it for good. Per window, because an activity and
    // a dialog over it can draw on the same vsync. Pruned to recent entries.
    std::unordered_map<std::string, std::set<std::uint64_t>> seenVsyncsByWindow_;
    std::uint64_t firstVsyncNs_ = 0;
    std::string trackedPackage_;

    std::atomic<bool> running_{false};
    std::atomic<bool> resetRequested_{false};
    std::atomic<std::int64_t> intervalMs_{1000};
    std::atomic<std::size_t> maxFrames_{4096};
    std::atomic<double> refreshOverrideMs_{-1.0};

    std::mutex sleepMutex_;
    std::condition_variable sleepCv_;
    std::thread thread_;
};

}  // namespace em
