#pragma once

#include <charconv>
#include <concepts>
#include <cstdarg>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace em {

template <typename T>
concept Numeric = std::integral<T> || std::floating_point<T>;

template <typename T>
concept StringViewable = std::convertible_to<T, std::string_view>;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;
[[nodiscard]] std::string_view trimLeft(std::string_view s) noexcept;
[[nodiscard]] std::string_view trimRight(std::string_view s) noexcept;

// Splits on a single delimiter. `keepEmpty == false` collapses runs of the
// delimiter, which is what nearly every `adb shell` output needs.
[[nodiscard]] std::vector<std::string_view> split(std::string_view s, char delim,
                                                  bool keepEmpty = true);

// Splits on any run of spaces/tabs. The workhorse for /proc and dumpsys output.
[[nodiscard]] std::vector<std::string_view> tokenize(std::string_view s);

// Splits into lines, tolerating CRLF from Windows adb builds.
[[nodiscard]] std::vector<std::string_view> splitLines(std::string_view s);

[[nodiscard]] bool containsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept;
[[nodiscard]] std::string toLower(std::string_view s);

// Returns the text after `prefix` if `line` starts with it, else nullopt.
[[nodiscard]] std::optional<std::string_view> stripPrefix(std::string_view line,
                                                          std::string_view prefix) noexcept;

// Extracts the value from a `key: value` or `key=value` style line.
[[nodiscard]] std::optional<std::string_view> fieldAfter(std::string_view line,
                                                         std::string_view key) noexcept;

// Tolerant numeric parse: ignores surrounding whitespace and trailing junk
// (units such as "kB" or "ms"), which dumpsys sprinkles everywhere.
template <Numeric T>
[[nodiscard]] std::optional<T> parseNumber(std::string_view text) {
    const std::string_view s = trim(text);
    if (s.empty()) return std::nullopt;

    T out{};
    if constexpr (std::floating_point<T>) {
        // from_chars for floating point is missing in libstdc++ < 11 and in
        // some Apple libc++ versions, so go through strtod on a NUL-terminated
        // copy. These strings are short; the copy is not a hot path.
        const std::string buf{s};
        char* end = nullptr;
        const double v = std::strtod(buf.c_str(), &end);
        if (end == buf.c_str()) return std::nullopt;
        out = static_cast<T>(v);
        return out;
    } else {
        const char* first = s.data();
        const char* last = s.data() + s.size();
        if (!s.empty() && s.front() == '+') ++first;
        const auto [ptr, ec] = std::from_chars(first, last, out);
        if (ec != std::errc{} || ptr == first) return std::nullopt;
        return out;
    }
}

[[nodiscard]] std::string format(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

[[nodiscard]] std::string humanBytes(double bytes);
[[nodiscard]] std::string humanDuration(double seconds);

// Joins argv for log output, quoting anything with whitespace.
[[nodiscard]] std::string quoteArgv(const std::vector<std::string>& argv);

// Wraps a string so it survives a trip through `adb shell` / `sh -c`.
[[nodiscard]] std::string shellQuote(std::string_view s);

}  // namespace em
