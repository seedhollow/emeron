#pragma once

// Minimal blocking HTTP/1.1 client, loopback only.
//
// Its sole consumer is the trace_processor_shell `--httpd` RPC endpoint, so it
// supports exactly what that needs: GET and POST with a body, no TLS, no
// redirects, no chunked request encoding (chunked *responses* are decoded).
// Keep it that way -- if emeron ever needs real HTTP, link a real library.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/Result.h"

namespace em {

struct HttpResponse {
    int statusCode = 0;
    std::unordered_map<std::string, std::string> headers;  // keys lowercased
    std::string body;

    [[nodiscard]] bool ok() const noexcept { return statusCode >= 200 && statusCode < 300; }
    [[nodiscard]] std::string_view header(std::string_view name) const;
};

struct HttpRequest {
    std::string method = "GET";
    std::string path = "/";
    std::string contentType;
    std::string body;
    int timeoutMs = 10'000;
};

class HttpClient {
public:
    HttpClient(std::string host, std::uint16_t port);

    [[nodiscard]] Result<HttpResponse> send(const HttpRequest& request) const;

    [[nodiscard]] Result<HttpResponse> get(std::string_view path, int timeoutMs = 10'000) const;
    [[nodiscard]] Result<HttpResponse> post(std::string_view path, std::string body,
                                            std::string contentType,
                                            int timeoutMs = 60'000) const;

    // Cheap liveness probe: can we complete a TCP connect?
    [[nodiscard]] bool canConnect(int timeoutMs = 300) const;

    [[nodiscard]] const std::string& host() const noexcept { return host_; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    std::string host_;
    std::uint16_t port_;
};

// Must be called once before the first HttpClient use (WSAStartup on Windows).
void initSocketSubsystem();

}  // namespace em
