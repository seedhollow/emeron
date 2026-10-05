#include "TestHarness.h"

#include <string>
#include <vector>

#include "collect/LogcatReader.h"

using namespace em;

// ---------------------------------------------------------------------------
// Line parsing
// ---------------------------------------------------------------------------

TEST_CASE("parseLogcatLine reads the threadtime format") {
    const LogcatEntry entry =
        parseLogcatLine("10-04 17:45:01.123  1234  1256 I MyTag: hello world", 1.5);

    CHECK(!entry.raw);
    CHECK_EQ(entry.deviceTime, std::string{"10-04 17:45:01.123"});
    CHECK_EQ(entry.pid, 1234);
    CHECK_EQ(entry.tid, 1256);
    CHECK(entry.level == LogcatLevel::Info);
    CHECK_EQ(entry.tag, std::string{"MyTag"});
    CHECK_EQ(entry.message, std::string{"hello world"});
    CHECK_NEAR(entry.hostTime, 1.5, 1e-9);
}

TEST_CASE("parseLogcatLine keeps colons inside the message") {
    const LogcatEntry entry = parseLogcatLine(
        "10-04 17:45:01.123  1234  1256 E AndroidRuntime: java.lang.IllegalStateException: "
        "nope: really",
        0.0);

    CHECK(!entry.raw);
    CHECK_EQ(entry.tag, std::string{"AndroidRuntime"});
    CHECK(entry.level == LogcatLevel::Error);
    // Only the first ": " after the level separates tag from message.
    CHECK_EQ(entry.message,
             std::string{"java.lang.IllegalStateException: nope: really"});
}

TEST_CASE("parseLogcatLine handles every priority") {
    const struct {
        char code;
        LogcatLevel expected;
    } cases[] = {
        {'V', LogcatLevel::Verbose}, {'D', LogcatLevel::Debug},
        {'I', LogcatLevel::Info},    {'W', LogcatLevel::Warn},
        {'E', LogcatLevel::Error},   {'F', LogcatLevel::Fatal},
    };

    for (const auto& testCase : cases) {
        const std::string line =
            std::string{"10-04 17:45:01.123  1 2 "} + testCase.code + " T: m";
        const LogcatEntry entry = parseLogcatLine(line, 0.0);
        CHECK(entry.level == testCase.expected);
        CHECK(!entry.raw);
    }
}

TEST_CASE("parseLogcatLine preserves separator lines verbatim") {
    const LogcatEntry entry = parseLogcatLine("--------- beginning of main", 0.0);
    CHECK(entry.raw);
    CHECK_EQ(entry.message, std::string{"--------- beginning of main"});
    CHECK_EQ(entry.pid, -1);
}

TEST_CASE("parseLogcatLine keeps unrecognised lines rather than dropping them") {
    // A native crash dump line, which does not follow threadtime at all.
    const LogcatEntry entry = parseLogcatLine("backtrace:", 0.0);
    CHECK(entry.raw);
    CHECK_EQ(entry.message, std::string{"backtrace:"});

    const LogcatEntry empty = parseLogcatLine("", 0.0);
    CHECK(empty.raw);
    CHECK(empty.message.empty());
}

TEST_CASE("parseLogcatLine tolerates a tag containing spaces") {
    const LogcatEntry entry =
        parseLogcatLine("10-04 17:45:01.123  1234  1256 W My Spaced Tag: careful", 0.0);
    CHECK(!entry.raw);
    CHECK_EQ(entry.tag, std::string{"My Spaced Tag"});
    CHECK_EQ(entry.message, std::string{"careful"});
}

TEST_CASE("parseLogcatLine handles a tag with no message") {
    const LogcatEntry entry =
        parseLogcatLine("10-04 17:45:01.123  1234  1256 I Lonely:", 0.0);
    CHECK(!entry.raw);
    CHECK_EQ(entry.tag, std::string{"Lonely"});
    CHECK(entry.message.empty());
}

TEST_CASE("parseLogcatLine rejects a non-numeric pid") {
    const LogcatEntry entry =
        parseLogcatLine("10-04 17:45:01.123  abcd  1256 I Tag: msg", 0.0);
    CHECK(entry.raw);
}

TEST_CASE("logcat level round-trips through char") {
    for (const char code : {'V', 'D', 'I', 'W', 'E', 'F', 'S'}) {
        CHECK_EQ(toChar(parseLogcatLevel(code)), code);
    }
    CHECK(parseLogcatLevel('?') == LogcatLevel::Unknown);
    CHECK(parseLogcatLevel('i') == LogcatLevel::Info);  // lowercase accepted
}

// ---------------------------------------------------------------------------
// Stream chunking
// ---------------------------------------------------------------------------

TEST_CASE("drainCompleteLines leaves a partial line in the buffer") {
    std::string buffer = "line one\nline two\npartial";
    std::vector<std::string> lines;
    drainCompleteLines(buffer, lines);

    CHECK_EQ(lines.size(), std::size_t{2});
    CHECK_EQ(lines[0], std::string{"line one"});
    CHECK_EQ(lines[1], std::string{"line two"});
    CHECK_EQ(buffer, std::string{"partial"});

    // The next read completes it.
    buffer += " now complete\n";
    lines.clear();
    drainCompleteLines(buffer, lines);
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK_EQ(lines[0], std::string{"partial now complete"});
    CHECK(buffer.empty());
}

TEST_CASE("drainCompleteLines strips CRLF from the Windows adb build") {
    std::string buffer = "a\r\nb\r\n";
    std::vector<std::string> lines;
    drainCompleteLines(buffer, lines);

    CHECK_EQ(lines.size(), std::size_t{2});
    CHECK_EQ(lines[0], std::string{"a"});
    CHECK_EQ(lines[1], std::string{"b"});
    CHECK(buffer.empty());
}

TEST_CASE("drainCompleteLines on a chunk with no newline keeps everything") {
    std::string buffer = "no newline yet";
    std::vector<std::string> lines;
    drainCompleteLines(buffer, lines);

    CHECK(lines.empty());
    CHECK_EQ(buffer, std::string{"no newline yet"});
}

TEST_CASE("drainCompleteLines preserves empty lines") {
    std::string buffer = "a\n\nb\n";
    std::vector<std::string> lines;
    drainCompleteLines(buffer, lines);

    CHECK_EQ(lines.size(), std::size_t{3});
    CHECK(lines[1].empty());
}

// ---------------------------------------------------------------------------
// Command construction
// ---------------------------------------------------------------------------

namespace {

bool hasArg(const std::vector<std::string>& args, std::string_view value) {
    for (const auto& arg : args) {
        if (arg == value) return true;
    }
    return false;
}

std::string joined(const std::vector<std::string>& args) {
    std::string out;
    for (const auto& arg : args) {
        if (!out.empty()) out += ' ';
        out += arg;
    }
    return out;
}

}  // namespace

TEST_CASE("buildLogcatArgs always requests threadtime") {
    LogcatOptions options;
    const auto args = buildLogcatArgs(options, {});
    CHECK_EQ(args.front(), std::string{"logcat"});
    CHECK(hasArg(args, "-v"));
    CHECK(hasArg(args, "threadtime"));
}

TEST_CASE("buildLogcatArgs uses --pid for a single-process package") {
    LogcatOptions options;
    options.scope = LogcatScope::SelectedProcess;
    options.deviceSideFilter = true;

    const auto args = buildLogcatArgs(options, {4321});
    CHECK(hasArg(args, "--pid=4321"));
}

TEST_CASE("buildLogcatArgs omits --pid when the package has several processes") {
    // logcat takes only one --pid, so a multi-process package must be
    // filtered host side instead.
    LogcatOptions options;
    options.scope = LogcatScope::SelectedProcess;
    options.deviceSideFilter = true;

    const auto args = buildLogcatArgs(options, {4321, 4322});
    CHECK(joined(args).find("--pid") == std::string::npos);
}

TEST_CASE("buildLogcatArgs omits --pid when the process is not running") {
    LogcatOptions options;
    options.scope = LogcatScope::SelectedProcess;
    const auto args = buildLogcatArgs(options, {});
    CHECK(joined(args).find("--pid") == std::string::npos);
}

TEST_CASE("buildLogcatArgs omits --pid for all-process scope") {
    LogcatOptions options;
    options.scope = LogcatScope::AllProcesses;
    options.deviceSideFilter = true;

    const auto args = buildLogcatArgs(options, {4321});
    CHECK(joined(args).find("--pid") == std::string::npos);
}

TEST_CASE("buildLogcatArgs honours the device-side filter toggle") {
    LogcatOptions options;
    options.scope = LogcatScope::SelectedProcess;
    options.deviceSideFilter = false;

    const auto args = buildLogcatArgs(options, {4321});
    CHECK(joined(args).find("--pid") == std::string::npos);
}

TEST_CASE("buildLogcatArgs passes backlog and buffers") {
    LogcatOptions options;
    options.backlogLines = 250;
    options.buffers = {"main", "crash"};

    const auto args = buildLogcatArgs(options, {});
    CHECK(hasArg(args, "-T"));
    CHECK(hasArg(args, "250"));
    CHECK(hasArg(args, "-b"));
    CHECK(hasArg(args, "main"));
    CHECK(hasArg(args, "crash"));
}

TEST_CASE("buildLogcatArgs omits -T when no backlog is wanted") {
    LogcatOptions options;
    options.backlogLines = 0;
    const auto args = buildLogcatArgs(options, {});
    CHECK(!hasArg(args, "-T"));
}

TEST_CASE("buildLogcatArgs emits a level spec only above verbose") {
    LogcatOptions verbose;
    verbose.deviceMinLevel = LogcatLevel::Verbose;
    CHECK(joined(buildLogcatArgs(verbose, {})).find("*:") == std::string::npos);

    LogcatOptions warn;
    warn.deviceMinLevel = LogcatLevel::Warn;
    CHECK(hasArg(buildLogcatArgs(warn, {}), "*:W"));
}

TEST_CASE("LogcatOptions compares by value so a no-op change cannot restart the stream") {
    LogcatOptions a;
    LogcatOptions b;
    CHECK(a == b);

    b.backlogLines = 1;
    CHECK(!(a == b));
}

TEST_CASE("LogcatSnapshot::tracksPid matches the resolved set") {
    LogcatSnapshot snapshot;
    snapshot.trackedPids = {100, 200};
    CHECK(snapshot.tracksPid(100));
    CHECK(snapshot.tracksPid(200));
    CHECK(!snapshot.tracksPid(300));

    snapshot.trackedPids.clear();
    CHECK(!snapshot.tracksPid(100));
}

// ---------------------------------------------------------------------------
// Host-side filter: the rule that defines "logcat from process"
// ---------------------------------------------------------------------------

namespace {

LogcatEntry makeEntry(int pid, LogcatLevel level, std::string tag, std::string message) {
    LogcatEntry entry;
    entry.pid = pid;
    entry.tid = pid;
    entry.level = level;
    entry.tag = std::move(tag);
    entry.message = std::move(message);
    entry.deviceTime = "10-04 17:45:01.123";
    return entry;
}

LogcatSnapshot snapshotWithPids(std::vector<int> pids) {
    LogcatSnapshot snapshot;
    snapshot.trackedPids = std::move(pids);
    return snapshot;
}

}  // namespace

TEST_CASE("filter keeps only the tracked process") {
    const auto snapshot = snapshotWithPids({4321});
    LogcatFilter filter;
    filter.scope = LogcatScope::SelectedProcess;
    filter.onlyTrackedPids = true;

    CHECK(filter.accepts(makeEntry(4321, LogcatLevel::Info, "T", "mine"), snapshot));
    CHECK(!filter.accepts(makeEntry(9999, LogcatLevel::Info, "T", "theirs"), snapshot));
}

TEST_CASE("filter accepts every pid of a multi-process package") {
    const auto snapshot = snapshotWithPids({100, 200});
    LogcatFilter filter;

    CHECK(filter.accepts(makeEntry(100, LogcatLevel::Info, "T", "a"), snapshot));
    CHECK(filter.accepts(makeEntry(200, LogcatLevel::Info, "T", "b"), snapshot));
    CHECK(!filter.accepts(makeEntry(300, LogcatLevel::Info, "T", "c"), snapshot));
}

TEST_CASE("filter shows nothing when the package is not running") {
    // No resolved pid means nothing can be attributed to the package. Showing
    // the whole device log here would silently answer a different question.
    const auto snapshot = snapshotWithPids({});
    LogcatFilter filter;
    filter.scope = LogcatScope::SelectedProcess;
    filter.onlyTrackedPids = true;

    CHECK(!filter.accepts(makeEntry(4321, LogcatLevel::Info, "T", "x"), snapshot));
}

TEST_CASE("filter ignores pids in all-process scope") {
    const auto snapshot = snapshotWithPids({4321});
    LogcatFilter filter;
    filter.scope = LogcatScope::AllProcesses;

    CHECK(filter.accepts(makeEntry(9999, LogcatLevel::Info, "T", "x"), snapshot));
}

TEST_CASE("filter always lets raw and separator lines through") {
    // A native crash tombstone does not follow threadtime, so suppressing
    // unparsed lines would hide exactly the thing you are looking for.
    const auto snapshot = snapshotWithPids({4321});
    LogcatFilter filter;
    filter.minLevel = LogcatLevel::Error;
    filter.tag = "nomatch";
    filter.text = "nomatch";

    LogcatEntry raw;
    raw.raw = true;
    raw.message = "--------- beginning of crash";
    CHECK(filter.accepts(raw, snapshot));
}

TEST_CASE("filter applies the minimum level") {
    const auto snapshot = snapshotWithPids({1});
    LogcatFilter filter;
    filter.minLevel = LogcatLevel::Warn;

    CHECK(!filter.accepts(makeEntry(1, LogcatLevel::Debug, "T", "x"), snapshot));
    CHECK(!filter.accepts(makeEntry(1, LogcatLevel::Info, "T", "x"), snapshot));
    CHECK(filter.accepts(makeEntry(1, LogcatLevel::Warn, "T", "x"), snapshot));
    CHECK(filter.accepts(makeEntry(1, LogcatLevel::Error, "T", "x"), snapshot));
    CHECK(filter.accepts(makeEntry(1, LogcatLevel::Fatal, "T", "x"), snapshot));
}

TEST_CASE("filter matches tags case-insensitively and partially") {
    const auto snapshot = snapshotWithPids({1});
    LogcatFilter filter;
    filter.tag = "activity";

    CHECK(filter.accepts(makeEntry(1, LogcatLevel::Info, "ActivityManager", "x"), snapshot));
    CHECK(!filter.accepts(makeEntry(1, LogcatLevel::Info, "WindowManager", "x"), snapshot));
}

TEST_CASE("text filter searches the message and the tag") {
    const auto snapshot = snapshotWithPids({1});
    LogcatFilter filter;
    filter.text = "boom";

    CHECK(filter.accepts(makeEntry(1, LogcatLevel::Info, "T", "it went BOOM"), snapshot));
    CHECK(filter.accepts(makeEntry(1, LogcatLevel::Info, "BoomTag", "quiet"), snapshot));
    CHECK(!filter.accepts(makeEntry(1, LogcatLevel::Info, "T", "all fine"), snapshot));
}

TEST_CASE("exclude mode inverts the text filter") {
    const auto snapshot = snapshotWithPids({1});
    LogcatFilter filter;
    filter.text = "chatty";
    filter.excludeText = true;

    CHECK(!filter.accepts(makeEntry(1, LogcatLevel::Info, "T", "chatty noise"), snapshot));
    CHECK(filter.accepts(makeEntry(1, LogcatLevel::Info, "T", "signal"), snapshot));
}

TEST_CASE("filter signature changes when the pid set changes") {
    LogcatFilter filter;
    const auto before = filter.signature(snapshotWithPids({100}));
    const auto after = filter.signature(snapshotWithPids({200}));
    CHECK(before != after);

    // And when the user edits a filter.
    filter.text = "x";
    CHECK(filter.signature(snapshotWithPids({200})) != after);

    // But not for an unrelated snapshot field.
    LogcatFilter stable;
    auto a = snapshotWithPids({1});
    auto b = snapshotWithPids({1});
    b.totalReceived = 999;
    CHECK_EQ(stable.signature(a), stable.signature(b));
}

TEST_CASE("logcatRetryDelay backs off and caps") {
    using std::chrono::milliseconds;
    CHECK(logcatRetryDelay(0) == milliseconds{0});
    CHECK(logcatRetryDelay(-3) == milliseconds{0});
    CHECK(logcatRetryDelay(1) == milliseconds{500});
    CHECK(logcatRetryDelay(2) == milliseconds{1000});
    CHECK(logcatRetryDelay(3) == milliseconds{2000});
    CHECK(logcatRetryDelay(5) == milliseconds{8000});
    // Capped, and no overflow however long the streak gets.
    CHECK(logcatRetryDelay(6) == milliseconds{10'000});
    CHECK(logcatRetryDelay(1000) == milliseconds{10'000});
}
