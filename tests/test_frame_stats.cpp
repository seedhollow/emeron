#include "TestHarness.h"

#include "collect/FrameStats.h"

using namespace em;

namespace {

// A realistic Android 13 gfxinfo dump: 60 Hz, three frames, the middle one
// taking 33 ms (one dropped frame).
constexpr const char* kGfxDump = R"DUMP(
** Graphics info for pid 12345 [com.example.app] **

Stats since: 1234567890ns
Total frames rendered: 1200
Janky frames: 36 (3.00%)
50th percentile: 7ms
90th percentile: 12ms
95th percentile: 18ms
99th percentile: 34ms
Number Missed Vsync: 4
Number High input latency: 1
Number Slow UI thread: 22
Number Slow bitmap uploads: 0
Number Slow issue draw commands: 9
Number Frame deadline missed: 30

---PROFILEDATA---
Flags,IntendedVsync,Vsync,OldestInputEvent,NewestInputEvent,HandleInputStart,AnimationStart,PerformTraversalsStart,DrawStart,FrameDeadline,FrameStartTime,SyncQueued,SyncStart,IssueDrawCommandsStart,SwapBuffers,FrameCompleted,DequeueBufferDuration,QueueBufferDuration,GpuCompleted
0,1000000000,1000100000,0,0,1000200000,1000300000,1000400000,1000900000,1016666667,1000150000,1002000000,1002100000,1002500000,1004000000,1005000000,120000,80000,1006000000
0,1016666667,1016766667,0,0,1016800000,1017000000,1017200000,1018000000,1033333334,1016700000,1030000000,1030200000,1031000000,1048000000,1049666667,150000,90000,1051000000
1,1033333334,1033433334,0,0,1033500000,1033600000,1033700000,1034000000,1050000001,1033400000,1035000000,1035100000,1035400000,1036000000,1037000000,100000,70000,1038000000
---PROFILEDATA---
)DUMP";

}  // namespace

TEST_CASE("extractProfileData finds the block between markers") {
    const auto body = extractProfileData(kGfxDump);
    CHECK(body.has_value());
    if (body) {
        CHECK(body->starts_with("Flags,IntendedVsync"));
        CHECK(body->find("---PROFILEDATA---") == std::string_view::npos);
    }
    CHECK(!extractProfileData("no markers here").has_value());
}

TEST_CASE("parseFrameStatsHeader maps names to positions") {
    const auto index = parseFrameStatsHeader("Flags,IntendedVsync,Vsync,FrameCompleted");
    CHECK_EQ(index.at("Flags"), std::size_t{0});
    CHECK_EQ(index.at("IntendedVsync"), std::size_t{1});
    CHECK_EQ(index.at("FrameCompleted"), std::size_t{3});
}

TEST_CASE("parseFrameStats reads durations and phases") {
    auto parsed = parseFrameStats(kGfxDump);
    CHECK(parsed.hasValue());
    if (!parsed) return;

    const ParsedFrameStats& stats = parsed.value();
    CHECK_EQ(stats.frames.size(), std::size_t{3});

    // Refresh period inferred from FrameDeadline - IntendedVsync.
    CHECK_NEAR(stats.refreshPeriodMs, 16.6666, 0.01);

    const FrameRecord& first = stats.frames[0];
    CHECK_NEAR(first.totalMs, 5.0, 0.001);
    CHECK(!first.excluded);
    CHECK(!first.janky);
    CHECK_NEAR(first.inputMs, 0.1, 0.001);        // 1000300000 - 1000200000
    CHECK_NEAR(first.animationMs, 0.1, 0.001);
    CHECK_NEAR(first.layoutMs, 0.5, 0.001);       // DrawStart - PerformTraversals
    CHECK_NEAR(first.drawMs, 1.1, 0.001);         // SyncQueued - DrawStart
    CHECK_NEAR(first.issueMs, 1.5, 0.001);        // SwapBuffers - IssueDrawCommands
    CHECK_NEAR(first.swapMs, 1.0, 0.001);         // FrameCompleted - SwapBuffers
    CHECK_NEAR(first.gpuMs, 2.0, 0.001);          // GpuCompleted - SwapBuffers
    CHECK_NEAR(first.dequeueMs, 0.12, 0.001);
    CHECK_NEAR(first.queueMs, 0.08, 0.001);

    // Second frame: 1049666667 - 1016666667 = 33 ms, over a 16.67 ms budget.
    const FrameRecord& second = stats.frames[1];
    CHECK_NEAR(second.totalMs, 33.0, 0.001);
    CHECK(second.janky);

    // Third frame has flags=0x1 (WindowVisibilityChanged). Android still counts
    // those -- an earlier version excluded every flagged frame and so disagreed
    // with dumpsys gfxinfo's janky count. It finished well inside its deadline.
    const FrameRecord& third = stats.frames[2];
    CHECK(!third.excluded);
    CHECK(!third.janky);

    // startSec is relative to the first frame.
    CHECK_NEAR(stats.frames[0].startSec, 0.0, 1e-9);
    CHECK_NEAR(stats.frames[1].startSec, 0.016666667, 1e-6);
}

TEST_CASE("parseFrameStats honours an explicit refresh period") {
    auto parsed = parseFrameStats(kGfxDump, 8.333);
    CHECK(parsed.hasValue());
    if (!parsed) return;
    CHECK_NEAR(parsed.value().refreshPeriodMs, 8.333, 1e-6);
    // At 120 Hz the 5 ms frame is still fine but nothing else is.
    CHECK(!parsed.value().frames[0].janky);
    CHECK(parsed.value().frames[1].janky);
}

TEST_CASE("parseFrameStats reports missing or empty data honestly") {
    auto noBlock = parseFrameStats("** Graphics info for pid 1 [x] **\n");
    CHECK(!noBlock.hasValue());
    if (!noBlock) CHECK(noBlock.error().kind == ErrorKind::ParseFailure);

    auto headerOnly = parseFrameStats(
        "---PROFILEDATA---\nFlags,IntendedVsync,FrameCompleted\n---PROFILEDATA---\n");
    // A header with no rows means nothing drawn yet: a waiting state, not
    // an error (see "an app that has drawn nothing since a reset ...").
    CHECK(headerOnly.hasValue());
    if (headerOnly) CHECK(headerOnly.value().frames.empty());
}

TEST_CASE("parseFrameStats tolerates an unknown column set") {
    // Only the two required columns; every phase must come back as n/a.
    constexpr const char* kMinimal =
        "---PROFILEDATA---\n"
        "Flags,IntendedVsync,FrameCompleted\n"
        "0,1000000000,1010000000\n"
        "---PROFILEDATA---\n";

    auto parsed = parseFrameStats(kMinimal);
    CHECK(parsed.hasValue());
    if (!parsed) return;

    const FrameRecord& frame = parsed.value().frames.at(0);
    CHECK_NEAR(frame.totalMs, 10.0, 0.001);
    CHECK_NEAR(frame.inputMs, -1.0, 1e-9);
    CHECK_NEAR(frame.gpuMs, -1.0, 1e-9);
    // No FrameDeadline column, so the period falls back to 60 Hz.
    CHECK_NEAR(parsed.value().refreshPeriodMs, 16.667, 0.001);
}

TEST_CASE("parseGfxSummary reads the text block") {
    const GfxSummary summary = parseGfxSummary(kGfxDump);
    CHECK(summary.valid());
    CHECK_EQ(summary.pid, 12345);
    CHECK_EQ(summary.packageName, std::string{"com.example.app"});
    CHECK_EQ(summary.totalFrames, 1200L);
    CHECK_EQ(summary.jankyFrames, 36L);
    CHECK_NEAR(summary.jankyPct, 3.0, 0.001);
    CHECK_NEAR(summary.p50Ms, 7.0, 0.001);
    CHECK_NEAR(summary.p99Ms, 34.0, 0.001);
    CHECK_EQ(summary.missedVsync, 4L);
    CHECK_EQ(summary.slowUiThread, 22L);
    CHECK_EQ(summary.deadlineMissed, 30L);
}

TEST_CASE("parseGfxSummary on an unrelated dump") {
    const GfxSummary summary = parseGfxSummary("nothing to see");
    CHECK(!summary.valid());
}

TEST_CASE("computeJankStats buckets by refresh multiples") {
    std::vector<FrameRecord> frames;
    const auto add = [&frames](double ms, bool excluded = false) {
        FrameRecord frame;
        frame.totalMs = ms;
        frame.excluded = excluded;
        frame.startSec = static_cast<double>(frames.size()) * 0.016;
        frames.push_back(frame);
    };

    add(8.0);    // on time
    add(10.0);   // on time
    add(20.0);   // 1 frame late
    add(50.0);   // 2-4 frames
    add(200.0);  // >4 frames
    add(999.0, /*excluded=*/true);

    const JankStats stats = computeJankStats(frames, 16.667);
    CHECK_EQ(stats.frameCount, std::size_t{5});
    CHECK_EQ(stats.within1, std::size_t{2});
    CHECK_EQ(stats.within2, std::size_t{1});
    CHECK_EQ(stats.within4, std::size_t{1});
    CHECK_EQ(stats.beyond4, std::size_t{1});
    CHECK_EQ(stats.jankCount, std::size_t{3});
    CHECK_NEAR(stats.jankPct, 60.0, 0.001);
    CHECK_NEAR(stats.maxMs, 200.0, 0.001);  // the excluded frame must not count
    CHECK_NEAR(stats.meanMs, (8.0 + 10.0 + 20.0 + 50.0 + 200.0) / 5.0, 0.001);
}

TEST_CASE("computeJankStats on no frames") {
    const JankStats stats = computeJankStats({}, 16.667);
    CHECK_EQ(stats.frameCount, std::size_t{0});
    CHECK_NEAR(stats.jankPct, 0.0, 1e-9);
}

TEST_CASE("only SurfaceCanvas and SkippedFrame frames are left out") {
    // android::uirenderer::FrameInfoFlags: 0x1 WindowVisibilityChanged,
    // 0x2 RTAnimation, 0x4 SurfaceCanvas, 0x8 SkippedFrame.
    for (const auto& [flags, excluded] : {std::pair{0, false}, std::pair{1, false},
                                          std::pair{2, false}, std::pair{3, false},
                                          std::pair{4, true}, std::pair{8, true},
                                          std::pair{6, true}}) {
        const std::string dump =
            "---PROFILEDATA---\nFlags,IntendedVsync,FrameCompleted,\n" + std::to_string(flags) +
            ",1000000000,1010000000,\n---PROFILEDATA---\n";
        auto parsed = parseFrameStats(dump);
        CHECK(parsed.hasValue());
        if (parsed) CHECK(parsed.value().frames.at(0).excluded == excluded);
    }
}

// ---------------------------------------------------------------------------
// A real capture: Android 14 phone, an app with two activity windows.
// Every row ends with a trailing comma, DisplayPresentTime is -2, the header
// has FrameInterval/FrameStartTime swapped, and the frames are split across
// two per-window blocks. Each of those broke an earlier parser.
// ---------------------------------------------------------------------------

#include <fstream>
#include <map>
#include <sstream>

namespace {

std::string readRealCapture() {
    std::ifstream file{std::string{EMERON_TEST_DATA_DIR} + "/gfxinfo_android14_two_windows.txt"};
    std::string text;
    for (std::string line; std::getline(file, line);) {
        if (!line.starts_with('#')) text += line + '\n';
    }
    return text;
}

}  // namespace

TEST_CASE("real Android 14 capture: every row of every window is kept") {
    const std::string dump = readRealCapture();
    CHECK(!dump.empty());

    const auto blocks = extractProfileBlocks(dump);
    CHECK_EQ(blocks.size(), std::size_t{2});

    auto parsed = parseFrameStats(dump);
    CHECK(parsed.hasValue());
    if (!parsed) {
        ::test::reportFailure(__FILE__, __LINE__, parsed.error().message);
        return;
    }
    // 19 rows from the first window, 86 from the second -- of which 2 are
    // frames still in flight when dumpsys ran (SwapBuffers set, FrameCompleted
    // not yet). 19 + 84 = 103, and 84 is exactly the platform's "Total frames
    // rendered" for that window. Before the fix this was zero: "no usable
    // frames in PROFILEDATA".
    CHECK_EQ(parsed.value().frames.size(), std::size_t{103});
}

TEST_CASE("real Android 14 capture: refresh period comes from the frame interval") {
    auto parsed = parseFrameStats(readRealCapture());
    CHECK(parsed.hasValue());
    if (!parsed) return;
    // A 120 Hz panel. FrameDeadline - IntendedVsync here is 20 ms, which an
    // earlier version used as the budget.
    CHECK_NEAR(parsed.value().refreshPeriodMs, 8.33, 0.1);
}

TEST_CASE("real Android 14 capture: janky count matches dumpsys gfxinfo per window") {
    auto parsed = parseFrameStats(readRealCapture());
    CHECK(parsed.hasValue());
    if (!parsed) return;

    std::map<std::string, std::vector<FrameRecord>> byWindow;
    for (const auto& frame : parsed.value().frames) byWindow[frame.window].push_back(frame);
    CHECK_EQ(byWindow.size(), std::size_t{2});

    const auto period = parsed.value().refreshPeriodMs;
    const auto list = computeJankStats(byWindow["com.example.app/com.example.app.ListActivity"], period);
    const auto detail = computeJankStats(byWindow["com.example.app/com.example.app.DetailActivity"], period);

    // The platform's own numbers from the same dump: "Janky frames: 2" and
    // "Janky frames: 8". Judging by duration over one refresh gave 12 and far
    // more; excluding every flagged frame gave 5 for the second window.
    CHECK_EQ(list.jankCount, std::size_t{2});
    CHECK_EQ(detail.jankCount, std::size_t{8});

    // Frame totals match the platform's "Total frames rendered" per window too.
    CHECK_EQ(list.frameCount, std::size_t{19});
    CHECK_EQ(detail.frameCount, std::size_t{84});

    // Every frame here carries a FrameDeadline, so all were judged by it.
    CHECK_EQ(list.deadlineFrames + detail.deadlineFrames, std::size_t{103});

    // "On time" is exactly "not janky".
    CHECK_EQ(detail.within1, detail.frameCount - detail.jankCount);
}

TEST_CASE("real Android 14 capture: the summary is the process-wide one") {
    const GfxSummary summary = parseGfxSummary(readRealCapture());
    // Not the last window's 84 / 8, and not "Janky frames (legacy): 32".
    CHECK_EQ(summary.totalFrames, 130L);
    CHECK_EQ(summary.jankyFrames, 13L);
    CHECK_NEAR(summary.jankyPct, 10.0, 0.01);
    CHECK_EQ(summary.deadlineMissed, 13L);
    CHECK_EQ(summary.packageName, std::string{"com.example.app"});
}

TEST_CASE("an app that has drawn nothing since a reset is waiting, not an error") {
    // What a real Android 14 phone returns for an app on a static screen right
    // after `dumpsys gfxinfo <pkg> reset`: the section and header, no rows.
    // This used to surface as a red "no usable frames" error with hints about
    // hardware acceleration.
    const std::string dump =
        "** Graphics info for pid 4321 [com.example.app] **\n"
        "Total frames rendered: 0\n"
        "Janky frames: 0 (0.00%)\n"
        "Window: com.example.app/com.example.app.ListActivity\n"
        "---PROFILEDATA---\n"
        "Flags,FrameTimelineVsyncId,IntendedVsync,Vsync,FrameCompleted,FrameDeadline,\n"
        "---PROFILEDATA---\n";

    auto parsed = parseFrameStats(dump);
    CHECK(parsed.hasValue());
    if (parsed) CHECK(parsed.value().frames.empty());

    const GfxSummary summary = parseGfxSummary(dump);
    CHECK(summary.valid());
    CHECK_EQ(summary.totalFrames, 0L);
}

TEST_CASE("rows that are all unusable are still an error") {
    // Rows exist but none can be measured: that is a real problem to report.
    auto parsed = parseFrameStats(
        "---PROFILEDATA---\nFlags,IntendedVsync,FrameCompleted,\n0,0,0,\n---PROFILEDATA---\n");
    CHECK(!parsed.hasValue());
}
