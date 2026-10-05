#include "core/Subprocess.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "core/Log.h"
#include "core/StringUtil.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <csignal>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace em {
namespace {

constexpr std::size_t kChunk = 64 * 1024;

std::chrono::steady_clock::time_point deadlineFrom(std::chrono::milliseconds timeout) {
    return std::chrono::steady_clock::now() + timeout;
}

int millisUntil(std::chrono::steady_clock::time_point deadline) {
    using namespace std::chrono;
    const auto left = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
    if (left <= 0) return 0;
    return static_cast<int>(std::min<long long>(left, 1000));
}

}  // namespace

std::string_view CommandOutput::firstErrLine() const noexcept {
    const auto pos = err.find('\n');
    return trim(pos == std::string::npos ? std::string_view{err}
                                         : std::string_view{err}.substr(0, pos));
}

// ===========================================================================
#if !defined(_WIN32)
// ============================= POSIX =======================================

namespace {

Status setCloseOnExec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return makeError(ErrorKind::Io, std::string{"fcntl(FD_CLOEXEC): "} + std::strerror(errno));
    }
    return {};
}

Result<std::size_t> readFdWithTimeout(int fd, std::span<char> dst, int waitMs, bool* timedOut) {
    if (timedOut) *timedOut = false;
    if (fd < 0) return std::size_t{0};

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    for (;;) {
        const int rc = ::poll(&pfd, 1, waitMs);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return makeError(ErrorKind::Io, std::string{"poll: "} + std::strerror(errno));
        }
        if (rc == 0) {
            if (timedOut) *timedOut = true;
            return std::size_t{0};
        }
        break;
    }

    for (;;) {
        const ssize_t n = ::read(fd, dst.data(), dst.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return std::size_t{0};
            return makeError(ErrorKind::Io, std::string{"read: "} + std::strerror(errno));
        }
        return static_cast<std::size_t>(n);
    }
}

}  // namespace

void initProcessSubsystem() {
    // Writing to a pipe whose reader died must give EPIPE, not kill the app.
    std::signal(SIGPIPE, SIG_IGN);
}

Result<Subprocess> Subprocess::spawn(Options options) {
    if (options.argv.empty()) {
        return makeError(ErrorKind::Unsupported, "spawn: empty argv");
    }

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    int inPipe[2] = {-1, -1};

    const auto cleanup = [&]() noexcept {
        for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1], inPipe[0], inPipe[1]}) {
            if (fd >= 0) ::close(fd);
        }
    };

    if (::pipe(outPipe) != 0) {
        return makeError(ErrorKind::Io, std::string{"pipe: "} + std::strerror(errno));
    }
    if (!options.mergeStderrIntoStdout && ::pipe(errPipe) != 0) {
        cleanup();
        return makeError(ErrorKind::Io, std::string{"pipe: "} + std::strerror(errno));
    }
    if (options.pipeStdin && ::pipe(inPipe) != 0) {
        cleanup();
        return makeError(ErrorKind::Io, std::string{"pipe: "} + std::strerror(errno));
    }

    // argv for execvp; built before fork so the child does no allocation.
    std::vector<char*> cargv;
    cargv.reserve(options.argv.size() + 1);
    for (auto& s : options.argv) cargv.push_back(s.data());
    cargv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        cleanup();
        return makeError(ErrorKind::Io, std::string{"fork: "} + std::strerror(errno));
    }

    if (pid == 0) {
        // -------- child --------
        // Own process group, so the parent can signal the whole subtree.
        // The parent races us to the same call; both are harmless and one of
        // them is guaranteed to have run before any kill(-pid, ...).
        ::setpgid(0, 0);

        ::dup2(outPipe[1], STDOUT_FILENO);
        ::dup2(options.mergeStderrIntoStdout ? outPipe[1] : errPipe[1], STDERR_FILENO);
        if (options.pipeStdin) {
            ::dup2(inPipe[0], STDIN_FILENO);
        } else {
            const int devnull = ::open("/dev/null", O_RDONLY);
            if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
        }

        for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1], inPipe[0], inPipe[1]}) {
            if (fd >= 0 && fd > STDERR_FILENO) ::close(fd);
        }

        ::execvp(cargv[0], cargv.data());
        // execvp only returns on failure. 127 matches the shell convention.
        ::_exit(127);
    }

    // -------- parent --------
    // Set the child's process group from here too. Without this, a terminate()
    // issued immediately after spawn can call kill(-pid, ...) before the child
    // has run setpgid, and the signal goes nowhere (ESRCH) while the child
    // keeps running. EACCES here just means the child won the race.
    ::setpgid(pid, pid);

    ::close(outPipe[1]);
    outPipe[1] = -1;
    if (errPipe[1] >= 0) {
        ::close(errPipe[1]);
        errPipe[1] = -1;
    }
    if (inPipe[0] >= 0) {
        ::close(inPipe[0]);
        inPipe[0] = -1;
    }

    Subprocess proc;
    proc.argv_ = std::move(options.argv);
    proc.pid_ = pid;
    proc.stdoutRead_ = outPipe[0];
    proc.stderrRead_ = errPipe[0];
    proc.stdinWrite_ = inPipe[1];

    (void)setCloseOnExec(proc.stdoutRead_);
    if (proc.stderrRead_ >= 0) (void)setCloseOnExec(proc.stderrRead_);
    if (proc.stdinWrite_ >= 0) (void)setCloseOnExec(proc.stdinWrite_);

    return proc;
}

bool Subprocess::valid() const noexcept { return pid_ > 0; }

Result<std::size_t> Subprocess::readStdout(std::span<char> dst, int waitMs, bool* timedOut) {
    return readFdWithTimeout(stdoutRead_, dst, waitMs, timedOut);
}

Result<std::size_t> Subprocess::readStderr(std::span<char> dst, int waitMs, bool* timedOut) {
    return readFdWithTimeout(stderrRead_, dst, waitMs, timedOut);
}

Status Subprocess::writeStdin(std::string_view data) {
    if (stdinWrite_ < 0) return makeError(ErrorKind::Io, "stdin is not piped");
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(stdinWrite_, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return makeError(ErrorKind::Io, std::string{"write(stdin): "} + std::strerror(errno));
        }
        written += static_cast<std::size_t>(n);
    }
    return {};
}

void Subprocess::closeStdin() {
    if (stdinWrite_ >= 0) {
        ::close(stdinWrite_);
        stdinWrite_ = -1;
    }
}

std::optional<int> Subprocess::tryWait() {
    if (exitCode_) return exitCode_;
    if (pid_ <= 0) return std::nullopt;

    int status = 0;
    const pid_t rc = ::waitpid(pid_, &status, WNOHANG);
    if (rc == 0) return std::nullopt;
    if (rc < 0) {
        if (errno == EINTR) return std::nullopt;
        exitCode_ = -1;  // already reaped or gone
        return exitCode_;
    }
    exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status)
                                  : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    return exitCode_;
}

Result<int> Subprocess::wait(std::chrono::milliseconds timeout) {
    const auto deadline = deadlineFrom(timeout);
    for (;;) {
        if (const auto code = tryWait()) return *code;
        if (std::chrono::steady_clock::now() >= deadline) {
            return makeError(ErrorKind::Timeout, "process did not exit within timeout");
        }
        // Children are short-lived; a 2 ms spin keeps latency low without burn.
        ::usleep(2000);
    }
}

namespace {

// Signals the whole group, falling back to the process itself if the group no
// longer exists (the child exec'd something that changed its pgid).
void signalGroupOrProcess(pid_t pid, int signal) {
    if (::kill(-pid, signal) != 0 && errno == ESRCH) ::kill(pid, signal);
}

}  // namespace

void Subprocess::terminate() {
    if (pid_ > 0 && !exitCode_) signalGroupOrProcess(pid_, SIGTERM);
}

void Subprocess::killHard() {
    if (pid_ > 0 && !exitCode_) signalGroupOrProcess(pid_, SIGKILL);
}

void Subprocess::closeAll() noexcept {
    if (stdoutRead_ >= 0) ::close(stdoutRead_);
    if (stderrRead_ >= 0) ::close(stderrRead_);
    if (stdinWrite_ >= 0) ::close(stdinWrite_);
    stdoutRead_ = stderrRead_ = stdinWrite_ = -1;

    if (pid_ > 0 && !exitCode_) {
        signalGroupOrProcess(pid_, SIGTERM);
        // Give it a moment, then reap so we do not leak zombies.
        for (int i = 0; i < 50; ++i) {
            int status = 0;
            const pid_t rc = ::waitpid(pid_, &status, WNOHANG);
            if (rc != 0) break;
            ::usleep(2000);
        }
        signalGroupOrProcess(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, WNOHANG);
    }
    pid_ = -1;
}

Result<CommandOutput> runCommand(std::vector<std::string> argv,
                                 std::chrono::milliseconds timeout,
                                 std::string_view stdinData) {
    const std::string display = quoteArgv(argv);

    Subprocess::Options opts;
    opts.argv = std::move(argv);
    opts.pipeStdin = !stdinData.empty();

    auto spawned = Subprocess::spawn(std::move(opts));
    if (!spawned) return std::move(spawned).error();
    Subprocess proc = std::move(spawned).value();

    if (!stdinData.empty()) {
        if (auto st = proc.writeStdin(stdinData); !st) {
            EM_LOG_DEBUG("proc", "stdin write failed for " + display + ": " + st.describe());
        }
    }
    proc.closeStdin();

    CommandOutput result;
    const auto deadline = deadlineFrom(timeout);

    std::array<char, kChunk> buffer{};
    bool outOpen = true;
    bool errOpen = true;

    while (outOpen || errOpen) {
        const int slice = millisUntil(deadline);
        if (slice == 0 && std::chrono::steady_clock::now() >= deadline) break;

        bool progressed = false;

        if (outOpen) {
            bool to = false;
            auto n = proc.readStdout(buffer, outOpen && errOpen ? 5 : slice, &to);
            if (!n) return std::move(n).error();
            if (*n > 0) {
                result.out.append(buffer.data(), *n);
                progressed = true;
            } else if (!to) {
                outOpen = false;
            }
        }
        if (errOpen) {
            bool to = false;
            auto n = proc.readStderr(buffer, outOpen && errOpen ? 5 : slice, &to);
            if (!n) return std::move(n).error();
            if (*n > 0) {
                result.err.append(buffer.data(), *n);
                progressed = true;
            } else if (!to) {
                errOpen = false;
            }
        }

        if (!progressed && proc.tryWait() && !outOpen && !errOpen) break;
    }

    if (std::chrono::steady_clock::now() >= deadline && !proc.tryWait()) {
        proc.terminate();
        result.timedOut = true;
        result.exitCode = -1;
        EM_LOG_WARN("proc", "timeout after " + std::to_string(timeout.count()) + "ms: " + display);
        return result;
    }

    auto code = proc.wait(std::chrono::milliseconds{2000});
    result.exitCode = code ? *code : -1;
    return result;
}

std::optional<std::filesystem::path> findOnPath(std::string_view name) {
    namespace fs = std::filesystem;

    const fs::path candidate{name};
    if (candidate.is_absolute()) {
        std::error_code ec;
        if (fs::exists(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0) return candidate;
        return std::nullopt;
    }

    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr) return std::nullopt;

    for (const auto dir : split(std::string_view{pathEnv}, ':', false)) {
        if (dir.empty()) continue;
        const fs::path full = fs::path{std::string{dir}} / std::string{name};
        std::error_code ec;
        if (fs::exists(full, ec) && ::access(full.c_str(), X_OK) == 0) return full;
    }
    return std::nullopt;
}

// ===========================================================================
#else
// ============================= Windows =====================================

namespace {

Result<std::size_t> readHandleWithTimeout(HANDLE h, std::span<char> dst, int waitMs,
                                          bool* timedOut) {
    if (timedOut) *timedOut = false;
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return std::size_t{0};

    // Anonymous pipes do not support overlapped I/O, so poll with PeekNamedPipe.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{
        waitMs < 0 ? 0 : waitMs};
    for (;;) {
        DWORD available = 0;
        if (!::PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_BROKEN_PIPE) return std::size_t{0};  // EOF
            return makeError(ErrorKind::Io, "PeekNamedPipe failed: " + std::to_string(err));
        }
        if (available > 0) break;
        if (waitMs >= 0 && std::chrono::steady_clock::now() >= deadline) {
            if (timedOut) *timedOut = true;
            return std::size_t{0};
        }
        ::Sleep(2);
    }

    DWORD read = 0;
    if (!::ReadFile(h, dst.data(), static_cast<DWORD>(dst.size()), &read, nullptr)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_BROKEN_PIPE) return std::size_t{0};
        return makeError(ErrorKind::Io, "ReadFile failed: " + std::to_string(err));
    }
    return static_cast<std::size_t>(read);
}

// CreateProcess takes one command line, so argv must be re-quoted per the
// CommandLineToArgvW rules.
std::string buildCommandLine(const std::vector<std::string>& argv) {
    std::string out;
    for (const auto& arg : argv) {
        if (!out.empty()) out.push_back(' ');
        const bool needsQuotes =
            arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string::npos;
        if (!needsQuotes) {
            out += arg;
            continue;
        }
        out.push_back('"');
        for (std::size_t i = 0;; ++i) {
            std::size_t backslashes = 0;
            while (i < arg.size() && arg[i] == '\\') {
                ++i;
                ++backslashes;
            }
            if (i == arg.size()) {
                out.append(backslashes * 2, '\\');
                break;
            }
            if (arg[i] == '"') {
                out.append(backslashes * 2 + 1, '\\');
            } else {
                out.append(backslashes, '\\');
            }
            out.push_back(arg[i]);
        }
        out.push_back('"');
    }
    return out;
}

}  // namespace

void initProcessSubsystem() {}

Result<Subprocess> Subprocess::spawn(Options options) {
    if (options.argv.empty()) return makeError(ErrorKind::Unsupported, "spawn: empty argv");

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRd = nullptr, outWr = nullptr;
    HANDLE errRd = nullptr, errWr = nullptr;
    HANDLE inRd = nullptr, inWr = nullptr;

    const auto closeIf = [](HANDLE& h) noexcept {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        h = nullptr;
    };

    if (!::CreatePipe(&outRd, &outWr, &sa, 0)) {
        return makeError(ErrorKind::Io, "CreatePipe(stdout) failed");
    }
    ::SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);

    if (!options.mergeStderrIntoStdout) {
        if (!::CreatePipe(&errRd, &errWr, &sa, 0)) {
            closeIf(outRd);
            closeIf(outWr);
            return makeError(ErrorKind::Io, "CreatePipe(stderr) failed");
        }
        ::SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);
    }
    if (options.pipeStdin) {
        if (!::CreatePipe(&inRd, &inWr, &sa, 0)) {
            closeIf(outRd); closeIf(outWr); closeIf(errRd); closeIf(errWr);
            return makeError(ErrorKind::Io, "CreatePipe(stdin) failed");
        }
        ::SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWr;
    si.hStdError = options.mergeStderrIntoStdout ? outWr : errWr;
    si.hStdInput = options.pipeStdin ? inRd : ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string cmdline = buildCommandLine(options.argv);

    // CREATE_NEW_PROCESS_GROUP so terminate() can reach the whole subtree.
    const BOOL ok = ::CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                                     nullptr, nullptr, &si, &pi);

    closeIf(outWr);
    closeIf(errWr);
    closeIf(inRd);

    if (!ok) {
        const DWORD err = ::GetLastError();
        closeIf(outRd); closeIf(errRd); closeIf(inWr);
        return makeError(err == ERROR_FILE_NOT_FOUND ? ErrorKind::NotFound : ErrorKind::Io,
                         "CreateProcess failed for '" + options.argv.front() +
                             "': " + std::to_string(err));
    }
    ::CloseHandle(pi.hThread);

    Subprocess proc;
    proc.argv_ = std::move(options.argv);
    proc.processHandle_ = pi.hProcess;
    proc.pid_ = pi.dwProcessId;
    proc.stdoutRead_ = outRd;
    proc.stderrRead_ = errRd;
    proc.stdinWrite_ = inWr;
    return proc;
}

bool Subprocess::valid() const noexcept { return processHandle_ != nullptr; }

Result<std::size_t> Subprocess::readStdout(std::span<char> dst, int waitMs, bool* timedOut) {
    return readHandleWithTimeout(static_cast<HANDLE>(stdoutRead_), dst, waitMs, timedOut);
}

Result<std::size_t> Subprocess::readStderr(std::span<char> dst, int waitMs, bool* timedOut) {
    return readHandleWithTimeout(static_cast<HANDLE>(stderrRead_), dst, waitMs, timedOut);
}

Status Subprocess::writeStdin(std::string_view data) {
    if (stdinWrite_ == nullptr) return makeError(ErrorKind::Io, "stdin is not piped");
    std::size_t written = 0;
    while (written < data.size()) {
        DWORD n = 0;
        if (!::WriteFile(static_cast<HANDLE>(stdinWrite_), data.data() + written,
                         static_cast<DWORD>(data.size() - written), &n, nullptr)) {
            return makeError(ErrorKind::Io,
                             "WriteFile(stdin) failed: " + std::to_string(::GetLastError()));
        }
        written += n;
    }
    return {};
}

void Subprocess::closeStdin() {
    if (stdinWrite_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(stdinWrite_));
        stdinWrite_ = nullptr;
    }
}

std::optional<int> Subprocess::tryWait() {
    if (exitCode_) return exitCode_;
    if (processHandle_ == nullptr) return std::nullopt;

    DWORD code = 0;
    if (!::GetExitCodeProcess(static_cast<HANDLE>(processHandle_), &code)) {
        exitCode_ = -1;
        return exitCode_;
    }
    if (code == STILL_ACTIVE) return std::nullopt;
    exitCode_ = static_cast<int>(code);
    return exitCode_;
}

Result<int> Subprocess::wait(std::chrono::milliseconds timeout) {
    if (processHandle_ == nullptr) return makeError(ErrorKind::Io, "no process");
    const DWORD rc = ::WaitForSingleObject(static_cast<HANDLE>(processHandle_),
                                           static_cast<DWORD>(timeout.count()));
    if (rc == WAIT_TIMEOUT) {
        return makeError(ErrorKind::Timeout, "process did not exit within timeout");
    }
    if (const auto code = tryWait()) return *code;
    return makeError(ErrorKind::Io, "WaitForSingleObject failed");
}

void Subprocess::terminate() {
    if (processHandle_ != nullptr && !exitCode_) {
        ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid_);
        ::TerminateProcess(static_cast<HANDLE>(processHandle_), 1);
    }
}

void Subprocess::killHard() { terminate(); }

void Subprocess::closeAll() noexcept {
    const auto closeIf = [](void*& h) noexcept {
        if (h != nullptr) ::CloseHandle(static_cast<HANDLE>(h));
        h = nullptr;
    };
    if (processHandle_ != nullptr && !exitCode_) {
        ::TerminateProcess(static_cast<HANDLE>(processHandle_), 1);
    }
    closeIf(stdoutRead_);
    closeIf(stderrRead_);
    closeIf(stdinWrite_);
    closeIf(processHandle_);
    pid_ = 0;
}

Result<CommandOutput> runCommand(std::vector<std::string> argv,
                                 std::chrono::milliseconds timeout,
                                 std::string_view stdinData) {
    const std::string display = quoteArgv(argv);

    Subprocess::Options opts;
    opts.argv = std::move(argv);
    opts.pipeStdin = !stdinData.empty();

    auto spawned = Subprocess::spawn(std::move(opts));
    if (!spawned) return std::move(spawned).error();
    Subprocess proc = std::move(spawned).value();

    if (!stdinData.empty()) (void)proc.writeStdin(stdinData);
    proc.closeStdin();

    CommandOutput result;
    const auto deadline = deadlineFrom(timeout);
    std::array<char, kChunk> buffer{};
    bool outOpen = true;
    bool errOpen = true;

    while (outOpen || errOpen) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        bool progressed = false;

        if (outOpen) {
            bool to = false;
            auto n = proc.readStdout(buffer, 5, &to);
            if (!n) return std::move(n).error();
            if (*n > 0) { result.out.append(buffer.data(), *n); progressed = true; }
            else if (!to) outOpen = false;
        }
        if (errOpen) {
            bool to = false;
            auto n = proc.readStderr(buffer, 5, &to);
            if (!n) return std::move(n).error();
            if (*n > 0) { result.err.append(buffer.data(), *n); progressed = true; }
            else if (!to) errOpen = false;
        }
        if (!progressed && proc.tryWait() && !outOpen && !errOpen) break;
    }

    if (std::chrono::steady_clock::now() >= deadline && !proc.tryWait()) {
        proc.terminate();
        result.timedOut = true;
        result.exitCode = -1;
        EM_LOG_WARN("proc", "timeout after " + std::to_string(timeout.count()) + "ms: " + display);
        return result;
    }

    auto code = proc.wait(std::chrono::milliseconds{2000});
    result.exitCode = code ? *code : -1;
    return result;
}

std::optional<std::filesystem::path> findOnPath(std::string_view name) {
    namespace fs = std::filesystem;

    std::array<char, MAX_PATH> buf{};
    const DWORD n = ::SearchPathA(nullptr, std::string{name}.c_str(), ".exe",
                                  static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    if (n > 0 && n < buf.size()) return fs::path{std::string{buf.data(), n}};

    const DWORD n2 = ::SearchPathA(nullptr, std::string{name}.c_str(), nullptr,
                                   static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    if (n2 > 0 && n2 < buf.size()) return fs::path{std::string{buf.data(), n2}};
    return std::nullopt;
}

#endif
// ===========================================================================

// ------------------------- shared special members --------------------------

Subprocess::~Subprocess() { closeAll(); }

Subprocess::Subprocess(Subprocess&& other) noexcept { moveFrom(std::move(other)); }

Subprocess& Subprocess::operator=(Subprocess&& other) noexcept {
    if (this != &other) {
        closeAll();
        moveFrom(std::move(other));
    }
    return *this;
}

void Subprocess::moveFrom(Subprocess&& other) noexcept {
    argv_ = std::move(other.argv_);
    exitCode_ = other.exitCode_;
#if defined(_WIN32)
    processHandle_ = std::exchange(other.processHandle_, nullptr);
    stdoutRead_ = std::exchange(other.stdoutRead_, nullptr);
    stderrRead_ = std::exchange(other.stderrRead_, nullptr);
    stdinWrite_ = std::exchange(other.stdinWrite_, nullptr);
    pid_ = std::exchange(other.pid_, 0);
#else
    pid_ = std::exchange(other.pid_, -1);
    stdoutRead_ = std::exchange(other.stdoutRead_, -1);
    stderrRead_ = std::exchange(other.stderrRead_, -1);
    stdinWrite_ = std::exchange(other.stdinWrite_, -1);
#endif
    other.exitCode_.reset();
}

}  // namespace em
