#include "core/CrashHandler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

#include "core/Log.h"
#include "core/StringUtil.h"

#if defined(_WIN32)
#  include <windows.h>
#  include <dbghelp.h>
#  pragma comment(lib, "dbghelp.lib")
#else
#  include <csignal>
#  include <cerrno>
#  include <execinfo.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <unistd.h>
#  include <sys/types.h>
#endif

namespace em::crash {
namespace {

constexpr const char* kCategory = "crash";
constexpr const char* kDirName = "crash-logs";

// Everything below the next divider has to survive the process being in an
// arbitrary, possibly corrupted state, so it avoids the heap and the
// allocator-backed standard library entirely. That rules out std::string,
// std::filesystem and the usual logging path, hence the fixed buffers and
// raw syscalls instead.
constexpr std::size_t kPathCap = 1024;
char gCrashDirPath[kPathCap] = {};
char gContextFilePath[kPathCap] = {};
std::size_t gCrashDirLen = 0;

#if !defined(_WIN32)
// 64 KiB alternate stack: SIGSEGV from a stack overflow cannot be handled on
// the overflowed stack itself, so the handler needs its own.
std::vector<char> gAltStack;
#endif

// Minimal decimal/hex integer formatting with no library calls, so it is
// safe to use from inside the signal handler.
std::size_t appendUInt(char* buf, std::size_t cap, std::size_t pos, unsigned long long value) {
    char digits[20];
    std::size_t n = 0;
    if (value == 0) digits[n++] = '0';
    while (value > 0 && n < sizeof(digits)) {
        digits[n++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    while (n > 0 && pos < cap) buf[pos++] = digits[--n];
    return pos;
}

std::size_t appendHex(char* buf, std::size_t cap, std::size_t pos, std::uintptr_t value) {
    constexpr char kHex[] = "0123456789abcdef";
    char digits[2 * sizeof(std::uintptr_t)];
    std::size_t n = 0;
    if (value == 0) digits[n++] = '0';
    while (value > 0 && n < sizeof(digits)) {
        digits[n++] = kHex[value & 0xF];
        value >>= 4;
    }
    if (pos + 2 <= cap) { buf[pos++] = '0'; buf[pos++] = 'x'; }
    while (n > 0 && pos < cap) buf[pos++] = digits[--n];
    return pos;
}

std::size_t appendLiteral(char* buf, std::size_t cap, std::size_t pos, const char* text) {
    while (*text != '\0' && pos < cap) buf[pos++] = *text++;
    return pos;
}

#if !defined(_WIN32)

void rawWrite(int fd, const char* data, std::size_t len) {
    while (len > 0) {
        const ssize_t n = ::write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        data += n;
        len -= static_cast<std::size_t>(n);
    }
}

void rawWrite(int fd, const char* text) { rawWrite(fd, text, std::strlen(text)); }

const char* signalName(int sig) noexcept {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (segmentation fault)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGFPE:  return "SIGFPE (floating point exception)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        case SIGBUS:  return "SIGBUS (bus error)";
        default:      return "unknown signal";
    }
}

// Runs on the faulting thread, on the alternate stack. Everything here has to
// stay async-signal-safe: open/write/close and backtrace_symbols_fd (which
// glibc and libSystem both document as safe here, unlike backtrace_symbols,
// because it writes straight to the fd instead of allocating). No
// std::string, no malloc, no std::mutex.
extern "C" void onFatalSignal(int sig, siginfo_t* info, void* /*ucontext*/) {
    // Restore the default disposition first so a second crash inside this
    // handler (or a genuine re-entrant fault) terminates normally instead of
    // looping.
    std::signal(sig, SIG_DFL);

    char path[kPathCap];
    std::size_t pos = 0;
    pos = appendLiteral(path, sizeof(path), pos, gCrashDirPath);
    pos = appendLiteral(path, sizeof(path), pos, "/crash-");
    pos = appendUInt(path, sizeof(path), pos, static_cast<unsigned long long>(::getpid()));
    pos = appendLiteral(path, sizeof(path), pos, "-");
    pos = appendUInt(path, sizeof(path), pos, static_cast<unsigned long long>(sig));
    pos = appendLiteral(path, sizeof(path), pos, ".txt");
    if (pos >= sizeof(path)) pos = sizeof(path) - 1;
    path[pos] = '\0';

    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { ::raise(sig); return; }

    rawWrite(fd, "emeron crash report\nsignal: ");
    rawWrite(fd, signalName(sig));
    rawWrite(fd, "\npid: ");
    {
        char buf[32];
        const std::size_t n =
            appendUInt(buf, sizeof(buf), 0, static_cast<unsigned long long>(::getpid()));
        rawWrite(fd, buf, n);
    }
    if (info != nullptr) {
        rawWrite(fd, "\nfault address: ");
        char buf[32];
        const std::size_t n = appendHex(buf, sizeof(buf), 0,
                                        reinterpret_cast<std::uintptr_t>(info->si_addr));
        rawWrite(fd, buf, n);
    }
    rawWrite(fd, "\n\nbacktrace:\n");

    void* frames[64];
    const int frameCount = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, frameCount, fd);

    // Best-effort: fold in whatever setBreadcrumb() last wrote to disk. This
    // is a plain read of an existing file, not a lock, so it stays
    // signal-safe even though the normal logging path is not reachable here.
    const int ctxFd = ::open(gContextFilePath, O_RDONLY);
    if (ctxFd >= 0) {
        rawWrite(fd, "\nlast known activity:\n");
        char buf[512];
        for (;;) {
            const ssize_t n = ::read(ctxFd, buf, sizeof(buf));
            if (n <= 0) break;
            rawWrite(fd, buf, static_cast<std::size_t>(n));
        }
        ::close(ctxFd);
    }

    ::close(fd);
    ::raise(sig);
    std::_Exit(128 + sig);
}

void installPosixHandlers() {
    gAltStack.resize(64 * 1024);
    stack_t ss{};
    ss.ss_sp = gAltStack.data();
    ss.ss_size = gAltStack.size();
    ss.ss_flags = 0;
    ::sigaltstack(&ss, nullptr);

    // Warm up lazily-loaded backtrace machinery now, in a normal context,
    // rather than paying for it for the first time inside the handler.
    void* warmup[4];
    ::backtrace(warmup, 4);

    struct sigaction action{};
    action.sa_sigaction = onFatalSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    for (const int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) {
        ::sigaction(sig, &action, nullptr);
    }
}

#else  // _WIN32

std::string symbolize(HANDLE process, DWORD64 address) {
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;

    DWORD64 displacement = 0;
    std::string text;
    if (::SymFromAddr(process, address, &displacement, symbol) != 0) {
        text = symbol->Name;
    } else {
        text = "?";
    }

    DWORD lineDisplacement = 0;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    if (::SymGetLineFromAddr64(process, address, &lineDisplacement, &line) != 0) {
        text += std::string{" ("} + line.FileName + ":" + std::to_string(line.LineNumber) + ")";
    }
    return text;
}

// Windows' SetUnhandledExceptionFilter is a last-resort callback, not a
// POSIX signal handler, so the async-signal-safety rules above do not apply
// here -- the normal standard library is fine to use.
LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;

    std::ostringstream report;
    report << "emeron crash report\n";
    report << "exception code: 0x" << std::hex << code << std::dec << "\n";
    report << "pid: " << ::GetCurrentProcessId() << "\n\n";
    report << "backtrace:\n";

    const HANDLE process = ::GetCurrentProcess();
    ::SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    ::SymInitialize(process, nullptr, TRUE);

    void* frames[64];
    const WORD frameCount = ::CaptureStackBackTrace(0, 64, frames, nullptr);
    for (WORD i = 0; i < frameCount; ++i) {
        report << "  #" << i << "  "
               << symbolize(process, reinterpret_cast<DWORD64>(frames[i])) << "\n";
    }
    ::SymCleanup(process);

    report << "\nlast known activity:\n";
    std::ifstream context{gContextFilePath};
    if (context) {
        report << context.rdbuf();
    }

    char path[kPathCap];
    std::snprintf(path, sizeof(path), "%s\\crash-%lu-%lu.txt", gCrashDirPath,
                 static_cast<unsigned long>(::GetCurrentProcessId()),
                 static_cast<unsigned long>(code));

    const std::string text = report.str();
    HANDLE file = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        ::CloseHandle(file);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

#endif

// --- the rest of this file runs in normal, non-signal context --------------

std::mutex gBreadcrumbMutex;
std::string gLastBreadcrumbText;  // de-dupes so a hot call site is not a hot file write

}  // namespace

void install(const std::filesystem::path& workspaceDir) {
    std::error_code ec;
    const std::filesystem::path dir = workspaceDir / kDirName;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        EM_LOG_WARN(kCategory, "could not create " + dir.string() + ": " + ec.message());
        return;
    }

    const std::string dirStr = dir.string();
    gCrashDirLen = std::min(dirStr.size(), kPathCap - 1);
    std::memcpy(gCrashDirPath, dirStr.data(), gCrashDirLen);
    gCrashDirPath[gCrashDirLen] = '\0';

    const std::string contextStr = (dir / ".context").string();
    const std::size_t contextLen = std::min(contextStr.size(), kPathCap - 1);
    std::memcpy(gContextFilePath, contextStr.data(), contextLen);
    gContextFilePath[contextLen] = '\0';

#if defined(_WIN32)
    ::SetUnhandledExceptionFilter(onUnhandledException);
#else
    installPosixHandlers();
#endif

    // A report left over from a previous run means that run did not shut
    // down cleanly -- worth a line in the log even before anyone opens the
    // Crash Logs panel.
    const auto previous = listReports();
    if (!previous.empty()) {
        EM_LOG_WARN(kCategory, std::to_string(previous.size()) +
                                   " crash report(s) found from a previous run in " + dirStr +
                                   " -- see the Crash Logs panel");
    }
}

std::filesystem::path logDirectory() { return std::filesystem::path{gCrashDirPath}; }

void setBreadcrumb(std::string_view text) {
    if (gCrashDirPath[0] == '\0') return;  // install() has not run (e.g. in tests)

    const std::lock_guard lock{gBreadcrumbMutex};
    if (text == gLastBreadcrumbText) return;
    gLastBreadcrumbText = std::string{text};

    const auto now = std::chrono::system_clock::now();
    const auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &timeT);
#else
    localtime_r(&timeT, &parts);
#endif
    const std::string line =
        format("%02d:%02d:%02d  ", parts.tm_hour, parts.tm_min, parts.tm_sec) + std::string{text} + "\n";

    // Written as a whole-file replace so a reader (or the crash handler,
    // across the next crash) never sees a half-written line.
    const std::filesystem::path tmp = std::filesystem::path{gContextFilePath}.string() + ".tmp";
    std::ofstream out{tmp, std::ios::trunc};
    if (!out) return;
    out << line;
    out.close();

    std::error_code ec;
    std::filesystem::rename(tmp, gContextFilePath, ec);
}

std::vector<CrashReport> listReports() {
    std::vector<CrashReport> reports;
    if (gCrashDirPath[0] == '\0') return reports;

    std::error_code ec;
    for (const auto& entry :
        std::filesystem::directory_iterator{std::filesystem::path{gCrashDirPath}, ec}) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".txt") continue;

        CrashReport report;
        report.path = entry.path();
        report.fileName = entry.path().filename().string();
        report.sizeBytes = entry.file_size(ec);
        report.modified = entry.last_write_time(ec);
        reports.push_back(std::move(report));
    }

    std::sort(reports.begin(), reports.end(),
             [](const CrashReport& a, const CrashReport& b) { return a.modified > b.modified; });
    return reports;
}

std::string readReport(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

}  // namespace em::crash
