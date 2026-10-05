#pragma once

// A connected TCP socket with bounded waits. Shared by the HTTP client (the
// trace_processor RPC) and the scrcpy mirror (video and control streams
// through an adb tunnel).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "core/Result.h"

namespace em {

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    // TCP_NODELAY is set: both users send small, latency-sensitive messages.
    [[nodiscard]] static Result<TcpSocket> connect(const std::string& host, std::uint16_t port,
                                                   int timeoutMs);

    [[nodiscard]] bool valid() const noexcept;

    // Writes everything. A peer that has gone away is an error, never a
    // SIGPIPE that would end the process.
    [[nodiscard]] Status sendAll(std::string_view data);

    // Waits up to `timeoutMs` for data, then reads what is there. 0 is EOF.
    // A timeout is an ErrorKind::Timeout error.
    [[nodiscard]] Result<std::size_t> recvSome(std::span<char> dst, int timeoutMs);

    // Reads exactly dst.size() bytes, waiting up to `timeoutMs` for each chunk.
    [[nodiscard]] Status recvExact(std::span<char> dst, int timeoutMs);

    // Unblocks a reader on another thread; close() then releases the handle.
    void shutdown() noexcept;
    void close() noexcept;

private:
#if defined(_WIN32)
    using Handle = std::uintptr_t;  // SOCKET
    static constexpr Handle kInvalid = ~Handle{0};
#else
    using Handle = int;
    static constexpr Handle kInvalid = -1;
#endif
    explicit TcpSocket(Handle h) : handle_(h) {}
    Handle handle_ = kInvalid;
};

}  // namespace em
