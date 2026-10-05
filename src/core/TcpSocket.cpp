#include "core/TcpSocket.h"

#include <cerrno>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socklen_t = int;
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace em {
namespace {

std::string lastSocketError() {
#if defined(_WIN32)
    return "winsock error " + std::to_string(::WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

#if defined(_WIN32)
SOCKET native(std::uintptr_t h) { return static_cast<SOCKET>(h); }
#else
int native(int h) { return h; }
#endif

void setBlocking(auto sock, bool blocking) {
#if defined(_WIN32)
    unsigned long mode = blocking ? 0 : 1;
    ::ioctlsocket(sock, FIONBIO, &mode);
#else
    const int flags = ::fcntl(sock, F_GETFL, 0);
    ::fcntl(sock, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

// Waits for the socket to become readable (or writable). 1 ready, 0 timeout,
// -1 error.
int waitFor(auto sock, bool forWrite, int timeoutMs) {
#if defined(_WIN32)
    fd_set set;
    FD_ZERO(&set);
    FD_SET(sock, &set);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    return ::select(0, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &tv);
#else
    pollfd pfd{};
    pfd.fd = sock;
    pfd.events = forWrite ? POLLOUT : POLLIN;
    for (;;) {
        const int rc = ::poll(&pfd, 1, timeoutMs);
        if (rc < 0 && errno == EINTR) continue;
        return rc;
    }
#endif
}

}  // namespace

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(std::exchange(other.handle_, kInvalid)) {}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, kInvalid);
    }
    return *this;
}

bool TcpSocket::valid() const noexcept { return handle_ != kInvalid; }

void TcpSocket::shutdown() noexcept {
    if (!valid()) return;
#if defined(_WIN32)
    ::shutdown(native(handle_), SD_BOTH);
#else
    ::shutdown(native(handle_), SHUT_RDWR);
#endif
}

void TcpSocket::close() noexcept {
    if (!valid()) return;
#if defined(_WIN32)
    ::closesocket(native(handle_));
#else
    ::close(native(handle_));
#endif
    handle_ = kInvalid;
}

Result<TcpSocket> TcpSocket::connect(const std::string& host, std::uint16_t port, int timeoutMs) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* list = nullptr;
    const std::string portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &list) != 0 || list == nullptr) {
        return makeError(ErrorKind::NotFound, "cannot resolve " + host);
    }

    Error lastError = makeError(ErrorKind::Io, "no addresses tried");
    for (const addrinfo* ai = list; ai != nullptr; ai = ai->ai_next) {
        const auto raw = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
#if defined(_WIN32)
        if (raw == INVALID_SOCKET) {
#else
        if (raw < 0) {
#endif
            lastError = makeError(ErrorKind::Io, "socket(): " + lastSocketError());
            continue;
        }
        TcpSocket sock{static_cast<Handle>(raw)};

        const int one = 1;
        ::setsockopt(raw, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#if defined(SO_NOSIGPIPE)
        // macOS: a write to a closed peer must not raise SIGPIPE.
        ::setsockopt(raw, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

        setBlocking(raw, false);
        int rc = ::connect(raw, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
        if (rc != 0) {
#if defined(_WIN32)
            const bool inProgress = ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
            const bool inProgress = errno == EINPROGRESS;
#endif
            if (!inProgress) {
                lastError = makeError(ErrorKind::Io, "connect(): " + lastSocketError());
                continue;
            }
            rc = waitFor(raw, /*forWrite=*/true, timeoutMs);
            if (rc <= 0) {
                lastError = makeError(ErrorKind::Timeout,
                                      "connect to " + host + ":" + portStr + " timed out");
                continue;
            }
            int soError = 0;
            socklen_t len = sizeof(soError);
            ::getsockopt(raw, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
            if (soError != 0) {
                lastError = makeError(ErrorKind::Io,
                                      "connect failed (errno " + std::to_string(soError) + ")");
                continue;
            }
        }
        // Back to blocking; reads and writes are bounded by waitFor().
        setBlocking(raw, true);
        ::freeaddrinfo(list);
        return std::move(sock);
    }
    ::freeaddrinfo(list);
    return lastError;
}

Status TcpSocket::sendAll(std::string_view data) {
    if (!valid()) return makeError(ErrorKind::Io, "socket is closed");
#if defined(MSG_NOSIGNAL)
    constexpr int kFlags = MSG_NOSIGNAL;  // Linux: no SIGPIPE on a closed peer
#else
    constexpr int kFlags = 0;
#endif
    std::size_t sent = 0;
    while (sent < data.size()) {
#if defined(_WIN32)
        const auto n = ::send(native(handle_), data.data() + sent,
                              static_cast<int>(data.size() - sent), kFlags);
#else
        const auto n = ::send(native(handle_), data.data() + sent, data.size() - sent, kFlags);
#endif
        if (n <= 0) {
#if !defined(_WIN32)
            if (n < 0 && errno == EINTR) continue;
#endif
            return makeError(ErrorKind::Io, "send(): " + lastSocketError());
        }
        sent += static_cast<std::size_t>(n);
    }
    return {};
}

Result<std::size_t> TcpSocket::recvSome(std::span<char> dst, int timeoutMs) {
    if (!valid()) return makeError(ErrorKind::Io, "socket is closed");
    const int rc = waitFor(native(handle_), /*forWrite=*/false, timeoutMs);
    if (rc == 0) return makeError(ErrorKind::Timeout, "read timed out");
    if (rc < 0) return makeError(ErrorKind::Io, "poll(): " + lastSocketError());
#if defined(_WIN32)
    const auto n = ::recv(native(handle_), dst.data(), static_cast<int>(dst.size()), 0);
#else
    const auto n = ::recv(native(handle_), dst.data(), dst.size(), 0);
#endif
    if (n < 0) return makeError(ErrorKind::Io, "recv(): " + lastSocketError());
    return static_cast<std::size_t>(n);
}

Status TcpSocket::recvExact(std::span<char> dst, int timeoutMs) {
    std::size_t got = 0;
    while (got < dst.size()) {
        EM_TRY(n, recvSome(dst.subspan(got), timeoutMs));
        if (n == 0) return makeError(ErrorKind::Io, "connection closed");
        got += n;
    }
    return {};
}

}  // namespace em
