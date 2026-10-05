#include "core/HttpClient.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <span>
#include <utility>

#include "core/StringUtil.h"
#include "core/TcpSocket.h"

#if defined(_WIN32)
#  include <winsock2.h>  // WSAStartup
#endif

namespace em {
namespace {

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
    return TcpSocket::connect(host_, port_, timeoutMs).hasValue();
}

Result<HttpResponse> HttpClient::send(const HttpRequest& request) const {
    auto connected = TcpSocket::connect(host_, port_, std::min(request.timeoutMs, 2000));
    if (!connected) return std::move(connected).error();
    TcpSocket sock = std::move(connected).value();

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

    EM_TRY_VOID(sock.sendAll(head));
    if (!request.body.empty()) EM_TRY_VOID(sock.sendAll(request.body));

    std::string raw;
    std::array<char, 32 * 1024> buffer{};
    for (;;) {
        auto n = sock.recvSome(buffer, request.timeoutMs);
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
