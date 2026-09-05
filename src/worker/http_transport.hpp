#pragma once

// The real HTTP transport (Phase 15) — a minimal HTTP/1.1 client over the
// OS socket API (POSIX sockets / Winsock, platform-split like the device
// discovery backends).
//
// DELIBERATE SCOPE (documented, honest):
//   * http:// only. TLS is NOT implemented in the C++ core (the dependency
//     policy). A deployment that needs HTTPS terminates TLS at a reverse
//     proxy in front of the API, or runs the agent inside a trusted
//     network segment (docs/worker/). An https:// endpoint is REFUSED at
//     configuration time — never silently downgraded.
//   * HTTP/1.1 with Content-Length or chunked responses; no persistent
//     connections (one exchange per request — the worker's call rate is
//     poll-interval bound, connection reuse buys nothing).
//   * Bearer-token authorization on every request (the worker token).
//   * A hard response-size cap (a hostile endpoint cannot exhaust memory).
//   * A connect/recv timeout (a hung control plane cannot hang the agent).

#include <cstdint>
#include <memory>
#include <string>

#include "platform/status.hpp"
#include "worker/worker_transport.hpp"

namespace vortyx::worker {

struct HttpTransportConfig {
    std::string endpoint;      // "http://host:port[/prefix]" (https refused)
    std::string bearer_token;  // the worker token ("" = no auth header)
    std::int32_t timeout_ms = 10000;       // connect + read budget
    std::int64_t max_response_bytes = 4LL * 1024 * 1024;
};

class HttpWorkerTransport final : public IWorkerApiTransport {
public:
    // Parses/validates the endpoint up front (a bad endpoint is a
    // configuration refusal, never a per-request surprise). Errors:
    // InvalidInput with 'error'.
    static vortyx::platform::Status create(const HttpTransportConfig& config,
                                           std::unique_ptr<IWorkerApiTransport>& out,
                                           std::string& error);

    ~HttpWorkerTransport() override;

    Response post(const std::string& path, const std::string& json_body) override;
    Response get(const std::string& path) override;

private:
    HttpWorkerTransport() = default;

    Response exchange(const std::string& method, const std::string& path,
                      const std::string& body);

    HttpTransportConfig config_;
    std::string host_;
    std::string port_;
    std::string prefix_;  // endpoint path prefix (often "")
};

}  // namespace vortyx::worker
