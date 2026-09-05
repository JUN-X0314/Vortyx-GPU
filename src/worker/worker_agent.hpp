#pragma once

// The native worker agent (Phase 15) — the loop that makes the control
// plane's job lifecycle REAL.
//
// One cycle:
//   1. claim        POST {base}/api/worker/claim    (Bearer worker token)
//   2. execute      the claimed metadata runs on the INativeExecutor (the
//                   real Phase 12 stack — see native_executor.hpp) while a
//                   heartbeat thread renews the lease and relays a
//                   cancel_requested observation into executor.request_cancel
//   3. report       POST .../complete with the TERMINAL outcome the
//                   orchestrator's record actually shows (completed / failed /
//                   cancelled + result metadata)
//
// HONESTY RULES (the ones this module exists to keep):
//   * a job is reported terminal ONLY from a terminal orchestrator record —
//     there is no path that reports success without execution;
//   * a transport/protocol failure during reporting leaves the job in the
//     control plane's hands (lease expiry + reconciliation own it) and is
//     reported to the operator — never swallowed, never faked;
//   * an unclaimable/failed claim cycle is a logged outcome, not an error
//     the agent crashes on (workers outlive transient control-plane faults).
//
// DETERMINISM: the heartbeat thread's schedule is configuration (interval),
// and its FIRST beat fires immediately when execution starts — tests pin the
// cancel relay with a scripted transport and a blocking heartbeat (no
// sleeps in assertions).

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "worker/native_executor.hpp"
#include "worker/worker_protocol.hpp"
#include "worker/worker_transport.hpp"

namespace vortyx::worker {

struct WorkerAgentConfig {
    std::string worker_id;      // stable label (claimed lease ownership)
    std::int64_t poll_interval_ms = 2000;   // idle poll cadence
    std::int64_t heartbeat_interval_ms = 15000;  // lease renewal cadence
    std::int64_t lease_ms = 60000;          // requested claim lease
};

class WorkerAgent {
public:
    // 'transport' must outlive the agent. 'executor' likewise. Errors:
    // InvalidInput (bad config — ids empty, intervals non-positive, lease
    // shorter than two heartbeat intervals).
    static vortyx::platform::Status create(IWorkerApiTransport* transport,
                                           INativeExecutor* executor,
                                           const WorkerAgentConfig& config,
                                           std::unique_ptr<WorkerAgent>& out,
                                           std::string& error);

    ~WorkerAgent();

    WorkerAgent(const WorkerAgent&) = delete;
    WorkerAgent& operator=(const WorkerAgent&) = delete;

    // One poll→execute→report cycle. Returns what happened (observable
    // vocabulary for the CLI and the tests).
    enum class CycleResult {
        Claimed,   // a job was claimed, executed and reported
        NoWork,    // the control plane had nothing queued
        Error,     // the control plane refused / failed (detail in 'detail')
    };

    // Runs one cycle. 'detail' carries a human-readable summary (the CLI
    // prints it; tests assert on it).
    CycleResult run_cycle(std::string& detail);

    // Stops a mid-flight heartbeat thread (destruction calls this too).
    void stop_heartbeat();

    // The last terminal status the agent reported (observability).
    const std::string& last_reported_status() const { return last_reported_status_; }

private:
    WorkerAgent() = default;

    // The heartbeater: renews the lease every interval (first beat
    // immediate) and relays cancel_requested into the executor. Runs for
    // one execution at a time.
    void heartbeat_loop(const std::string& job_id);

    IWorkerApiTransport* transport_ = nullptr;
    INativeExecutor* executor_ = nullptr;
    WorkerAgentConfig config_;

    std::atomic<bool> heartbeat_stop_{false};
    std::atomic<bool> cancel_observed_{false};
    std::string last_reported_status_;
};

}  // namespace vortyx::worker
