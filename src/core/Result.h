#pragma once

// A small, allocation-light error-or-value type.
//
// We deliberately do not use std::expected: it needs a C++23 standard library
// and emeron targets Apple Clang / GCC 11 / MSVC 19.3x with C++20. The API here
// is intentionally a subset of std::expected so switching later is mechanical.
//
// Error-handling policy for the whole project: every operation that touches the
// outside world (processes, sockets, the filesystem, a device) returns Result.
// Exceptions are reserved for programmer errors and are never used for control
// flow across module boundaries.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace em {

enum class ErrorKind : std::uint8_t {
    Unknown,
    NotFound,        // binary, file or device missing
    PermissionDenied,
    Timeout,
    ProcessFailed,   // non-zero exit status
    ParseFailure,
    Io,
    Protocol,        // malformed response from adb / trace_processor
    Cancelled,
    Unsupported,
};

struct Error {
    ErrorKind kind = ErrorKind::Unknown;
    std::string message;
    int exitCode = 0;

    Error() = default;
    Error(ErrorKind k, std::string msg, int code = 0)
        : kind(k), message(std::move(msg)), exitCode(code) {}

    [[nodiscard]] std::string_view kindName() const noexcept {
        switch (kind) {
            case ErrorKind::NotFound:         return "not-found";
            case ErrorKind::PermissionDenied: return "permission-denied";
            case ErrorKind::Timeout:          return "timeout";
            case ErrorKind::ProcessFailed:    return "process-failed";
            case ErrorKind::ParseFailure:     return "parse-failure";
            case ErrorKind::Io:               return "io";
            case ErrorKind::Protocol:         return "protocol";
            case ErrorKind::Cancelled:        return "cancelled";
            case ErrorKind::Unsupported:      return "unsupported";
            case ErrorKind::Unknown:          break;
        }
        return "unknown";
    }
};

[[nodiscard]] inline Error makeError(ErrorKind kind, std::string message, int exitCode = 0) {
    return Error{kind, std::move(message), exitCode};
}

namespace detail {
struct VoidValue {};
}  // namespace detail

template <typename T>
class Result;

template <typename T>
concept ResultLike = requires(const T& r) {
    { r.hasValue() } -> std::convertible_to<bool>;
    { r.error() } -> std::convertible_to<const Error&>;
};

template <typename T>
class [[nodiscard]] Result {
    using Stored = std::conditional_t<std::is_void_v<T>, detail::VoidValue, T>;

public:
    using value_type = T;

    Result()
        requires std::is_void_v<T>
        : slot_(Stored{}) {}

    Result(Stored value)  // NOLINT(google-explicit-constructor) -- implicit by design
        requires(!std::is_void_v<T>)
        : slot_(std::move(value)) {}

    Result(Error err) : slot_(std::move(err)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool hasValue() const noexcept { return slot_.index() == 0; }
    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] const Error& error() const& noexcept { return std::get<1>(slot_); }
    [[nodiscard]] Error&& error() && noexcept { return std::get<1>(std::move(slot_)); }

    [[nodiscard]] decltype(auto) value() & requires(!std::is_void_v<T>) {
        return std::get<0>(slot_);
    }
    [[nodiscard]] decltype(auto) value() const& requires(!std::is_void_v<T>) {
        return std::get<0>(slot_);
    }
    // decltype(auto), not T&&: a member declaration is instantiated with the
    // class even when its requires-clause excludes it, and `void&&` is
    // ill-formed. Deduction defers the return type until the call.
    [[nodiscard]] decltype(auto) value() && requires(!std::is_void_v<T>) {
        return std::get<0>(std::move(slot_));
    }

    [[nodiscard]] decltype(auto) operator*() & requires(!std::is_void_v<T>) { return value(); }
    [[nodiscard]] decltype(auto) operator*() const& requires(!std::is_void_v<T>) { return value(); }
    [[nodiscard]] auto* operator->() requires(!std::is_void_v<T>) { return &value(); }
    [[nodiscard]] const auto* operator->() const requires(!std::is_void_v<T>) { return &value(); }

    template <typename U>
    [[nodiscard]] T valueOr(U&& fallback) const& requires(!std::is_void_v<T>) {
        return hasValue() ? value() : static_cast<T>(std::forward<U>(fallback));
    }

    // Convenience for propagating an error while changing the value type.
    template <typename U>
    [[nodiscard]] Result<U> propagate() const {
        return Result<U>{error()};
    }

    [[nodiscard]] std::string describe() const {
        if (hasValue()) return "ok";
        return std::string{error().kindName()} + ": " + error().message;
    }

private:
    std::variant<Stored, Error> slot_;
};

using Status = Result<void>;

// `EM_TRY(expr)` evaluates a Result-returning expression and returns early on
// failure. Uses a statement-expression-free form so it works on MSVC too.
#define EM_TRY(dest, expr)                     \
    auto _em_r_##dest = (expr);                \
    if (!_em_r_##dest) return std::move(_em_r_##dest).error(); \
    auto dest = std::move(_em_r_##dest).value()

#define EM_TRY_VOID(expr)                              \
    do {                                               \
        auto _em_status = (expr);                      \
        if (!_em_status) return std::move(_em_status).error(); \
    } while (false)

}  // namespace em
