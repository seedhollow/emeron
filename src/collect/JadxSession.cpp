#include "collect/JadxSession.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "core/Log.h"
#include "core/StringUtil.h"
#include "core/TaskQueue.h"

namespace em {
namespace fs = std::filesystem;

namespace {

constexpr const char* kCategory = "jadx";
constexpr std::size_t kStderrKeep = 16 * 1024;
constexpr std::size_t kMaxFrame = 512U * 1024U * 1024U;

bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool isDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

std::string envOr(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string{v} : std::string{};
}

fs::path homeDir() {
    std::string home = envOr("HOME");
    if (home.empty()) home = envOr("USERPROFILE");
    return home;
}

// The jadx jar directly inside `dir`, preferring the fat jar.
std::optional<fs::path> jarInDir(const fs::path& dir) {
    if (!isDir(dir)) return std::nullopt;
    std::optional<fs::path> any;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with("jadx") || !name.ends_with(".jar")) continue;
        if (name.ends_with("-all.jar")) return entry.path();
        if (!any) any = entry.path();
    }
    return any;
}

std::optional<fs::path> jarInInstall(const fs::path& dir) {
    for (const auto& sub : {fs::path{"lib"}, fs::path{"libexec"} / "lib", fs::path{}}) {
        if (auto jar = jarInDir(sub.empty() ? dir : dir / sub)) return jar;
    }
    return std::nullopt;
}

// Newest-looking subdirectory, e.g. the highest version under Cellar/jadx.
std::vector<fs::path> subdirsNewestFirst(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!isDir(dir)) return out;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_directory(ec)) out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end(), std::greater<>{});
    return out;
}

std::optional<fs::path> findJava() {
    const std::string exe =
#if defined(_WIN32)
        "java.exe";
#else
        "java";
#endif
    if (const std::string home = envOr("JAVA_HOME"); !home.empty()) {
        const fs::path java = fs::path{home} / "bin" / exe;
        if (isFile(java)) return java;
    }
    if (auto java = findOnPath("java")) return java;
    for (const char* p :
         {"/opt/homebrew/opt/openjdk/bin/java", "/usr/local/opt/openjdk/bin/java"}) {
        if (isFile(p)) return fs::path{p};
    }
    return std::nullopt;
}

JadxFrame::Status parseStatus(std::string_view s, bool& ok) {
    ok = true;
    if (s == "ok") return JadxFrame::Status::Ok;
    if (s == "err") return JadxFrame::Status::Err;
    if (s == "part") return JadxFrame::Status::Part;
    ok = false;
    return JadxFrame::Status::Err;
}

std::string clean(std::string_view arg) {
    std::string out{arg};
    std::replace_if(
        out.begin(), out.end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; },
        ' ');
    return out;
}

}  // namespace

// --- finding jadx -------------------------------------------------------------------------

std::optional<fs::path> jadxJarIn(const fs::path& path) {
    if (path.empty()) return std::nullopt;
    if (isFile(path) && path.extension() == ".jar") return path;
    if (isDir(path)) return jarInInstall(path);
    if (!isFile(path)) return std::nullopt;

    // A launcher: bin/jadx next to lib/, possibly reached through a symlink or
    // a wrapper script (Homebrew's execs the real one by absolute path).
    std::error_code ec;
    const fs::path real = fs::weakly_canonical(path, ec);
    if (auto jar = jarInInstall((ec ? path : real).parent_path().parent_path())) return jar;
    if (fs::file_size(path, ec) < 64 * 1024) {
        std::ifstream in{path, std::ios::binary};
        std::stringstream text;
        text << in.rdbuf();
        const std::string script = text.str();
        for (std::size_t at = script.find("/bin/jadx"); at != std::string::npos;
             at = script.find("/bin/jadx", at + 1)) {
            std::size_t start = script.find_last_of("\"' \t\n=", at);
            start = start == std::string::npos ? 0 : start + 1;
            const fs::path bin{script.substr(start, at - start)};
            if (auto jar = jarInInstall(bin)) return jar;
        }
    }
    return std::nullopt;
}

JadxSetup findJadx(const fs::path& jadxOverride) {
    JadxSetup setup;
    std::optional<fs::path> jar;
    if (!jadxOverride.empty()) {
        jar = jadxJarIn(jadxOverride);
        if (!jar) {
            setup.error = "No jadx jar in " + jadxOverride.string() +
                          ". Point at a jadx folder (the one with bin/ and lib/), its "
                          "bin/jadx, or jadx-<version>-all.jar.";
            return setup;
        }
    }
    if (!jar) jar = jadxJarIn(envOr("EMERON_JADX"));
    if (!jar) {
        if (auto launcher = findOnPath("jadx")) jar = jadxJarIn(*launcher);
    }
    if (!jar) {
        std::vector<fs::path> candidates;
        for (const char* cellar : {"/opt/homebrew/Cellar/jadx", "/usr/local/Cellar/jadx"}) {
            for (auto& d : subdirsNewestFirst(cellar)) candidates.push_back(d);
        }
        for (const char* d : {"/usr/share/jadx", "/usr/lib/jadx", "/opt/jadx"})
            candidates.emplace_back(d);
        const fs::path home = homeDir();
        if (!home.empty()) {
            candidates.push_back(home / "jadx");
            candidates.push_back(home / ".local" / "share" / "jadx");
            candidates.push_back(home / "scoop" / "apps" / "jadx" / "current");
        }
        if (const std::string local = envOr("LOCALAPPDATA"); !local.empty()) {
            candidates.push_back(fs::path{local} / "jadx");
        }
        for (const auto& c : candidates) {
            if ((jar = jarInInstall(c))) break;
        }
    }
    if (!jar) {
        setup.error =
            "jadx was not found. Install it (`brew install jadx`, your package manager, or "
            "unzip a release from github.com/skylot/jadx/releases), then put its bin/ on "
            "PATH or use Locate jadx.";
        return setup;
    }
    setup.jar = *jar;
    auto java = findJava();
    if (!java) {
        setup.error =
            "Java was not found. jadx needs a JDK 11 or newer (`brew install openjdk`, "
            "Temurin, or your package manager); set JAVA_HOME or put java on PATH.";
        return setup;
    }
    setup.java = *java;
    return setup;
}

// --- protocol -------------------------------------------------------------------------------

std::optional<JadxFrame> JadxFrameReader::next() {
    const std::string_view rest = std::string_view{buffer_}.substr(consumed_);
    const std::size_t nl = rest.find('\n');
    if (nl == std::string_view::npos) {
        if (rest.size() > 256) {  // no header is that long: resynchronise
            buffer_.clear();
            consumed_ = 0;
            return JadxFrame{-1, JadxFrame::Status::Err, "malformed output from jadx"};
        }
        return std::nullopt;
    }
    const std::string_view header = rest.substr(0, nl);
    const auto parts = split(header, ' ');
    bool statusOk = false;
    const auto id = parts.size() == 3 && header.starts_with('@')
                        ? parseNumber<int>(parts[0].substr(1))
                        : std::nullopt;
    const auto length = parts.size() == 3 ? parseNumber<std::size_t>(parts[2]) : std::nullopt;
    const auto status =
        parts.size() == 3 ? parseStatus(parts[1], statusOk) : JadxFrame::Status::Err;
    if (!id || !length || !statusOk || *length > kMaxFrame) {
        // Not a frame header: something else wrote to stdout. Skip the line.
        consumed_ += nl + 1;
        return JadxFrame{-1, JadxFrame::Status::Err,
                         "unexpected output: " + std::string{header}};
    }
    if (rest.size() < nl + 1 + *length) return std::nullopt;
    JadxFrame frame{*id, status, std::string{rest.substr(nl + 1, *length)}};
    consumed_ += nl + 1 + *length;
    if (consumed_ > 64 * 1024 && consumed_ * 2 > buffer_.size()) {
        buffer_.erase(0, consumed_);
        consumed_ = 0;
    }
    return frame;
}

std::string jadxRequestLine(int id, std::string_view command,
                            const std::vector<std::string>& args) {
    std::string line = std::to_string(id);
    line += '\t';
    line += clean(command);
    for (const auto& a : args) {
        line += '\t';
        line += clean(a);
    }
    line += '\n';
    return line;
}

std::string_view JadxClass::packageName() const {
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string_view{}
                                    : std::string_view{name}.substr(0, dot);
}

std::string_view JadxClass::simpleName() const {
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string_view{name}
                                    : std::string_view{name}.substr(dot + 1);
}

std::vector<JadxClass> parseJadxClasses(std::string_view payload) {
    std::vector<JadxClass> out;
    for (const auto line : splitLines(payload)) {
        const auto f = split(line, '\t');
        if (f.size() < 2 || f[0].empty()) continue;
        JadxClass c;
        c.raw = std::string{f[0]};
        c.name = f[1].empty() ? c.raw : std::string{f[1]};
        if (f.size() > 2) {
            for (const char ch : f[2]) {
                if (ch == 'n')
                    c.noCode = true;
                else
                    c.kind = ch;
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

Result<DecompiledCode> parseJadxCode(std::string_view payload) {
    const std::size_t nl = payload.find('\n');
    const auto length = nl == std::string_view::npos
                            ? std::nullopt
                            : parseNumber<std::size_t>(payload.substr(0, nl));
    if (!length || payload.size() < nl + 1 + *length) {
        return makeError(ErrorKind::Protocol, "truncated code from jadx");
    }
    DecompiledCode code;
    code.text = std::string{payload.substr(nl + 1, *length)};
    for (const auto line : splitLines(payload.substr(nl + 1 + *length))) {
        const auto f = split(line, '\t');
        if (f.size() != 2 || f[1].size() != 1) continue;
        const auto pos = parseNumber<std::uint32_t>(f[0]);
        if (!pos || *pos >= code.text.size()) continue;
        code.refs.push_back({*pos, f[1][0]});
    }
    std::sort(code.refs.begin(), code.refs.end(),
              [](const CodeRef& a, const CodeRef& b) { return a.pos < b.pos; });
    return code;
}

Result<JadxResolved> parseJadxResolved(std::string_view payload) {
    const auto f = split(trimRight(payload), '\t');
    const auto pos = f.size() >= 2 ? parseNumber<std::uint32_t>(f[1]) : std::nullopt;
    if (!pos || f[0].empty()) return makeError(ErrorKind::Protocol, "bad answer from jadx");
    return JadxResolved{std::string{f[0]}, *pos,
                        f.size() > 2 ? std::string{f[2]} : std::string{}};
}

CodeHitChunk parseJadxHits(std::string_view payload) {
    CodeHitChunk chunk;
    for (const auto line : splitLines(payload)) {
        const auto f = split(line, '\t');
        if (f.size() == 3 && f[0] == "P") {
            chunk.done = parseNumber<int>(f[1]).value_or(-1);
            chunk.total = parseNumber<int>(f[2]).value_or(-1);
        } else if (f.size() == 1 && f[0] == "L") {
            chunk.limited = true;
        } else if (f.size() == 2 && f[0] == "N") {
            chunk.node = std::string{f[1]};
        } else if (f.size() == 2 && f[0] == "T") {
            chunk.usageCount = parseNumber<int>(f[1]).value_or(-1);
        } else if (f.size() >= 4) {
            const auto pos = parseNumber<std::uint32_t>(f[2]);
            if (!pos) continue;
            // The line itself may hold tabs only if jadx printed them; the
            // bridge replaces them, but keep any remainder anyway.
            const std::size_t lineAt = static_cast<std::size_t>(f[3].data() - line.data());
            chunk.hits.push_back(
                {std::string{f[0]}, std::string{f[1]}, *pos, std::string{line.substr(lineAt)}});
        }
    }
    return chunk;
}

std::vector<JadxResource> parseJadxResources(std::string_view payload) {
    std::vector<JadxResource> out;
    for (const auto line : splitLines(payload)) {
        const auto f = split(line, '\t');
        if (f.empty() || f[0].empty()) continue;
        out.push_back({std::string{f[0]}, f.size() > 1 ? std::string{f[1]} : std::string{}});
    }
    return out;
}

JadxResourceContent parseJadxResource(std::string_view payload) {
    JadxResourceContent c;
    const std::size_t nl = payload.find('\n');
    const std::string_view head = payload.substr(0, nl);
    const std::string_view body =
        nl == std::string_view::npos ? std::string_view{} : payload.substr(nl + 1);
    if (head == "t") {
        c.kind = JadxResourceContent::Kind::Text;
        c.text = std::string{body};
    } else if (head == "l") {
        c.kind = JadxResourceContent::Kind::List;
        for (const auto line : splitLines(body)) {
            if (!line.empty()) c.children.emplace_back(line);
        }
    } else {
        c.kind = JadxResourceContent::Kind::Binary;
        const auto f = split(head, '\t');
        if (f.size() == 2) c.size = parseNumber<std::uint64_t>(f[1]).value_or(0);
    }
    return c;
}

// --- the process --------------------------------------------------------------------------

JadxSession::JadxSession(Dispatcher& dispatcher) : dispatcher_(dispatcher) {}

JadxSession::~JadxSession() {
    if (process_.valid()) {
        (void)process_.writeStdin("0\tquit\n");
        process_.closeStdin();
    }
    stop_ = true;
    if (reader_.joinable()) reader_.join();
    if (process_.valid() && !process_.tryWait()) {
        if (!process_.wait(std::chrono::milliseconds{300})) process_.terminate();
    }
}

Status JadxSession::start(const JadxSetup& setup, const std::vector<fs::path>& apks,
                          bool deobfuscate, std::string_view bridgeSource,
                          const fs::path& workDir, Handler onLoaded) {
    if (!setup.ok()) return makeError(ErrorKind::NotFound, setup.error);
    std::error_code ec;
    fs::create_directories(workDir, ec);
    const fs::path source = workDir / "JadxBridge.java";
    {
        std::ofstream out{source, std::ios::binary | std::ios::trunc};
        out.write(bridgeSource.data(), static_cast<std::streamsize>(bridgeSource.size()));
        if (!out) return makeError(ErrorKind::Io, "cannot write " + source.string());
    }

    Subprocess::Options options;
    options.argv = {setup.java.string(),
                    "-XX:+IgnoreUnrecognizedVMOptions",
                    "-Xms256M",
                    "-XX:MaxRAMPercentage=50.0",
                    "-Xss8m",
                    "-Djdk.util.zip.disableZip64ExtraFieldValidation=true",
                    "-cp",
                    setup.jar.string(),
                    source.string()};
    if (deobfuscate) options.argv.emplace_back("--deobf");
    for (const auto& apk : apks) options.argv.push_back(apk.string());
    options.pipeStdin = true;

    auto spawned = Subprocess::spawn(std::move(options));
    if (!spawned) return spawned.error();
    process_ = std::move(*spawned);
    {
        const std::lock_guard lock{mutex_};
        handlers_[0] = std::move(onLoaded);
    }
    running_ = true;
    reader_ = std::thread{[this] { readLoop(); }};
    return {};
}

int JadxSession::request(std::string_view command, std::vector<std::string> args,
                         Handler handler) {
    if (!running_) return -1;
    int id = 0;
    {
        const std::lock_guard lock{mutex_};
        id = nextId_++;
        handlers_[id] = std::move(handler);
    }
    if (!process_.writeStdin(jadxRequestLine(id, command, args))) {
        const std::lock_guard lock{mutex_};
        handlers_.erase(id);
        return -1;
    }
    return id;
}

void JadxSession::cancel(int id) {
    if (!running_ || id <= 0) return;
    (void)process_.writeStdin(jadxRequestLine(0, "cancel", {std::to_string(id)}));
}

std::string JadxSession::stderrTail() const {
    const std::lock_guard lock{mutex_};
    return stderr_;
}

void JadxSession::readLoop() {
    JadxFrameReader reader;
    std::array<char, 64 * 1024> buf{};
    std::array<char, 8 * 1024> errBuf{};  // never buf: stdout bytes wait in it until parsed
    while (!stop_) {
        bool timedOut = false;
        auto n = process_.readStdout(buf, 100, &timedOut);
        // Drain stderr too, or a chatty JVM blocks on a full pipe.
        for (;;) {
            bool errTimedOut = false;
            auto e = process_.readStderr(errBuf, 0, &errTimedOut);
            if (!e || *e == 0) break;
            const std::lock_guard lock{mutex_};
            stderr_.append(errBuf.data(), *e);
            if (stderr_.size() > kStderrKeep) stderr_.erase(0, stderr_.size() - kStderrKeep);
        }
        if (!n) break;
        if (*n == 0) {
            if (timedOut) continue;
            break;  // EOF: the JVM exited
        }
        reader.feed(std::string_view{buf.data(), *n});
        while (auto frame = reader.next()) {
            if (frame->id < 0) {
                EM_LOG_WARN(kCategory, frame->payload);
                continue;
            }
            Handler handler;
            {
                const std::lock_guard lock{mutex_};
                auto it = handlers_.find(frame->id);
                if (it == handlers_.end()) continue;
                handler = frame->final() ? std::move(it->second) : it->second;
                if (frame->final()) handlers_.erase(it);
            }
            dispatcher_.post(
                [handler = std::move(handler), f = std::move(*frame)] { handler(f); });
        }
    }
    running_ = false;
    if (stop_) return;
    // Collect what the JVM said on the way out.
    for (int i = 0; i < 20; ++i) {
        bool errTimedOut = false;
        auto e = process_.readStderr(errBuf, 50, &errTimedOut);
        if (!e || (*e == 0 && !errTimedOut)) break;
        if (*e == 0) continue;
        const std::lock_guard lock{mutex_};
        stderr_.append(errBuf.data(), *e);
    }
    std::string reason = "jadx stopped";
    {
        const std::lock_guard lock{mutex_};
        const auto lines = splitLines(trim(stderr_));
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            const auto l = trim(*it);
            if (!l.empty() && !l.starts_with("at ") && !l.starts_with("INFO")) {
                reason += ": " + std::string{l};
                break;
            }
        }
    }
    failPending(reason);
}

void JadxSession::failPending(const std::string& message) {
    std::map<int, Handler> pending;
    {
        const std::lock_guard lock{mutex_};
        pending.swap(handlers_);
    }
    for (auto& entry : pending) {
        dispatcher_.post(
            [handler = std::move(entry.second),
             f = JadxFrame{entry.first, JadxFrame::Status::Err, message}] { handler(f); });
    }
}

}  // namespace em
