#pragma once

// SQL query front-end for captured Perfetto traces, via Perfetto's own
// `trace_processor_shell`.
//
// TWO TRANSPORTS
// --------------
// Batch (implemented, the default): writes the SQL to a temp file and runs
//   trace_processor_shell -q <file> <trace>
// capturing stdout. Robust, no daemon to babysit, one process per query.
//
// Rpc (scaffolded): `trace_processor_shell --httpd` keeps the trace parsed in
// memory and answers POSTs on 127.0.0.1:9001, which is far faster for
// interactive querying. Its /query response body is a protobuf-encoded
// QueryResult; decoding that needs either Perfetto's generated bindings or a
// hand-rolled wire decoder, which is the one piece left to do here. Daemon
// lifecycle, health checking and the HTTP plumbing are all in place.
//
// The binary is looked for at: an explicit path, $EMERON_TRACE_PROCESSOR,
// PATH, then the usual SDK/homebrew locations. If it is missing the panel says
// so and links to Perfetto's download page rather than failing silently.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/HttpClient.h"
#include "core/Result.h"
#include "core/Subprocess.h"
#include "core/TaskQueue.h"

namespace em {

struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    double elapsedMs = 0.0;
    std::string statement;

    [[nodiscard]] bool empty() const noexcept { return rows.empty(); }
    [[nodiscard]] std::size_t columnCount() const noexcept { return columns.size(); }
};

enum class TraceProcessorTransport : std::uint8_t { Batch, Rpc };

struct TraceProcessorState {
    bool binaryFound = false;
    std::string binaryPath;
    std::string binaryOrigin;  // how it was found, shown in the Explore tab
    std::filesystem::path tracePath;
    bool traceLoaded = false;
    bool httpdRunning = false;
    std::uint16_t httpdPort = 9001;
    std::string lastError;
    std::string version;
};

class TraceProcessor {
public:
    explicit TraceProcessor(std::optional<std::filesystem::path> explicitBinary = std::nullopt);
    ~TraceProcessor();

    TraceProcessor(const TraceProcessor&) = delete;
    TraceProcessor& operator=(const TraceProcessor&) = delete;
    TraceProcessor(TraceProcessor&&) = delete;
    TraceProcessor& operator=(TraceProcessor&&) = delete;

    void relocate(std::optional<std::filesystem::path> explicitBinary = std::nullopt);
    [[nodiscard]] bool available() const;
    [[nodiscard]] TraceProcessorState state() const { return state_.get(); }

    // Points at a trace file. Does not parse anything in Batch mode; in Rpc
    // mode it restarts the daemon with the new trace.
    [[nodiscard]] Status openTrace(std::filesystem::path tracePath);
    void closeTrace();

    void setTransport(TraceProcessorTransport transport);
    [[nodiscard]] TraceProcessorTransport transport() const noexcept {
        return transport_.load(std::memory_order_acquire);
    }

    // Blocking. Call from a worker thread.
    [[nodiscard]] Result<QueryResult> query(std::string sql,
                                            std::chrono::milliseconds timeout =
                                                std::chrono::milliseconds{120'000});

    // --- Rpc transport lifecycle -------------------------------------------
    [[nodiscard]] Status startHttpd(std::uint16_t port = 9001);
    void stopHttpd();
    [[nodiscard]] bool httpdAlive() const;

    // Canned queries the panel offers as a starting point.
    struct NamedQuery {
        const char* title;
        const char* sql;
        const char* note;
    };
    [[nodiscard]] static std::vector<NamedQuery> standardQueries();

private:
    [[nodiscard]] Result<QueryResult> queryBatch(const std::string& sql,
                                                 std::chrono::milliseconds timeout);
    [[nodiscard]] Result<QueryResult> queryRpc(const std::string& sql,
                                               std::chrono::milliseconds timeout);

    [[nodiscard]] static std::vector<std::filesystem::path> searchCandidates();

    mutable std::mutex mutex_;
    std::optional<std::filesystem::path> binary_;
    std::filesystem::path tracePath_;
    Shared<TraceProcessorState> state_;
    std::atomic<TraceProcessorTransport> transport_{TraceProcessorTransport::Batch};

    std::unique_ptr<Subprocess> httpd_;
    std::unique_ptr<HttpClient> http_;
    std::uint16_t httpdPort_ = 9001;
};

// Parses trace_processor_shell's tabular stdout. Tolerant by design: the shell
// has used comma-separated, tab-separated and box-drawn layouts across
// versions, so all three are accepted.
[[nodiscard]] Result<QueryResult> parseQueryOutput(std::string_view text);

// Reduces trace_processor_shell's stderr for a failed query to the error and
// its position, e.g. "line 1 col 15: no such table: foo". Empty input -> "".
[[nodiscard]] std::string cleanTraceProcessorError(std::string_view text);

}  // namespace em
