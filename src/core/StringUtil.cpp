#include "core/StringUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace em {
namespace {

constexpr std::string_view kSpace = " \t\r\n\v\f";

bool isSpaceChar(char c) noexcept {
    return kSpace.find(c) != std::string_view::npos;
}

char lowerChar(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

}  // namespace

std::string_view trimLeft(std::string_view s) noexcept {
    const auto pos = s.find_first_not_of(kSpace);
    return pos == std::string_view::npos ? std::string_view{} : s.substr(pos);
}

std::string_view trimRight(std::string_view s) noexcept {
    const auto pos = s.find_last_not_of(kSpace);
    return pos == std::string_view::npos ? std::string_view{} : s.substr(0, pos + 1);
}

std::string_view trim(std::string_view s) noexcept { return trimRight(trimLeft(s)); }

std::vector<std::string_view> split(std::string_view s, char delim, bool keepEmpty) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto pos = s.find(delim, start);
        const auto end = (pos == std::string_view::npos) ? s.size() : pos;
        if (keepEmpty || end > start) out.push_back(s.substr(start, end - start));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

std::vector<std::string_view> tokenize(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isSpaceChar(s[i])) ++i;
        const std::size_t start = i;
        while (i < s.size() && !isSpaceChar(s[i])) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::vector<std::string_view> splitLines(std::string_view s) {
    std::vector<std::string_view> out = split(s, '\n', true);
    for (auto& line : out) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    }
    // A trailing newline produces a final empty element; drop it.
    if (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

std::string toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) out.push_back(lowerChar(c));
    return out;
}

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b) { return lowerChar(a) == lowerChar(b); });
    return it != haystack.end();
}

std::optional<std::string_view> stripPrefix(std::string_view line,
                                            std::string_view prefix) noexcept {
    if (!line.starts_with(prefix)) return std::nullopt;
    return line.substr(prefix.size());
}

std::optional<std::string_view> fieldAfter(std::string_view line,
                                           std::string_view key) noexcept {
    const auto pos = line.find(key);
    if (pos == std::string_view::npos) return std::nullopt;
    std::string_view rest = line.substr(pos + key.size());
    rest = trimLeft(rest);
    if (!rest.empty() && (rest.front() == ':' || rest.front() == '=')) {
        rest.remove_prefix(1);
    }
    return trim(rest);
}

std::string format(const char* fmt, ...) {
    std::array<char, 512> stackBuf{};

    std::va_list args;
    va_start(args, fmt);
    std::va_list argsCopy;
    va_copy(argsCopy, args);
    const int needed = std::vsnprintf(stackBuf.data(), stackBuf.size(), fmt, args);
    va_end(args);

    if (needed < 0) {
        va_end(argsCopy);
        return {};
    }
    const auto neededSize = static_cast<std::size_t>(needed);
    if (neededSize < stackBuf.size()) {
        va_end(argsCopy);
        return std::string{stackBuf.data(), neededSize};
    }

    std::string heap(neededSize + 1, '\0');
    std::vsnprintf(heap.data(), heap.size(), fmt, argsCopy);
    va_end(argsCopy);
    heap.resize(neededSize);
    return heap;
}

std::string humanBytes(double bytes) {
    static constexpr std::array<const char*, 6> kUnits{"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = bytes;
    std::size_t unit = 0;
    const bool negative = value < 0.0;
    value = std::abs(value);
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }
    // Literal format strings so -Wformat can still check these calls.
    if (unit == 0 || value >= 100.0) {
        return format("%s%.0f %s", negative ? "-" : "", value, kUnits[unit]);
    }
    return format("%s%.1f %s", negative ? "-" : "", value, kUnits[unit]);
}

std::string humanDuration(double seconds) {
    if (seconds < 1.0) return format("%.0f ms", seconds * 1000.0);
    if (seconds < 60.0) return format("%.1f s", seconds);
    const auto total = static_cast<long long>(seconds);
    const long long h = total / 3600;
    const long long m = (total % 3600) / 60;
    const long long s = total % 60;
    if (h > 0) return format("%lldh %02lldm %02llds", h, m, s);
    return format("%lldm %02llds", m, s);
}

std::string quoteArgv(const std::vector<std::string>& argv) {
    std::string out;
    for (const auto& arg : argv) {
        if (!out.empty()) out.push_back(' ');
        const bool needsQuote =
            arg.empty() || arg.find_first_of(" \t\"'") != std::string::npos;
        if (needsQuote) {
            out.push_back('"');
            out += arg;
            out.push_back('"');
        } else {
            out += arg;
        }
    }
    return out;
}

std::string shellQuote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (const char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

}  // namespace em
