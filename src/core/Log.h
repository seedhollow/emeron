#pragma once

// Thread-safe ring-buffered logger. The GUI reads the ring each frame to render
// the Log panel, so the sink must never block a worker thread for long.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace em {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error };

struct LogEntry {
    std::chrono::system_clock::time_point when;
    LogLevel level;
    std::string category;
    std::string message;
};

class Logger {
public:
    static Logger& instance();

    void setMinLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel minLevel() const noexcept;

    void push(LogLevel level, std::string_view category, std::string message);

    // Copies out the current contents; cheap enough at the 4k cap we use and it
    // keeps the lock away from ImGui's draw loop.
    [[nodiscard]] std::vector<LogEntry> snapshot() const;
    void clear();

    [[nodiscard]] std::uint64_t revision() const noexcept;

private:
    Logger() = default;

    static constexpr std::size_t kCapacity = 4096;

    mutable std::mutex mutex_;
    std::deque<LogEntry> entries_;
    LogLevel minLevel_ = LogLevel::Info;
    std::uint64_t revision_ = 0;
};

[[nodiscard]] std::string_view toString(LogLevel level) noexcept;

void logMessage(LogLevel level, std::string_view category, std::string message);

// Formatting helpers kept printf-free: callers build the string with
// std::string concatenation or em::format() from StringUtil.h.
#define EM_LOG_TRACE(cat, msg) ::em::logMessage(::em::LogLevel::Trace, (cat), (msg))
#define EM_LOG_DEBUG(cat, msg) ::em::logMessage(::em::LogLevel::Debug, (cat), (msg))
#define EM_LOG_INFO(cat, msg)  ::em::logMessage(::em::LogLevel::Info,  (cat), (msg))
#define EM_LOG_WARN(cat, msg)  ::em::logMessage(::em::LogLevel::Warn,  (cat), (msg))
#define EM_LOG_ERROR(cat, msg) ::em::logMessage(::em::LogLevel::Error, (cat), (msg))

}  // namespace em
