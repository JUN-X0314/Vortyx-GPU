// The real HTTP transport (Phase 15) — implementation.
//
// Platform split (the device-discovery pattern): POSIX sockets and Winsock
// differ in API names and lifecycle; the exchange logic is shared.

#include "worker/http_transport.hpp"

#include <cstring>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>

#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace vortyx::worker {

namespace {

#if defined(_WIN32)

struct WsaLifetime {
    WsaLifetime() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WsaLifetime() { WSACleanup(); }
};

// One process-wide WSA lifecycle (idempotent, thread-safe).
const WsaLifetime& wsa() {
    static const WsaLifetime lifetime;
    return lifetime;
}

using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

SocketHandle open_stream() { return socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); }
void close_socket(SocketHandle handle) { closesocket(handle); }
std::string last_error() { return "winsock error " + std::to_string(WSAGetLastError()); }

#else  // POSIX

using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

SocketHandle open_stream() { return ::socket(AF_INET, SOCK_STREAM, 0); }
void close_socket(SocketHandle handle) { ::close(handle); }
std::string last_error() { return std::string(std::strerror(errno)); }

#endif

bool set_socket_timeout(SocketHandle handle, std::int32_t timeout_ms) {
#if defined(_WIN32)
    const DWORD ms = static_cast<DWORD>(timeout_ms);
    if (setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms),
                   sizeof(ms)) != 0) {
        return false;
    }
    return setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms),
                      sizeof(ms)) == 0;
#else
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return false;
    return setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

// Resolves 'host' (a name or dotted quad) to an IPv4 address string. Honest
// refusal (false) on failure — the agent reports a transport error instead
// of guessing.
bool resolve_host(const std::string& host, std::string& ipv4) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        if (result != nullptr) ::freeaddrinfo(result);
        return false;
    }
    char text[INET_ADDRSTRLEN] = {0};
    bool ok = false;
    for (const addrinfo* it = result; it != nullptr; it = it->ai_next) {
        const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
        if (::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) != nullptr) {
            ipv4 = text;
            ok = true;
            break;
        }
    }
    ::freeaddrinfo(result);
    return ok;
}

// Connect with a bounded budget (non-blocking connect + select), then
// restore blocking mode for the exchange.
bool connect_bounded(SocketHandle handle, const std::string& ipv4, std::uint16_t port,
                     std::int32_t timeout_ms) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, ipv4.c_str(), &address.sin_addr) != 1) return false;

#if defined(_WIN32)
    u_long non_blocking = 1;
    ioctlsocket(handle, FIONBIO, &non_blocking);
#else
    const int flags = ::fcntl(handle, F_GETFL, 0);
    ::fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif

    const int connect_result =
        ::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    bool connected = connect_result == 0;
    if (!connected) {
#if defined(_WIN32)
        const bool in_progress = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool in_progress = errno == EINPROGRESS;
#endif
        if (in_progress) {
            fd_set writes;
            FD_ZERO(&writes);
            FD_SET(handle, &writes);
            timeval tv{};
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            connected = ::select(static_cast<int>(handle) + 1, nullptr, &writes, nullptr,
                                 &tv) > 0;
            if (connected) {
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                ::getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error),
                             &length);
                connected = socket_error == 0;
            }
        }
    }

#if defined(_WIN32)
    u_long blocking = 0;
    ioctlsocket(handle, FIONBIO, &blocking);
#else
    ::fcntl(handle, F_SETFL, flags);
#endif
    return connected;
}

bool send_all(SocketHandle handle, const std::string& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int chunk = static_cast<int>(
            ::send(handle, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0));
        if (chunk <= 0) return false;
        sent += static_cast<std::size_t>(chunk);
    }
    return true;
}

std::string lowercase(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

// True when the response headers declare Transfer-Encoding: chunked (the
// framing servers use when the body length is not known up front).
bool transfer_encoding_chunked(const std::string& raw, std::size_t header_end) {
    if (header_end == std::string::npos) return false;
    std::string headers = lowercase(raw.substr(0, header_end));
    return headers.find("transfer-encoding:") != std::string::npos &&
           headers.find("chunked") != std::string::npos;
}

// Minimal RFC 7230 chunked decoding: hex size line, that many bytes, CRLF,
// repeat; terminal 0 chunk; trailers skipped. Returns "" on a malformed
// stream (the caller reports it — never guesses).
std::string decode_chunked(const std::string& input) {
    std::string output;
    std::size_t cursor = 0;
    while (cursor < input.size()) {
        const std::size_t line_end = input.find("\r\n", cursor);
        if (line_end == std::string::npos) return "";
        std::string size_text = input.substr(cursor, line_end - cursor);
        const std::size_t semicolon = size_text.find(';');  // chunk extensions
        if (semicolon != std::string::npos) size_text = size_text.substr(0, semicolon);
        if (size_text.empty()) return "";
        std::size_t size = 0;
        try {
            size = static_cast<std::size_t>(std::stoull(size_text, nullptr, 16));
        } catch (...) {
            return "";
        }
        if (size == 0) {
            return output;  // the terminal chunk; trailers are skipped
        }
        if (line_end + 2 + size > input.size()) return "";
        output.append(input, line_end + 2, size);
        cursor = line_end + 2 + size;
        if (cursor + 2 > input.size()) return "";
        cursor += 2;  // the CRLF after the chunk data
    }
    return output;
}

}  // namespace

// ---------------------------------------------------------------------------
// HttpWorkerTransport
// ---------------------------------------------------------------------------

vortyx::platform::Status HttpWorkerTransport::create(const HttpTransportConfig& config,
                                                     std::unique_ptr<IWorkerApiTransport>& out,
                                                     std::string& error) {
    // Endpoint: http://host[:port][/prefix] — https is REFUSED (documented
    // scope; TLS termination belongs to the deployment).
    const std::string scheme_separator = "://";
    const std::size_t scheme_end = config.endpoint.find(scheme_separator);
    if (scheme_end == std::string::npos) {
        error = "endpoint must be http://host:port[/prefix]";
        return vortyx::platform::Status::InvalidInput;
    }
    const std::string scheme = config.endpoint.substr(0, scheme_end);
    if (scheme != "http") {
        error = "only plain http:// endpoints are supported by the native worker "
                "(terminate TLS at a reverse proxy in front of the API)";
        return vortyx::platform::Status::InvalidInput;
    }

    std::unique_ptr<HttpWorkerTransport> transport(new HttpWorkerTransport());
    transport->config_ = config;

    std::string rest = config.endpoint.substr(scheme_end + scheme_separator.size());
    const std::size_t path_start = rest.find('/');
    std::string host_port = path_start == std::string::npos ? rest : rest.substr(0, path_start);
    transport->prefix_ = path_start == std::string::npos ? "" : rest.substr(path_start);
    while (transport->prefix_.size() > 1 && transport->prefix_.back() == '/') {
        transport->prefix_.pop_back();
    }

    if (host_port.empty()) {
        error = "endpoint is missing the host";
        return vortyx::platform::Status::InvalidInput;
    }
    if (host_port.front() == '[') {
        error = "IPv6 literal endpoints are not supported (use an IPv4 host name)";
        return vortyx::platform::Status::InvalidInput;
    }
    const std::size_t colon = host_port.find(':');
    if (colon == std::string::npos) {
        transport->host_ = host_port;
        transport->port_ = "80";
    } else {
        transport->host_ = host_port.substr(0, colon);
        transport->port_ = host_port.substr(colon + 1);
        for (const char c : transport->port_) {
            if (c < '0' || c > '9') {
                error = "endpoint port must be numeric";
                return vortyx::platform::Status::InvalidInput;
            }
        }
        if (transport->port_.empty() || transport->port_.size() > 5) {
            error = "endpoint port is invalid";
            return vortyx::platform::Status::InvalidInput;
        }
    }
    if (transport->host_.empty()) {
        error = "endpoint is missing the host";
        return vortyx::platform::Status::InvalidInput;
    }
    if (config.timeout_ms <= 0 || config.max_response_bytes <= 0) {
        error = "timeout and response cap must be positive";
        return vortyx::platform::Status::InvalidInput;
    }

    // Resolve once at configuration time: a bad host is a startup failure,
    // not a per-request surprise.
    std::string ipv4;
    if (!resolve_host(transport->host_, ipv4)) {
        error = "endpoint host '" + transport->host_ + "' does not resolve";
        return vortyx::platform::Status::InvalidInput;
    }

    out = std::move(transport);
    return vortyx::platform::Status::Ok;
}

HttpWorkerTransport::~HttpWorkerTransport() = default;

HttpWorkerTransport::Response HttpWorkerTransport::exchange(const std::string& method,
                                                            const std::string& path,
                                                            const std::string& body) {
    Response outcome;
#if defined(_WIN32)
    (void)wsa();
#endif

    std::string ipv4;
    if (!resolve_host(host_, ipv4)) {
        outcome.error = "host no longer resolves: " + host_;
        return outcome;
    }

    const SocketHandle handle = open_stream();
    if (handle == kInvalidSocket) {
        outcome.error = "socket creation failed: " + last_error();
        return outcome;
    }

    // Close on every path out (RAII-lite via a guard lambda).
    struct Guard {
        SocketHandle handle;
        ~Guard() { close_socket(handle); }
    } guard{handle};

    if (!set_socket_timeout(handle, config_.timeout_ms)) {
        outcome.error = "socket timeout configuration failed: " + last_error();
        return outcome;
    }
    if (!connect_bounded(handle, ipv4, static_cast<std::uint16_t>(std::stoi(port_)),
                         config_.timeout_ms)) {
        outcome.error = "connect failed (timeout " + std::to_string(config_.timeout_ms) +
                        " ms): " + last_error();
        return outcome;
    }

    std::string request = method + " " + prefix_ + path + " HTTP/1.1\r\n";
    request += "Host: " + host_ + ":" + port_ + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Accept: application/json\r\n";
    if (!config_.bearer_token.empty()) {
        request += "Authorization: Bearer " + config_.bearer_token + "\r\n";
    }
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;

    if (!send_all(handle, request)) {
        outcome.error = "send failed: " + last_error();
        return outcome;
    }

    // Read to EOF (Connection: close framing) under the response cap.
    std::string raw;
    raw.reserve(4096);
    char buffer[8192];
    while (raw.size() < static_cast<std::size_t>(config_.max_response_bytes)) {
        const int received = ::recv(handle, buffer, sizeof(buffer), 0);
        if (received < 0) {
            outcome.error = "receive failed: " + last_error();
            return outcome;
        }
        if (received == 0) break;
        raw.append(buffer, static_cast<std::size_t>(received));
    }
    if (raw.size() >= static_cast<std::size_t>(config_.max_response_bytes)) {
        outcome.error = "response exceeds the size cap";
        return outcome;
    }

    // Status line: HTTP/1.x NNN
    const std::size_t first_space = raw.find(' ');
    if (raw.compare(0, 5, "HTTP/") != 0 || first_space == std::string::npos) {
        outcome.error = "response is not HTTP";
        return outcome;
    }
    const std::size_t second_space = raw.find(' ', first_space + 1);
    if (second_space == std::string::npos) {
        outcome.error = "response status line is malformed";
        return outcome;
    }
    const std::string status_text = raw.substr(first_space + 1, second_space - first_space - 1);
    for (const char c : status_text) {
        if (c < '0' || c > '9') {
            outcome.error = "response status is not numeric";
            return outcome;
        }
    }
    outcome.status = std::stoi(status_text);

    const std::size_t header_end = raw.find("\r\n\r\n");
    const std::string body_bytes =
        header_end == std::string::npos ? "" : raw.substr(header_end + 4);
    // Framing: Content-Length when present; chunked decoding when the
    // server streams (Node/Vercel do for bodies without a known length).
    if (transfer_encoding_chunked(raw, header_end)) {
        outcome.body = decode_chunked(body_bytes);
        if (outcome.body.empty() && !body_bytes.empty()) {
            outcome.error = "chunked response could not be decoded";
            return outcome;
        }
    } else {
        outcome.body = body_bytes;
    }
    // A 204/304-style bodyless response has no body by definition.
    if (outcome.status == 204 || outcome.status == 304) outcome.body.clear();
    outcome.ok = true;
    return outcome;
}

IWorkerApiTransport::Response HttpWorkerTransport::post(const std::string& path,
                                                        const std::string& json_body) {
    return exchange("POST", path, json_body);
}

IWorkerApiTransport::Response HttpWorkerTransport::get(const std::string& path) {
    return exchange("GET", path, "");
}

}  // namespace vortyx::worker
