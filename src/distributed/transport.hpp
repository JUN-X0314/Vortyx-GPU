#pragma once

// Transport abstraction (Phase 12) — the seam between the orchestrator and
// the workers that execute its placements.
//
// SCOPE, stated honestly: Phase 12 implements ONE transport, the
// LocalInProcessTransport ("loopback"): it dispatches shard executions to
// in-process workers by direct function call. NO HTTP, no gRPC, no
// WebSocket, no serialization — nothing here is a network implementation,
// and no document in this repository claims otherwise. The INTERFACE is
// the deliverable: a future remote transport implements the same three
// responsibilities (submit / cancel / worker lookup) against real
// connections without touching the orchestrator.
//
// Transport responsibilities (deliberately narrow):
//   submit_shard   — hand one shard execution to the worker that serves
//                    its device; return the ShardResult (synchronous in
//                    the local transport).
//   cancel_shard   — request cancellation of one pending/in-flight shard
//                    (best effort; a shard that already finished reports
//                    its real outcome — cancellation never rewrites a
//                    completed result).
//   worker_for     — the worker serving a device (nullptr when none).
//
// DETERMINISTIC FAILURE INJECTION: the local transport carries a test-only
// injection table (device -> remaining synthetic failures + code). It lets
// the suite exercise device loss, flaky workers and retry flows without
// sleeps, threads or real faults. Injection affects submit_shard only; a
// device with remaining injections fails BEFORE its worker is called.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "distributed/retry.hpp"
#include "distributed/worker.hpp"

namespace vortyx::distributed {

using vortyx::platform::DeviceId;  // reused platform identity (see device.hpp)

class IWorkerTransport {
public:
    virtual ~IWorkerTransport() = default;

    virtual ShardResult submit_shard(const ShardExecution& execution) = 0;
    virtual bool cancel_shard(const std::string& shard_id) = 0;
    virtual IWorker* worker_for(const DeviceId& device_id) = 0;
};

class LocalInProcessTransport final : public IWorkerTransport {
public:
    LocalInProcessTransport() = default;

    LocalInProcessTransport(const LocalInProcessTransport&) = delete;
    LocalInProcessTransport& operator=(const LocalInProcessTransport&) = delete;

    // Attaches a worker (non-owning; the caller — the simulator — keeps
    // the workers alive). One worker per device: a duplicate device
    // refuses (InvalidInput via the returned bool).
    bool attach(IWorker* worker);

    // Detaches everything (does NOT stop the workers — ownership stays
    // with the holder).
    void detach_all();

    // IWorkerTransport
    ShardResult submit_shard(const ShardExecution& execution) override;
    bool cancel_shard(const std::string& shard_id) override;
    IWorker* worker_for(const DeviceId& device_id) override;

    // ---- deterministic failure injection (test hook) -----------------------

    // The next 'count' submit_shard calls for 'device_id' fail with 'code'
    // before the worker runs. count == 0 clears the injection.
    void inject_failure(const DeviceId& device_id, std::uint32_t count, FailureCode code);

    // Remaining injected failure count for a device (0 = none).
    std::uint32_t injected_failures(const DeviceId& device_id) const;

private:
    struct Injection {
        std::uint32_t remaining = 0;
        FailureCode code = FailureCode::DeviceLost;
    };

    std::vector<IWorker*> workers_;
    std::vector<Injection> injections_;  // parallel to workers_
    // Cancellation requests observed by this transport (observability; the
    // local transport executes synchronously, so a cancel can only take
    // effect for work submitted from another thread).
    std::vector<std::string> cancelled_shards_;

    mutable std::mutex mutex_;
};

}  // namespace vortyx::distributed
