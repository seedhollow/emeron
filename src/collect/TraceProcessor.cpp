#include "collect/TraceProcessor.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <utility>

#include "core/Log.h"
#include "core/StringUtil.h"

namespace em {
namespace {

namespace fs = std::filesystem;

constexpr const char* kCategory = "traceproc";

#if defined(_WIN32)
constexpr const char* kBinaryName = "trace_processor_shell.exe";
#else
constexpr const char* kBinaryName = "trace_processor_shell";
#endif

// Perfetto's documented install (curl get.perfetto.dev/trace_processor) gives a
// Python launcher with this name, not the native binary. It forwards its
// arguments to the real trace_processor_shell, which it downloads on first run
// and caches -- so it is a working fallback, and its cache is a better one.
constexpr const char* kLauncherName = "trace_processor";

std::optional<fs::path> envPath(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return fs::path{value};
}

// Strips the quoting trace_processor_shell puts around string cells.
std::string unquoteCell(std::string_view cell) {
    std::string_view t = trim(cell);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        t = t.substr(1, t.size() - 2);
    }
    if (t == "[NULL]" || t == "NULL") return {};
    return std::string{t};
}

// Splits a CSV line, honouring double-quoted fields that contain commas.
std::vector<std::string> splitCsvLine(std::string_view line) {
    std::vector<std::string> fields;
    std::string current;
    bool inQuotes = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    current.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                current.push_back(c);
            }
        } else if (c == '"') {
            inQuotes = true;
        } else if (c == ',') {
            fields.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    fields.push_back(std::move(current));
    for (auto& field : fields) field = unquoteCell(field);
    return fields;
}

bool looksLikeBoxBorder(std::string_view line) {
    const std::string_view t = trim(line);
    if (t.empty()) return false;
    return t.find_first_not_of("-+=|_ ") == std::string_view::npos;
}

}  // namespace

std::string cleanTraceProcessorError(std::string_view text) {
    // trace_processor_shell's stderr on a failed query is the load progress
    // (rewritten in place with \r), a timestamped log line, then a Python-style
    // "Traceback" block whose last line is the actual error. Only the error and
    // its position are worth showing.
    std::string location;
    std::string message;

    std::string normalized{text};
    std::replace(normalized.begin(), normalized.end(), '\r', '\n');

    for (const auto rawLine : splitLines(normalized)) {
        const std::string_view line = trim(rawLine);
        if (line.empty()) continue;
        if (line.starts_with("Loading trace")) continue;
        if (line.starts_with("Traceback")) continue;
        // "[939.710]     common_flags.cc:525 Trace loaded: ..."
        if (line.starts_with('[') && line.find(".cc:") != std::string_view::npos) continue;
        // A caret pointing at the error column.
        if (line.find_first_not_of("^~ ") == std::string_view::npos) continue;

        // File "stdin" line 1 col 31
        if (line.starts_with("File \"")) {
            const auto at = line.find(" line ");
            if (at != std::string_view::npos) location = std::string{line.substr(at + 1)};
            message.clear();  // what follows is the echoed SQL, then the error
            continue;
        }
        message = std::string{line};
    }

    if (message.empty()) return std::string{trim(text)};
    return location.empty() ? message : location + ": " + message;
}

Result<QueryResult> parseQueryOutput(std::string_view text) {
    QueryResult result;

    // Errors arrive on stdout too, as a line starting with "Error" or
    // "[ERROR]"; treat them as failures rather than as data.
    for (const auto line : splitLines(text)) {
        const std::string_view t = trim(line);
        if (t.starts_with("Error:") || t.starts_with("[ERROR") ||
            t.starts_with("Query failed")) {
            return makeError(ErrorKind::ParseFailure, std::string{t});
        }
    }

    std::vector<std::string_view> dataLines;
    for (const auto line : splitLines(text)) {
        const std::string_view t = trimRight(line);
        if (trim(t).empty()) continue;
        if (looksLikeBoxBorder(t)) continue;
        // The shell prints a banner and timing lines around the table.
        if (trim(t).starts_with("Trace loaded:")) continue;
        if (trim(t).starts_with("Query took")) continue;
        dataLines.push_back(t);
    }

    if (dataLines.empty()) {
        // A statement such as CREATE VIEW legitimately returns nothing.
        return result;
    }

    // Pick the delimiter from the header line.
    const std::string_view header = dataLines.front();
    const bool pipeDelimited = header.find('|') != std::string_view::npos;
    const bool tabDelimited = !pipeDelimited && header.find('\t') != std::string_view::npos;

    const auto splitRow = [&](std::string_view line) {
        std::vector<std::string> fields;
        if (pipeDelimited) {
            for (const auto field : split(line, '|', true)) fields.push_back(unquoteCell(field));
            // Box-drawn tables have empty leading/trailing cells.
            if (!fields.empty() && fields.front().empty()) fields.erase(fields.begin());
            if (!fields.empty() && fields.back().empty()) fields.pop_back();
        } else if (tabDelimited) {
            for (const auto field : split(line, '\t', true)) fields.push_back(unquoteCell(field));
        } else {
            fields = splitCsvLine(line);
        }
        return fields;
    };

    result.columns = splitRow(header);
    if (result.columns.empty()) {
        return makeError(ErrorKind::ParseFailure, "could not read a header row");
    }

    result.rows.reserve(dataLines.size() - 1);
    for (std::size_t i = 1; i < dataLines.size(); ++i) {
        auto fields = splitRow(dataLines[i]);
        if (fields.empty()) continue;
        fields.resize(result.columns.size());
        result.rows.push_back(std::move(fields));
    }
    return result;
}

std::vector<fs::path> TraceProcessor::searchCandidates() {
    std::vector<fs::path> out;
    if (const auto p = envPath("EMERON_TRACE_PROCESSOR")) out.push_back(*p);
    if (const auto home = envPath("HOME")) {
        out.push_back(*home / ".local" / "bin" / kBinaryName);
        out.push_back(*home / "bin" / kBinaryName);
        // Where Perfetto's own install script puts it.
        out.push_back(*home / ".perfetto" / kBinaryName);
    }
    out.emplace_back(fs::path{"/opt/homebrew/bin"} / kBinaryName);
    out.emplace_back(fs::path{"/usr/local/bin"} / kBinaryName);
    out.emplace_back(fs::path{"/usr/bin"} / kBinaryName);
    return out;
}

TraceProcessor::TraceProcessor(std::optional<fs::path> explicitBinary) {
    relocate(std::move(explicitBinary));
}

TraceProcessor::~TraceProcessor() { stopHttpd(); }

namespace {

// The newest native binary the Perfetto launcher has downloaded, if any. It
// names them trace_processor_shell-<sha prefix> in ~/.local/share/perfetto/prebuilts.
std::optional<fs::path> launcherCachedBinary() {
    const auto home = envPath("HOME");
    if (!home) return std::nullopt;

    std::optional<fs::path> newest;
    fs::file_time_type newestTime{};
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator{*home / ".local" / "share" / "perfetto" / "prebuilts", ec}) {
        const std::string name = entry.path().filename().string();
        // ".tmp" files are interrupted downloads.
        if (!name.starts_with("trace_processor_shell") || name.ends_with(".tmp")) continue;
        if (!entry.is_regular_file(ec)) continue;
        const auto when = entry.last_write_time(ec);
        if (!newest || when > newestTime) {
            newest = entry.path();
            newestTime = when;
        }
    }
    return newest;
}

}  // namespace

void TraceProcessor::relocate(std::optional<fs::path> explicitBinary) {
    TraceProcessorState next;
    {
        const std::lock_guard lock{mutex_};
        binary_.reset();
        std::string origin;

        std::error_code ec;
        if (explicitBinary && fs::is_regular_file(*explicitBinary, ec)) {
            binary_ = *explicitBinary;
            origin = "--trace-processor";
        }
        // Documented as equivalent to --trace-processor, so it outranks PATH.
        if (!binary_) {
            if (const auto env = envPath("EMERON_TRACE_PROCESSOR");
                env && fs::is_regular_file(*env, ec)) {
                binary_ = *env;
                origin = "EMERON_TRACE_PROCESSOR";
            }
        }
        if (!binary_) {
            if (auto found = findOnPath(kBinaryName)) {
                binary_ = *found;
                origin = "PATH";
            }
        }
        // Explicit directories too: an app launched from Finder or an IDE does
        // not inherit the PATH a terminal sets up in ~/.zshrc.
        if (!binary_) {
            for (const auto& candidate : searchCandidates()) {
                if (fs::is_regular_file(candidate, ec)) {
                    binary_ = candidate;
                    origin = "standard location";
                    break;
                }
            }
        }
        // Only Perfetto's launcher was installed. Prefer the native binary it
        // has already downloaded: no Python, no per-query interpreter start.
        if (!binary_) {
            if (auto cached = launcherCachedBinary()) {
                binary_ = *cached;
                origin = "downloaded by Perfetto's trace_processor launcher";
            }
        }
        if (!binary_) {
            std::optional<fs::path> launcher = findOnPath(kLauncherName);
            if (!launcher) {
                for (const auto& dir : {fs::path{"/usr/local/bin"}, fs::path{"/opt/homebrew/bin"}}) {
                    if (fs::is_regular_file(dir / kLauncherName, ec)) {
                        launcher = dir / kLauncherName;
                        break;
                    }
                }
            }
            if (!launcher) {
                if (const auto home = envPath("HOME");
                    home && fs::is_regular_file(*home / ".local" / "bin" / kLauncherName, ec)) {
                    launcher = *home / ".local" / "bin" / kLauncherName;
                }
            }
            if (launcher) {
                binary_ = *launcher;
                origin = "Perfetto's trace_processor launcher (needs python3)";
            }
        }

        next.binaryFound = binary_.has_value();
        next.binaryPath = binary_ ? binary_->string() : std::string{};
        next.binaryOrigin = origin;
        next.tracePath = tracePath_;
        next.traceLoaded = !tracePath_.empty();
        next.httpdPort = httpdPort_;
    }

    if (!next.binaryFound) {
        next.lastError =
            "trace_processor_shell not found. Install Perfetto's "
            "https://get.perfetto.dev/trace_processor (as trace_processor or "
            "trace_processor_shell) into ~/.local/bin, /usr/local/bin or PATH, or "
            "set EMERON_TRACE_PROCESSOR.";
        EM_LOG_WARN(kCategory, next.lastError);
    } else {
        EM_LOG_INFO(kCategory, "using " + next.binaryPath + " (" + next.binaryOrigin + ")");
        if (auto out = runCommand({next.binaryPath, "--version"}, std::chrono::seconds{5});
            out && out->ok()) {
            const auto lines = splitLines(out->out);
            if (!lines.empty()) next.version = std::string{trim(lines.front())};
        }
    }
    state_.set(std::move(next));
}

bool TraceProcessor::available() const {
    const std::lock_guard lock{mutex_};
    return binary_.has_value();
}

void TraceProcessor::setTransport(TraceProcessorTransport newTransport) {
    transport_.store(newTransport, std::memory_order_release);
    if (newTransport == TraceProcessorTransport::Batch) stopHttpd();
}

Status TraceProcessor::openTrace(fs::path tracePath) {
    std::error_code ec;
    if (!fs::is_regular_file(tracePath, ec)) {
        return makeError(ErrorKind::NotFound, "no such trace file: " + tracePath.string());
    }
    if (!available()) {
        return makeError(ErrorKind::NotFound, "trace_processor_shell not found");
    }

    // Copy the path out from under mutex_ before touching state_. startHttpd
    // and stopHttpd take mutex_ and then state_'s lock, so acquiring them in
    // the other order here would be a lock-order inversion.
    std::filesystem::path opened;
    {
        const std::lock_guard lock{mutex_};
        tracePath_ = std::move(tracePath);
        opened = tracePath_;
    }
    state_.mutate([&opened](TraceProcessorState& s) {
        s.tracePath = opened;
        s.traceLoaded = true;
        s.lastError.clear();
    });

    if (transport() == TraceProcessorTransport::Rpc) {
        stopHttpd();
        return startHttpd(httpdPort_);
    }
    return {};
}

void TraceProcessor::closeTrace() {
    stopHttpd();
    {
        const std::lock_guard lock{mutex_};
        tracePath_.clear();
    }
    state_.mutate([](TraceProcessorState& s) {
        s.tracePath.clear();
        s.traceLoaded = false;
    });
}

Status TraceProcessor::startHttpd(std::uint16_t port) {
    const std::lock_guard lock{mutex_};
    if (!binary_) return makeError(ErrorKind::NotFound, "trace_processor_shell not found");
    if (tracePath_.empty()) return makeError(ErrorKind::Unsupported, "no trace opened");

    httpdPort_ = port;
    const std::string listen = "127.0.0.1:" + std::to_string(port);

    Subprocess::Options options;
    options.argv = {binary_->string(), "--httpd", "--http-port", std::to_string(port),
                    tracePath_.string()};
    options.mergeStderrIntoStdout = true;

    auto spawned = Subprocess::spawn(std::move(options));
    if (!spawned) return std::move(spawned).error();

    httpd_ = std::make_unique<Subprocess>(std::move(spawned).value());
    http_ = std::make_unique<HttpClient>("127.0.0.1", port);

    // The daemon parses the whole trace before it binds, so give it room.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto code = httpd_->tryWait()) {
            httpd_.reset();
            return makeError(ErrorKind::ProcessFailed,
                             "trace_processor httpd exited with code " + std::to_string(*code));
        }
        if (http_->canConnect(200)) {
            EM_LOG_INFO(kCategory, "httpd listening on " + listen);
            state_.mutate([port](TraceProcessorState& s) {
                s.httpdRunning = true;
                s.httpdPort = port;
            });
            return {};
        }
    }

    httpd_->terminate();
    httpd_.reset();
    return makeError(ErrorKind::Timeout, "trace_processor httpd did not start listening");
}

void TraceProcessor::stopHttpd() {
    const std::lock_guard lock{mutex_};
    if (httpd_) {
        httpd_->terminate();
        httpd_.reset();
    }
    http_.reset();
    state_.mutate([](TraceProcessorState& s) { s.httpdRunning = false; });
}

bool TraceProcessor::httpdAlive() const {
    const std::lock_guard lock{mutex_};
    return httpd_ != nullptr && http_ != nullptr;
}

Result<QueryResult> TraceProcessor::query(std::string sql, std::chrono::milliseconds timeout) {
    if (trim(sql).empty()) return makeError(ErrorKind::Unsupported, "empty query");

    const auto chosen = transport();
    auto result = (chosen == TraceProcessorTransport::Rpc) ? queryRpc(sql, timeout)
                                                           : queryBatch(sql, timeout);
    if (!result) {
        state_.mutate([msg = result.error().message](TraceProcessorState& s) {
            s.lastError = msg;
        });
    } else {
        state_.mutate([](TraceProcessorState& s) { s.lastError.clear(); });
    }
    return result;
}

Result<QueryResult> TraceProcessor::queryBatch(const std::string& sql,
                                               std::chrono::milliseconds timeout) {
    fs::path binary;
    fs::path trace;
    {
        const std::lock_guard lock{mutex_};
        if (!binary_) return makeError(ErrorKind::NotFound, "trace_processor_shell not found");
        if (tracePath_.empty()) return makeError(ErrorKind::Unsupported, "no trace opened");
        binary = *binary_;
        trace = tracePath_;
    }

    // trace_processor_shell reads the query from a file; passing it on the
    // command line would hit shell-quoting problems with multi-line SQL.
    static std::atomic<std::uint64_t> queryCounter{0};
    std::error_code ec;
    const fs::path scratch =
        fs::temp_directory_path(ec) /
        ("emeron-query-" +
         std::to_string(queryCounter.fetch_add(1, std::memory_order_relaxed)) + ".sql");
    {
        std::ofstream file{scratch, std::ios::binary | std::ios::trunc};
        if (!file) return makeError(ErrorKind::Io, "cannot write " + scratch.string());
        file << sql;
        if (!sql.empty() && sql.back() != '\n') file << '\n';
    }

    const auto started = std::chrono::steady_clock::now();
    auto output = runCommand({binary.string(), "-q", scratch.string(), trace.string()}, timeout);
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();

    fs::remove(scratch, ec);

    if (!output) return std::move(output).error();
    const CommandOutput run = std::move(output).value();

    if (run.timedOut) {
        return makeError(ErrorKind::Timeout, "query timed out");
    }
    if (run.exitCode != 0) {
        std::string detail = cleanTraceProcessorError(run.err);
        if (detail.empty()) detail = cleanTraceProcessorError(run.out);
        if (detail.empty()) detail = "trace_processor_shell failed";
        return makeError(ErrorKind::ProcessFailed, detail, run.exitCode);
    }

    auto parsed = parseQueryOutput(run.out);
    if (!parsed) return std::move(parsed).error();

    QueryResult result = std::move(parsed).value();
    result.elapsedMs = elapsedMs;
    result.statement = sql;
    return result;
}

Result<QueryResult> TraceProcessor::queryRpc(const std::string& sql,
                                             std::chrono::milliseconds timeout) {
    const HttpClient* client = nullptr;
    {
        const std::lock_guard lock{mutex_};
        client = http_.get();
    }
    if (client == nullptr) {
        return makeError(ErrorKind::Unsupported, "httpd transport is not running");
    }

    auto response = client->post("/query", sql, "text/plain",
                                 static_cast<int>(timeout.count()));
    if (!response) return std::move(response).error();
    const HttpResponse http = std::move(response).value();
    if (!http.ok()) {
        return makeError(ErrorKind::Protocol,
                         "httpd returned HTTP " + std::to_string(http.statusCode));
    }

    // trace_processor's /query answers with a protobuf-encoded QueryResult.
    // Decoding it is the remaining work for this transport; until then the
    // Batch transport is the supported path and this reports honestly rather
    // than handing back a half-parsed table.
    return makeError(ErrorKind::Unsupported,
                     "the Rpc transport needs a protobuf QueryResult decoder; "
                     "use the Batch transport (received " +
                         std::to_string(http.body.size()) + " bytes)");
}

std::vector<TraceProcessor::NamedQuery> TraceProcessor::standardQueries() {
    return {
        {"Trace bounds", "SELECT * FROM trace_bounds;",
         "Start and end timestamp of the trace, in nanoseconds."},

        {"Processes by name",
         "SELECT upid, pid, name FROM process WHERE name IS NOT NULL ORDER BY name;",
         "Every process the trace saw."},

        {"Slowest slices",
         "SELECT s.name, s.dur / 1e6 AS dur_ms, t.name AS track\n"
         "FROM slice s JOIN track t ON s.track_id = t.id\n"
         "ORDER BY s.dur DESC LIMIT 50;",
         "The 50 longest slices, which is usually where a hitch lives."},

        {"Self time by slice name",
         "SELECT name, COUNT(*) AS count, SUM(dur) / 1e6 AS total_ms,\n"
         "       AVG(dur) / 1e6 AS avg_ms\n"
         "FROM slice GROUP BY name ORDER BY total_ms DESC LIMIT 50;",
         "Aggregate cost per slice name: finds death by a thousand cuts."},

        {"Frame timeline (actual)",
         "SELECT * FROM actual_frame_timeline_slice ORDER BY dur DESC LIMIT 50;",
         "Needs the android.surfaceflinger.frametimeline data source."},

        {"Janky frames",
         "SELECT name, dur / 1e6 AS dur_ms, jank_type, on_time_finish\n"
         "FROM actual_frame_timeline_slice\n"
         "WHERE jank_type != 'None' ORDER BY dur DESC LIMIT 100;",
         "Perfetto's own jank classification, with the reason per frame."},

        {"CPU time per thread",
         "SELECT t.name AS thread, p.name AS process,\n"
         "       SUM(sched.dur) / 1e6 AS cpu_ms\n"
         "FROM sched JOIN thread t USING(utid) JOIN process p USING(upid)\n"
         "GROUP BY utid ORDER BY cpu_ms DESC LIMIT 50;",
         "Needs the sched_switch ftrace event."},

        {"Counter tracks",
         "SELECT id, name FROM counter_track ORDER BY name;",
         "Every counter available to chart, e.g. cpu frequency or memory."},
    };
}

}  // namespace em
