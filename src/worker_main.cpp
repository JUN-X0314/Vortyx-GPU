// vortyx_worker — the Phase 15 native execution agent.
//
// The bridge between the Vortyx control plane (the web/API) and the real
// execution engine: it claims queued jobs over the worker protocol, executes
// them on the existing Phase 12 distributed stack (local simulated devices —
// the honest local execution story), and reports the actual terminal
// outcome. It does NOT implement its own scheduler, queue or job state
// machine — the control plane owns the lifecycle; this process owns
// execution.
//
// Configuration (environment; see docs/worker/local-development.md):
//   VORTYX_WORKER_ENDPOINT      required  http://host:port[/prefix]
//   VORTYX_WORKER_TOKEN         required  the worker bearer token
//   VORTYX_WORKER_ID            optional  default "vortyx-worker-<pid>"
//   VORTYX_WORKER_POLL_MS       optional  default 2000
//   VORTYX_WORKER_LEASE_MS      optional  default 60000
//   VORTYX_WORKER_HEARTBEAT_MS  optional  default 15000
//   VORTYX_WORKER_DEVICES       optional  default 2 (simulated devices)
//   VORTYX_WORKER_DEVICE_MEMORY_MB  optional  default 256
//
// The agent runs until interrupted (Ctrl-C); every cycle prints its honest
// outcome. A control plane that is unreachable is a logged condition, not a
// crash — a worker outlives transient server faults.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#define VORTYX_GETPID _getpid
#else
#include <unistd.h>
#define VORTYX_GETPID getpid
#endif

#include "core/version.hpp"
#include "worker/http_transport.hpp"
#include "worker/worker_agent.hpp"

namespace {

std::atomic<bool> g_running{true};

void handle_signal(int) { g_running.store(false); }

bool env_int64(const char* name, std::int64_t& out) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') return true;  // absent: keep default
    try {
        std::size_t consumed = 0;
        const long long parsed = std::stoll(raw, &consumed);
        if (consumed != std::string(raw).size()) return false;
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

int main() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::cout << "Vortyx Worker (Phase 15 native execution agent, config "
              << VORTYX_BUILD_CONFIG << ")\n";
    std::cout << "version " << VORTYX_VERSION_STRING << "\n\n";

    const char* endpoint = std::getenv("VORTYX_WORKER_ENDPOINT");
    const char* token = std::getenv("VORTYX_WORKER_TOKEN");
    if (endpoint == nullptr || *endpoint == '\0' || token == nullptr || *token == '\0') {
        std::cerr << "configuration error: VORTYX_WORKER_ENDPOINT and VORTYX_WORKER_TOKEN "
                     "are required (see docs/worker/local-development.md)\n";
        return 2;
    }

    std::int64_t poll_ms = 2000;
    std::int64_t lease_ms = 60000;
    std::int64_t heartbeat_ms = 15000;
    std::int64_t devices = 2;
    std::int64_t device_memory_mb = 256;
    if (!env_int64("VORTYX_WORKER_POLL_MS", poll_ms) || poll_ms <= 0) {
        std::cerr << "configuration error: VORTYX_WORKER_POLL_MS must be a positive integer\n";
        return 2;
    }
    if (!env_int64("VORTYX_WORKER_LEASE_MS", lease_ms) || lease_ms <= 0) {
        std::cerr << "configuration error: VORTYX_WORKER_LEASE_MS must be a positive integer\n";
        return 2;
    }
    if (!env_int64("VORTYX_WORKER_HEARTBEAT_MS", heartbeat_ms) || heartbeat_ms <= 0) {
        std::cerr << "configuration error: VORTYX_WORKER_HEARTBEAT_MS must be positive\n";
        return 2;
    }
    if (!env_int64("VORTYX_WORKER_DEVICES", devices) || devices <= 0 || devices > 64) {
        std::cerr << "configuration error: VORTYX_WORKER_DEVICES must be 1..64\n";
        return 2;
    }
    if (!env_int64("VORTYX_WORKER_DEVICE_MEMORY_MB", device_memory_mb) ||
        device_memory_mb <= 0) {
        std::cerr << "configuration error: VORTYX_WORKER_DEVICE_MEMORY_MB must be positive\n";
        return 2;
    }

    std::string worker_id_raw;
    const char* worker_id = std::getenv("VORTYX_WORKER_ID");
    if (worker_id != nullptr && *worker_id != '\0') {
        worker_id_raw = worker_id;
    } else {
        worker_id_raw = "vortyx-worker-" + std::to_string(VORTYX_GETPID());
    }

    vortyx::worker::HttpTransportConfig transport_config;
    transport_config.endpoint = endpoint;
    transport_config.bearer_token = token;
    transport_config.timeout_ms = 10000;

    std::unique_ptr<vortyx::worker::IWorkerApiTransport> transport;
    std::string error;
    if (vortyx::worker::HttpWorkerTransport::create(transport_config, transport, error) !=
        vortyx::platform::Status::Ok) {
        std::cerr << "configuration error: " << error << "\n";
        return 2;
    }

    vortyx::worker::NativeExecutorConfig executor_config;
    executor_config.device_count = static_cast<std::uint32_t>(devices);
    executor_config.device_memory_bytes = device_memory_mb * 1024 * 1024;
    std::unique_ptr<vortyx::worker::INativeExecutor> executor;
    if (vortyx::worker::SimulatorNativeExecutor::create(executor_config, executor, error) !=
        vortyx::platform::Status::Ok) {
        std::cerr << "executor error: " << error << "\n";
        return 2;
    }

    vortyx::worker::WorkerAgentConfig agent_config;
    agent_config.worker_id = worker_id_raw;
    agent_config.poll_interval_ms = poll_ms;
    agent_config.lease_ms = lease_ms;
    agent_config.heartbeat_interval_ms = heartbeat_ms;

    std::unique_ptr<vortyx::worker::WorkerAgent> agent;
    if (vortyx::worker::WorkerAgent::create(transport.get(), executor.get(), agent_config,
                                            agent, error) != vortyx::platform::Status::Ok) {
        std::cerr << "agent error: " << error << "\n";
        return 2;
    }

    std::cout << "worker id:     " << worker_id_raw << "\n"
              << "endpoint:      " << endpoint << "\n"
              << "devices:       " << devices << " (local simulated, real compute)\n"
              << "poll/lease/hb: " << poll_ms << "/" << lease_ms << "/" << heartbeat_ms
              << " ms\n\n";

    // A health probe first: an unreachable control plane is reported
    // honestly before the loop starts.
    const vortyx::worker::IWorkerApiTransport::Response health = transport->get("/api/health");
    if (!health.ok) {
        std::cerr << "control plane probe failed: " << health.error << "\n";
    } else {
        std::cout << "control plane probe: HTTP " << health.status << "\n\n";
    }

    while (g_running.load()) {
        std::string detail;
        const vortyx::worker::WorkerAgent::CycleResult result = agent->run_cycle(detail);
        switch (result) {
            case vortyx::worker::WorkerAgent::CycleResult::Claimed:
                std::cout << "[claimed] " << detail << "\n";
                break;
            case vortyx::worker::WorkerAgent::CycleResult::NoWork:
                std::cout << "[idle] " << detail << "\n";
                break;
            case vortyx::worker::WorkerAgent::CycleResult::Error:
                std::cerr << "[error] " << detail << "\n";
                break;
        }
        if (!g_running.load()) break;

        // Idle/error backoff: poll cadence, sliced so shutdown is prompt.
        std::int64_t slept = 0;
        while (g_running.load() && slept < poll_ms) {
            const std::int64_t slice = poll_ms - slept > 50 ? 50 : poll_ms - slept;
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            slept += slice;
        }
    }

    std::cout << "\nworker stopped\n";
    return 0;
}
