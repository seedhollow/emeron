#include "core/HttpClient.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <span>
#include <utility>

#include "core/StringUtil.h"

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
using SocketHandle = SOCKET;
using socklen_t = int;
#  define EM_INVALID_SOCKET INVALID_SOCKET
#  define EM_CLOSE_SOCKET closesocket
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
using SocketHandle = int;
#  define EM_INVALID_SOCKET (-1)
#  define EM_CLOSE_SOCKET ::close
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

// RAII socket so every early return closes the descriptor.
class Socket {
public:
    Socket() = default;
    explicit Socket(SocketHandle h) : handle_(h) {}
    ~Socket() { reset(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : handle_(other.release()) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] SocketHandle get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != EM_INVALID_SOCKET; }

    SocketHandle release() noexcept {
        SocketHandle h = handle_;
        handle_ = EM_INVALID_SOCKET;
        return h;
    }
    void reset() noexcept {
        if (valid()) EM_CLOSE_SOCKET(handle_);
        handle_ = EM_INVALID_SOCKET;
    }

private:
    SocketHandle handle_ = EM_INVALID_SOCKET;
};

Result<Socket> connectTo(const std::string& host, std::uint16_t port, int timeoutMs) {
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
        Socket sock{::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)};
        if (!sock.valid()) {
            lastError = makeError(ErrorKind::Io, "socket(): " + lastSocketError());
            continue;
        }

        const int one = 1;
        ::setsockopt(sock.get(), IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&one), sizeof(one));

#if defined(_WIN32)
        unsigned long nonblock = 1;
        ::ioctlsocket(sock.get(), FIONBIO, &nonblock);
#else
        const int flags = ::fcntl(sock.get(), F_GETFL, 0);
        ::fcntl(sock.get(), F_SETFL, flags | O_NONBLOCK);
#endif

        int rc = ::connect(sock.get(), ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
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

#if defined(_WIN32)
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(sock.get(), &writeSet);
            timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            rc = ::select(0, nullptr, &writeSet, nullptr, &tv);
#else
            pollfd pfd{};
            pfd.fd = sock.get();
            pfd.events = POLLOUT;
            rc = ::poll(&pfd, 1, timeoutMs);
#endif
            if (rc <= 0) {
                lastError = makeError(ErrorKind::Timeout,
                                      "connect to " + host + ":" + portStr + " timed out");
                continue;
            }

            int soError = 0;
            socklen_t len = sizeof(soError);
            ::getsockopt(sock.get(), SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&soError), &len);
            if (soError != 0) {
                lastError = makeError(ErrorKind::Io, "connect failed (errno " +
                                                         std::to_string(soError) + ")");
                continue;
            }
        }

        // Back to blocking; reads and writes are bounded by poll() below.
#if defined(_WIN32)
        unsigned long blocking = 0;
        ::ioctlsocket(sock.get(), FIONBIO, &blocking);
#else
        const int f2 = ::fcntl(sock.get(), F_GETFL, 0);
        ::fcntl(sock.get(), F_SETFL, f2 & ~O_NONBLOCK);
#endif
        ::freeaddrinfo(list);
        return std::move(sock);
    }

    ::freeaddrinfo(list);
    return lastError;
}

Status sendAll(SocketHandle sock, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = ::send(sock, data.data() + sent,
#if defined(_WIN32)
                              static_cast<int>(data.size() - sent),
#else
                              data.size() - sent,
#endif
                              0);
        if (n <= 0) {
#if !defined(_WIN32)
            if (errno == EINTR) continue;
#endif
            return makeError(ErrorKind::Io, "send(): " + lastSocketError());
        }
        sent += static_cast<std::size_t>(n);
    }
    return {};
}

Result<std::size_t> recvSome(SocketHandle sock, std::span<char> dst, int timeoutMs) {
#if defined(_WIN32)
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    const int rc = ::select(0, &readSet, nullptr, nullptr, &tv);
#else
    pollfd pfd{};
    pfd.fd = sock;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, timeoutMs);
#endif
    if (rc == 0) return makeError(ErrorKind::Timeout, "read timed out");
    if (rc < 0) return makeError(ErrorKind::Io, "poll(): " + lastSocketError());

    const auto n = ::recv(sock, dst.data(),
#if defined(_WIN32)
                          static_cast<int>(dst.size()),
#else
                          dst.size(),
#endif
                          0);
    if (n < 0) return makeError(ErrorKind::Io, "recv(): " + lastSocketError());
    return static_cast<std::size_t>(n);
}

// Parses "HTTP/1.1 200 OK" + headers. Returns the body offset.
Result<std::size_t> parseHead(std::string_view raw, HttpResponse& out) {
    const auto headEnd = raw.find("\r\n\r\n");
    if (headEnd == std::string_view::npos) {
        return makeError(ErrorKind::Protocol, "incomplete HTTP head");
    }

    const std::string_view head = raw.substr(0, headEnd);
    const auto lines = splitLines(head);
    if (lines.empty()) return makeError(ErrorKind::Protocol, "empty HTTP head");

    const auto statusParts = tokenize(lines.front());
    if (statusParts.size() < 2) return makeError(ErrorKind::Protocol, "bad status line");
    const auto code = parseNumber<int>(statusParts[1]);
    if (!code) return makeError(ErrorKind::Protocol, "bad status code");
    out.statusCode = *code;

    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto colon = lines[i].find(':');
        if (colon == std::string_view::npos) continue;
        out.headers.emplace(toLower(trim(lines[i].substr(0, colon))),
                            std::string{trim(lines[i].substr(colon + 1))});
    }
    return headEnd + 4;
}

std::string decodeChunked(std::string_view body) {
    std::string out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        const auto lineEnd = body.find("\r\n", pos);
        if (lineEnd == std::string_view::npos) break;
        const std::string_view sizeLine = body.substr(pos, lineEnd - pos);

        std::size_t chunkSize = 0;
        bool parsed = false;
        for (const char c : sizeLine) {
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else break;
            chunkSize = chunkSize * 16 + static_cast<std::size_t>(digit);
            parsed = true;
        }
        if (!parsed || chunkSize == 0) break;

        const std::size_t dataStart = lineEnd + 2;
        if (dataStart + chunkSize > body.size()) break;
        out.append(body.substr(dataStart, chunkSize));
        pos = dataStart + chunkSize + 2;  // skip trailing CRLF
    }
    return out;
}

}  // namespace

std::string_view HttpResponse::header(std::string_view name) const {
    const auto it = headers.find(toLower(name));
    return it == headers.end() ? std::string_view{} : std::string_view{it->second};
}

void initSocketSubsystem() {
#if defined(_WIN32)
    static bool started = false;
    if (!started) {
        WSADATA data{};
        ::WSAStartup(MAKEWORD(2, 2), &data);
        started = true;
    }
#endif
}

HttpClient::HttpClient(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port) {}

bool HttpClient::canConnect(int timeoutMs) const {
    return connectTo(host_, port_, timeoutMs).hasValue();
}

Result<HttpResponse> HttpClient::send(const HttpRequest& request) const {
    auto connected = connectTo(host_, port_, std::min(request.timeoutMs, 2000));
    if (!connected) return std::move(connected).error();
    const Socket sock = std::move(connected).value();

    std::string head;
    head.reserve(256 + request.body.size());
    head += request.method;
    head += ' ';
    head += request.path;
    head += " HTTP/1.1\r\nHost: ";
    head += host_;
    head += ':';
    head += std::to_string(port_);
    head += "\r\nConnection: close\r\nUser-Agent: emeron/0.1\r\n";
    if (!request.contentType.empty()) {
        head += "Content-Type: " + request.contentType + "\r\n";
    }
    head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n\r\n";

    EM_TRY_VOID(sendAll(sock.get(), head));
    if (!request.body.empty()) EM_TRY_VOID(sendAll(sock.get(), request.body));

    std::string raw;
    std::array<char, 32 * 1024> buffer{};
    for (;;) {
        auto n = recvSome(sock.get(), buffer, request.timeoutMs);
        if (!n) {
            if (n.error().kind == ErrorKind::Timeout && !raw.empty()) break;
            return std::move(n).error();
        }
        if (*n == 0) break;  // peer closed
        raw.append(buffer.data(), *n);
    }

    HttpResponse response;
    EM_TRY(bodyStart, parseHead(raw, response));
    response.body = raw.substr(bodyStart);

    if (response.header("transfer-encoding") == "chunked") {
        response.body = decodeChunked(response.body);
    }
    return response;
}

Result<HttpResponse> HttpClient::get(std::string_view path, int timeoutMs) const {
    HttpRequest request;
    request.method = "GET";
    request.path = std::string{path};
    request.timeoutMs = timeoutMs;
    return send(request);
}

Result<HttpResponse> HttpClient::post(std::string_view path, std::string body,
                                      std::string contentType, int timeoutMs) const {
    HttpRequest request;
    request.method = "POST";
    request.path = std::string{path};
    request.body = std::move(body);
    request.contentType = std::move(contentType);
    request.timeoutMs = timeoutMs;
    return send(request);
}

}  // namespace em
