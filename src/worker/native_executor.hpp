#pragma once

// Native executor (Phase 15) — the execution-plane seam of the worker.
//
// The native worker agent (worker_agent.hpp) claims a job from the control
// plane and hands the job's METADATA to an INativeExecutor. The executor is
// the ONLY component that turns metadata into real computation: it builds
// the concrete payload and drives the EXISTING Phase 12 distributed stack
// (LocalDeviceRegistry + LocalInProcessTransport + LocalWorker runtimes +
// DistributedOrchestrator — no second scheduler, no second execution
// engine).
//
// PAYLOAD SYNTHESIS (the honest consequence of the metadata-only protocol):
// the control plane stores metadata only — no tensor data, no result bytes
// (the Phase 11/12/14 rule, kept). The executor therefore synthesizes the
// input payload DETERMINISTICALLY from the job id and the claimed element
// count (a fixed keyed generator, documented and pinned by tests): the same
// job id always produces the same inputs, a re-claim re-produces the same
// execution, and nothing about the control plane pretends to carry data.
// VectorScale's scalar operand is part of that synthesized payload — an
// execution detail of the executor, never a control-plane field.
//
// HONEST EXECUTION: the executor reports what the orchestrator's record
// actually says — Completed / Failed / Cancelled, real shard counts, real
// backends. A claimed job that cannot execute (no device, bad operation)
// FAILS; it is never reported as completed.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "distributed/distributed.hpp"
#include "platform/platform.hpp"

namespace vortyx::worker {

using vortyx::platform::JobId;
using vortyx::platform::Status;
using vortyx::platform::UserId;

// The execution boundary: one claimed job's metadata in, one terminal
// orchestrator record out. Implementations must be safe for one executing
// thread plus one cancellation requester (request_cancel from another
// thread) — exactly the concurrency the worker agent runs.
class INativeExecutor {
public:
    virtual ~INativeExecutor() = default;

    // Executes the claimed job SYNCHRONOUSLY: fills 'request.task' with the
    // deterministic synthesized payload (from request.envelope), submits
    // into the real Phase 12 stack, and returns when the job is terminal.
    // 'out' carries the terminal record (status/error/shards/result
    // metadata — never a fabricated success). Errors: InvalidInput
    // (metadata that cannot form a valid job).
    virtual Status execute(vortyx::distributed::DistributedJobRequest& request,
                           vortyx::distributed::DistributedJobRecord& out) = 0;

    // Delivers a cancellation request for a job this executor is (or may
    // soon be) running. Ok when the request reached the executing record
    // (or its pending intent); the record's own state machine decides the
    // outcome.
    virtual Status request_cancel(const JobId& job_id) = 0;
};

// The local executor configuration: the SHAPE of the agent's local cluster
// (self-reported capacities of virtual devices — configuration, never a
// hardware measurement, the Phase 12 simulator rule).
struct NativeExecutorConfig {
    UserId owner_user_id = "vortyx-worker";                  // the agent's local identity
    std::uint32_t device_count = 2;                          // 1..64 virtual devices
    std::int64_t device_memory_bytes = 256LL * 1024 * 1024;  // per device
    std::int64_t device_concurrent_jobs = 2;                 // per device
};

// Deterministically synthesizes the task payload for one claimed job from
// (job_id, operation, element_count). Same key -> same bytes, always. This
// is the documented executor-side consequence of the metadata-only control
// plane; tests pin the determinism (not the exact values).
Status synthesize_task(const vortyx::platform::JobId& job_id,
                       vortyx::compute::ComputeOp operation, std::uint64_t element_count,
                       vortyx::compute::ComputeTask& out, std::string& error);

// The real implementation: a local simulated cluster driving the UNCHANGED
// Phase 12 orchestrator. This is what vortyx_worker runs in every mode
// today; a remote/hardware executor would implement the same interface.
class SimulatorNativeExecutor final : public INativeExecutor {
public:
    // Builds the cluster (registry, transport, N simulated devices) and the
    // orchestrator over it. Errors: InvalidInput (bad config) | propagated
    // construction failures (unknown policy etc.).
    static Status create(const NativeExecutorConfig& config,
                         std::unique_ptr<INativeExecutor>& out, std::string& error);

    ~SimulatorNativeExecutor() override;

    SimulatorNativeExecutor(const SimulatorNativeExecutor&) = delete;
    SimulatorNativeExecutor& operator=(const SimulatorNativeExecutor&) = delete;

    Status execute(vortyx::distributed::DistributedJobRequest& request,
                   vortyx::distributed::DistributedJobRecord& out) override;

    Status request_cancel(const JobId& job_id) override;

private:
    SimulatorNativeExecutor() = default;

    NativeExecutorConfig config_;
    std::shared_ptr<vortyx::distributed::IClock> clock_;
    std::unique_ptr<vortyx::distributed::LocalDeviceRegistry> registry_;
    std::unique_ptr<vortyx::distributed::LocalInProcessTransport> transport_;
    std::unique_ptr<vortyx::distributed::LocalMultiDeviceSimulator> simulator_;
    std::unique_ptr<vortyx::distributed::DistributedOrchestrator> orchestrator_;

    // One executing thread at a time (the agent's contract): submit() is
    // synchronous, so concurrent cycle attempts serialize here. The
    // cancellation path (request_cancel) deliberately does NOT take this
    // mutex — it must reach the orchestrator while execute() is running.
    std::mutex execute_mutex_;
};

}  // namespace vortyx::worker
