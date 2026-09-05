#pragma once

// Worker API transport (Phase 15) — how the native agent reaches the control
// plane.
//
// The agent speaks ONLY this interface: a plain HTTP exchange (method, path,
// headers, JSON body) with the control plane. Two implementations exist:
//
//   * HttpWorkerTransport (http_transport.hpp) — a real HTTP/1.1 client over
//     the OS socket API (POSIX/Winsock). PLAIN HTTP ONLY: TLS is not
//     implemented in the C++ core (the dependency policy keeps the core
//     standard-library only), so a production deployment either terminates
//     TLS at a reverse proxy in front of the API or keeps the agent on a
//     trusted network segment — both documented in docs/worker/.
//   * test/scripted transports — in-process fakes used by the tests to pin
//     the agent's loop deterministically (no network, no timing flake).
//
// The transport never parses bodies: raw status + body pass through to the
// protocol codec (worker_protocol.hpp), so the wire layer stays dumb.

#include <string>

namespace vortyx::worker {

class IWorkerApiTransport {
public:
    virtual ~IWorkerApiTransport() = default;

    struct Response {
        bool ok = false;          // a well-formed HTTP exchange happened
        int status = 0;           // HTTP status code (0 when !ok)
        std::string body;         // the response body ("" when none)
        std::string error;        // transport-level failure reason (when !ok)
    };

    // POST with a JSON body. 'path' starts with '/'.
    virtual Response post(const std::string& path, const std::string& json_body) = 0;

    // GET without a body (health probes).
    virtual Response get(const std::string& path) = 0;
};

}  // namespace vortyx::worker
