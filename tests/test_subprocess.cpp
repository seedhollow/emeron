#include "TestHarness.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "core/Result.h"
#include "core/Subprocess.h"
#include "core/TaskQueue.h"

using namespace em;
using namespace std::chrono_literals;

TEST_CASE("Result carries a value or an error") {
    Result<int> ok{42};
    CHECK(ok.hasValue());
    CHECK(static_cast<bool>(ok));
    CHECK_EQ(ok.value(), 42);
    CHECK_EQ(*ok, 42);
    CHECK_EQ(ok.valueOr(7), 42);

    Result<int> bad{makeError(ErrorKind::Timeout, "too slow")};
    CHECK(!bad.hasValue());
    CHECK(bad.error().kind == ErrorKind::Timeout);
    CHECK_EQ(bad.error().message, std::string{"too slow"});
    CHECK_EQ(bad.valueOr(7), 7);
    CHECK_EQ(bad.describe(), std::string{"timeout: too slow"});
}

TEST_CASE("Status is a Result<void>") {
    Status ok;
    CHECK(ok.hasValue());
    CHECK_EQ(ok.describe(), std::string{"ok"});

    Status bad{makeError(ErrorKind::NotFound, "gone")};
    CHECK(!bad.hasValue());
    CHECK_EQ(std::string{bad.error().kindName()}, std::string{"not-found"});
}

TEST_CASE("Result moves a non-copyable payload") {
    Result<std::unique_ptr<int>> owned{std::make_unique<int>(5)};
    CHECK(owned.hasValue());
    auto pointer = std::move(owned).value();
    CHECK(pointer != nullptr);
    if (pointer) CHECK_EQ(*pointer, 5);
}

#if !defined(_WIN32)

TEST_CASE("runCommand captures stdout and the exit code") {
    auto result = runCommand({"/bin/sh", "-c", "printf 'hello\\n'"}, 5s);
    CHECK(result.hasValue());
    if (!result) return;

    CHECK_EQ(result->exitCode, 0);
    CHECK(result->ok());
    CHECK_EQ(result->out, std::string{"hello\n"});
    CHECK(result->err.empty());
    CHECK(!result->timedOut);
}

TEST_CASE("runCommand separates stderr and reports a non-zero exit") {
    auto result = runCommand({"/bin/sh", "-c", "printf 'oops\\n' >&2; exit 3"}, 5s);
    CHECK(result.hasValue());
    if (!result) return;

    CHECK_EQ(result->exitCode, 3);
    CHECK(!result->ok());
    CHECK_EQ(result->err, std::string{"oops\n"});
    CHECK_EQ(std::string{result->firstErrLine()}, std::string{"oops"});
}

TEST_CASE("runCommand handles output larger than one pipe buffer") {
    // 400 KiB is well past the 64 KiB pipe capacity, so this fails if the
    // reader does not drain while the child is still writing.
    auto result = runCommand(
        {"/bin/sh", "-c", "i=0; while [ $i -lt 4000 ]; do "
                          "printf '0123456789012345678901234567890123456789"
                          "012345678901234567890123456789012345678901234567890123456789"
                          "0123456789012345678901234567890123456789\\n'; "
                          "i=$((i+1)); done"},
        20s);
    CHECK(result.hasValue());
    if (!result) return;
    CHECK_EQ(result->exitCode, 0);
    CHECK(result->out.size() > 400'000);
}

TEST_CASE("runCommand writes stdin and closes it") {
    auto result = runCommand({"/bin/sh", "-c", "cat"}, 5s, "piped input\n");
    CHECK(result.hasValue());
    if (!result) return;
    CHECK_EQ(result->out, std::string{"piped input\n"});
}

TEST_CASE("runCommand times out and kills the child") {
    const auto started = std::chrono::steady_clock::now();
    auto result = runCommand({"/bin/sh", "-c", "sleep 30"}, 400ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(result.hasValue());
    if (!result) return;
    CHECK(result->timedOut);
    CHECK(!result->ok());
    // It must actually return promptly rather than wait out the sleep.
    CHECK(elapsed < 5s);
}

TEST_CASE("runCommand reports a missing executable") {
    auto result = runCommand({"/nonexistent/emeron-does-not-exist"}, 5s);
    // fork+execvp surfaces the failure as the child's 127 exit status.
    CHECK(result.hasValue());
    if (result) CHECK_EQ(result->exitCode, 127);
}

TEST_CASE("Subprocess streams incrementally and reports exit") {
    Subprocess::Options options;
    options.argv = {"/bin/sh", "-c", "printf 'a\\n'; printf 'b\\n'"};

    auto spawned = Subprocess::spawn(std::move(options));
    CHECK(spawned.hasValue());
    if (!spawned) return;

    Subprocess process = std::move(spawned).value();
    CHECK(process.valid());

    std::string collected;
    std::array<char, 64> buffer{};
    for (int i = 0; i < 100; ++i) {
        bool timedOut = false;
        auto count = process.readStdout(buffer, 100, &timedOut);
        CHECK(count.hasValue());
        if (!count) break;
        if (*count == 0 && !timedOut) break;  // EOF
        collected.append(buffer.data(), *count);
    }

    CHECK_EQ(collected, std::string{"a\nb\n"});
    auto code = process.wait(2s);
    CHECK(code.hasValue());
    if (code) CHECK_EQ(*code, 0);
}

TEST_CASE("Subprocess move leaves the source empty") {
    Subprocess::Options options;
    options.argv = {"/bin/sh", "-c", "sleep 5"};

    auto spawned = Subprocess::spawn(std::move(options));
    CHECK(spawned.hasValue());
    if (!spawned) return;

    Subprocess first = std::move(spawned).value();
    CHECK(first.valid());

    Subprocess second = std::move(first);
    CHECK(second.valid());
    CHECK(!first.valid());

    second.terminate();
}

TEST_CASE("Subprocess terminate reaps the child") {
    Subprocess::Options options;
    options.argv = {"/bin/sh", "-c", "sleep 30"};

    auto spawned = Subprocess::spawn(std::move(options));
    CHECK(spawned.hasValue());
    if (!spawned) return;

    Subprocess process = std::move(spawned).value();
    CHECK(!process.tryWait().has_value());

    process.terminate();
    auto code = process.wait(3s);
    CHECK(code.hasValue());
}

TEST_CASE("findOnPath locates a standard binary") {
    const auto shell = findOnPath("sh");
    CHECK(shell.has_value());
    CHECK(!findOnPath("emeron-definitely-not-a-real-binary").has_value());
}

#endif  // !_WIN32

TEST_CASE("ThreadPool runs every submitted task") {
    ThreadPool pool{3};
    std::atomic<int> counter{0};

    constexpr int kTasks = 200;
    for (int i = 0; i < kTasks; ++i) {
        pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }

    // Drain by waiting on the counter rather than sleeping a fixed time.
    for (int i = 0; i < 500 && counter.load(std::memory_order_relaxed) < kTasks; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK_EQ(counter.load(std::memory_order_relaxed), kTasks);
    CHECK(pool.workerCount() == std::size_t{3});
}

TEST_CASE("Dispatcher defers tasks posted during a drain") {
    Dispatcher dispatcher;
    int order = 0;
    int firstRanAt = -1;
    int nestedRanAt = -1;

    dispatcher.post([&] {
        firstRanAt = order++;
        dispatcher.post([&] { nestedRanAt = order++; });
    });

    CHECK_EQ(dispatcher.drain(), std::size_t{1});
    CHECK_EQ(firstRanAt, 0);
    CHECK_EQ(nestedRanAt, -1);  // not this drain

    CHECK_EQ(dispatcher.drain(), std::size_t{1});
    CHECK_EQ(nestedRanAt, 1);
}

TEST_CASE("Shared serialises access and tracks a revision") {
    Shared<std::vector<int>> shared;
    CHECK_EQ(shared.revision(), std::uint64_t{0});

    shared.set({1, 2, 3});
    CHECK_EQ(shared.revision(), std::uint64_t{1});
    CHECK_EQ(shared.get().size(), std::size_t{3});

    shared.mutate([](std::vector<int>& value) { value.push_back(4); });
    CHECK_EQ(shared.revision(), std::uint64_t{2});

    std::size_t observed = 0;
    shared.read([&observed](const std::vector<int>& value) { observed = value.size(); });
    CHECK_EQ(observed, std::size_t{4});
}

#if !defined(_WIN32)

#include <cstdlib>
#include <fstream>

#include "adb/AdbCliTransport.h"
#include "collect/TraceProcessor.h"

namespace {

// Restores an environment variable when the test ends.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name)) previous_ = old;
        ::setenv(name, value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (previous_) {
            ::setenv(name_, previous_->c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    std::optional<std::string> previous_;
};

std::filesystem::path makeFakeExecutable(const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream{path} << "#!/bin/sh\nexit 0\n";
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    return path;
}

}  // namespace

TEST_CASE("EMERON_ADB outranks an adb already on PATH") {
    // The bug: PATH was consulted first, so on any machine with adb installed
    // the variable was silently ignored despite being documented as --adb.
    const auto fake = makeFakeExecutable("emeron-test-fake-adb");
    const ScopedEnv env{"EMERON_ADB", fake.string()};

    const AdbCliTransport transport;
    CHECK(transport.binaryPath().has_value());
    if (transport.binaryPath()) CHECK(*transport.binaryPath() == fake);
    CHECK(transport.describe().find("EMERON_ADB") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(fake, ec);
}

TEST_CASE("an explicit adb path still outranks EMERON_ADB") {
    const auto fromEnv = makeFakeExecutable("emeron-test-env-adb");
    const auto explicitPath = makeFakeExecutable("emeron-test-explicit-adb");
    const ScopedEnv env{"EMERON_ADB", fromEnv.string()};

    const AdbCliTransport transport{explicitPath};
    if (transport.binaryPath()) CHECK(*transport.binaryPath() == explicitPath);

    std::error_code ec;
    std::filesystem::remove(fromEnv, ec);
    std::filesystem::remove(explicitPath, ec);
}

TEST_CASE("EMERON_TRACE_PROCESSOR outranks PATH") {
    const auto fake = makeFakeExecutable("emeron-test-fake-tp");
    const ScopedEnv env{"EMERON_TRACE_PROCESSOR", fake.string()};

    const TraceProcessor processor;
    CHECK(processor.state().binaryPath == fake.string());

    std::error_code ec;
    std::filesystem::remove(fake, ec);
}

#endif  // !_WIN32
