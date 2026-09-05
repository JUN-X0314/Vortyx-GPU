#pragma once

// Worker abstraction (Phase 12) — the execution end of a device.
//
// A Worker is the ONLY component that turns a placed shard into real
// compute work, and it does so exclusively through the EXISTING local
// execution path (vortyx::compute::Runtime -> Resource Manager -> Backend).
// Nothing about ComputeTask execution is duplicated here: the worker
// validates the assignment, slices the caller's task to the shard's range
// (a pure host-side copy of the slice — the elementwise ops are
// range-independent, pinned bit-exact by tests), executes the slice
// through the Runtime, and reports a ShardResult.
//
// WORKER IDENTITY: one worker serves exactly one DeviceId. The device is
// the worker's identity; a worker never executes for another device.
//
// LIFECYCLE (worker state, separate from DeviceState — a worker is the
// process-side executor, the device is the registry record):
//
//   Starting -> Ready -> Running -> Draining -> Stopped
//                 ^___________|  (Running returns to Ready between shards)
//
//   Starting  — constructed, runtime not yet initialized
//   Ready     — initialized, accepting assignments
//   Running   — at least one shard execution in flight
//   Draining  — accepting no new assignments (finishing in-flight ones)
//   Stopped   — terminal; the runtime is shut down
//
// THREADING: a Runtime is NOT thread-safe (the Phase 4 contract). Each
// LocalWorker owns its Runtime exclusively and serializes executions
// through its own mutex — concurrent shards for the same device execute
// one after another (bounded by the device's concurrency declaration at
// the PLACEMENT level; the worker's mutex is the last-resort correctness
// gate). Different devices' workers are independent and run in parallel.
//
// NO exceptions cross this boundary: every outcome is a ShardResult with a
// stable FailureCode and a human-readable error.

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/compute/runtime.hpp"   // the execution path (adapter target)
#include "distributed/device.hpp"     // capabilities
#include "distributed/resource.hpp"
#include "distributed/shard.hpp"      // WorkPartition / ElementRange
#include "distributed/retry.hpp"      // FailureCode
#include "platform/identity.hpp"

namespace vortyx::distributed {

using vortyx::platform::DeviceId;  // reused platform identity (see device.hpp)
using vortyx::platform::JobId;

enum class WorkerState {
    Starting,
    Ready,
    Running,
    Draining,
    Stopped,
};

const char* to_string(WorkerState state);

// One executable shard assignment: the shard's identity + partition, the
// FULL local task (the worker slices it), and the resolved backend ("" =
// the device's own preferred backend).
struct ShardExecution {
    std::string shard_id;
    JobId parent_job_id;
    std::uint32_t shard_index = 0;
    std::uint32_t attempt = 0;
    DeviceId device_id;        // must match the worker's device
    WorkPartition work;
    vortyx::compute::ComputeTask task;   // the local payload (sliced here)
    std::string backend;                 // resolved backend ("" = preferred)
    std::string lease_id;                // the reservation backing this run
};

// The outcome of one shard execution attempt. 'data' holds the slice's
// output when status == Completed (the aggregator reassembles the whole).
struct ShardResult {
    std::string shard_id;
    JobId parent_job_id;
    std::uint32_t shard_index = 0;
    std::uint32_t attempt = 0;
    DeviceId device_id;
    std::string backend;              // backend actually used ("" if never ran)

    bool completed = false;           // true = success, false = failure
    FailureCode failure_code = FailureCode::None;
    std::string error;                // human-readable reason when failed

    std::vector<std::int32_t> data;   // slice output when completed
    std::uint64_t element_begin = 0;  // where the slice sits in the domain
    std::uint64_t element_end = 0;
};

class IWorker {
public:
    virtual ~IWorker() = default;

    // The device this worker serves.
    virtual const DeviceId& device_id() const = 0;

    // Current lifecycle state.
    virtual WorkerState state() const = 0;

    // Prepares the worker (initializes its runtime). Starting -> Ready.
    // Errors: InvalidInput | BackendError via Status (the runtime's own
    // reason is in 'error'). Ready is idempotent.
    virtual vortyx::compute::Status start(std::string& error) = 0;

    // Ready/Running -> Draining (no new assignments; in-flight ones finish).
    // Errors: InvalidInput on an illegal transition.
    virtual vortyx::compute::Status drain() = 0;

    // Any -> Stopped (terminal). Safe to call repeatedly. Releases the
    // runtime. Never touches the registry or any lease (that is the
    // orchestrator's bookkeeping).
    virtual void stop() = 0;

    // Executes one shard attempt. Validates the assignment first (device
    // match, partition shape, range within the task, state). A refused
    // execution returns a FAILED ShardResult with the precise
    // FailureCode — it never throws and never executes partially.
    virtual ShardResult execute_shard(const ShardExecution& execution) = 0;
};

// The local implementation: one worker, one runtime, one device. This is
// the adapter the local multi-device simulator instantiates per virtual
// device; a remote worker would implement IWorkerTransport-side RPC behind
// the same interface instead (Phase 13+ seam — no network code exists here).
class LocalWorker final : public IWorker {
public:
    // 'claimed_operations' is the set of operations this device claims (the
    // same vocabulary the device's capabilities declare). An assignment
    // whose operation is outside the claim is refused (the placement lied
    // about capability).
    LocalWorker(DeviceId device_id,
                std::vector<vortyx::compute::ComputeOp> claimed_operations);
    ~LocalWorker() override;

    LocalWorker(const LocalWorker&) = delete;
    LocalWorker& operator=(const LocalWorker&) = delete;
    LocalWorker(LocalWorker&&) = delete;
    LocalWorker& operator=(LocalWorker&&) = delete;

    const DeviceId& device_id() const override { return device_id_; }
    WorkerState state() const override;

    vortyx::compute::Status start(std::string& error) override;
    vortyx::compute::Status drain() override;
    void stop() override;

    ShardResult execute_shard(const ShardExecution& execution) override;

private:
    // Validates and slices; returns false with a ShardResult failure filled.
    bool validate_and_slice(const ShardExecution& execution, vortyx::compute::ComputeTask& slice,
                            std::string& resolved_backend, ShardResult& failure);

    DeviceId device_id_;
    std::vector<vortyx::compute::ComputeOp> claimed_operations_;

    std::unique_ptr<vortyx::compute::Runtime> runtime_;
    WorkerState state_ = WorkerState::Starting;

    // Serializes runtime access (the Phase 4 single-thread contract) and
    // the lifecycle state. Mutable: state() is a const query.
    mutable std::mutex mutex_;
};

}  // namespace vortyx::distributed
