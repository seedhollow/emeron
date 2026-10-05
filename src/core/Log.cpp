#include "core/Log.h"

#include <cstdio>

namespace em {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setMinLevel(LogLevel level) noexcept {
    const std::lock_guard lock{mutex_};
    minLevel_ = level;
}

LogLevel Logger::minLevel() const noexcept {
    const std::lock_guard lock{mutex_};
    return minLevel_;
}

void Logger::push(LogLevel level, std::string_view category, std::string message) {
    const std::lock_guard lock{mutex_};
    if (level < minLevel_) return;

    if (entries_.size() >= kCapacity) entries_.pop_front();
    entries_.push_back(LogEntry{std::chrono::system_clock::now(), level,
                                std::string{category}, std::move(message)});
    ++revision_;

    // Mirror warnings and errors to stderr so headless runs stay diagnosable.
    if (level >= LogLevel::Warn) {
        const LogEntry& e = entries_.back();
        std::fprintf(stderr, "[%.*s] %.*s: %s\n",
                     static_cast<int>(toString(level).size()), toString(level).data(),
                     static_cast<int>(e.category.size()), e.category.data(),
                     e.message.c_str());
    }
}

std::vector<LogEntry> Logger::snapshot() const {
    const std::lock_guard lock{mutex_};
    return std::vector<LogEntry>{entries_.begin(), entries_.end()};
}

void Logger::clear() {
    const std::lock_guard lock{mutex_};
    entries_.clear();
    ++revision_;
}

std::uint64_t Logger::revision() const noexcept {
    const std::lock_guard lock{mutex_};
    return revision_;
}

std::string_view toString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

void logMessage(LogLevel level, std::string_view category, std::string message) {
    Logger::instance().push(level, category, std::move(message));
}

}  // namespace em
