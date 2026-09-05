#pragma once

// Local multi-device simulator (Phase 12) — the honest test rig.
//
// WHAT IT IS: a factory for N logical devices that behave like real Vortyx
// nodes from the cluster's point of view: each has a registry record with
// SELF-REPORTED capabilities, an EXCLUSIVELY OWNED compute Runtime (the
// unchanged Phase 3/10 execution path), and a LocalWorker that executes
// shards through that Runtime. The transport dispatches to them in
// process; the whole distributed flow (registry -> placement -> lease ->
// worker -> runtime -> aggregation) therefore runs for real without any
// GPU hardware or network.
//
// WHAT IT IS NOT (loudly): a performance model, a hardware emulator, or a
// claim about real multi-GPU behavior. Capacities are the CONFIGURATION
// given below (documented as such in the device model); execution times
// are real CPU times of real compute work and carry no GPU meaning.
//
// HONEST CAPABILITY REPORTING: a simulated device's backend list is
// queried from its own Runtime (has_backend / backend_names) — the same
// answer a real node would give. If the host has a working Vulkan device,
// every simulated device honestly reports "vulkan" alongside "cpu";
// otherwise it reports only "cpu". Nothing is fabricated either way.
//
// All simulated devices belong to ONE owner (the simulator's user id) —
// the local cluster of Phase 12 is single-owner by construction; the
// orchestrator/registry ownership rules are exactly the multi-user ones.

#include <memory>
#include <string>
#include <vector>

#include "distributed/device.hpp"
#include "distributed/registry.hpp"
#include "distributed/transport.hpp"  // LocalInProcessTransport (the loopback)
#include "distributed/worker.hpp"
#include "platform/status.hpp"

namespace vortyx::distributed {

using vortyx::platform::UserId;  // reused platform identity (see device.hpp)

// One simulated device's configuration (self-reported capacity — never a
// hardware measurement).
struct SimulatorDeviceConfig {
    DeviceId device_id;
    std::string display_name;

    ResourceVector capacity;                    // self-reported schedulable capacity
    std::int64_t max_concurrent_shards = 0;     // must cover capacity.concurrent_jobs
    std::vector<vortyx::compute::ComputeOp> operations = {
        vortyx::compute::ComputeOp::VectorAdd, vortyx::compute::ComputeOp::VectorMultiply,
        vortyx::compute::ComputeOp::VectorScale};
};

class LocalMultiDeviceSimulator {
public:
    // Non-owning registry/transport references (both must outlive the
    // simulator; the orchestrator's Deps point at the same objects). The
    // simulator attaches to the LOCAL transport — the only transport
    // Phase 12 ships; a future remote transport gets its own rig.
    LocalMultiDeviceSimulator(IDeviceRegistry& registry, LocalInProcessTransport& transport,
                              UserId owner_user_id);
    ~LocalMultiDeviceSimulator();

    LocalMultiDeviceSimulator(const LocalMultiDeviceSimulator&) = delete;
    LocalMultiDeviceSimulator& operator=(const LocalMultiDeviceSimulator&) = delete;

    // Creates one virtual device: worker + runtime (started, backends
    // honestly queried), registry record (registered, activated Ready,
    // heartbeat stamped Healthy). 'created' mirrors the registry's rule.
    // Errors: InvalidInput (bad config / id / the worker's runtime failed
    // to start) | Conflict (duplicate id with different payload) |
    // Internal.
    vortyx::platform::Status add_device(const SimulatorDeviceConfig& config, bool& created,
                                        std::string& error);

    // The devices' workers (observability: lifecycle states for the CLI).
    std::vector<LocalWorker*> workers();

private:
    IDeviceRegistry& registry_;
    LocalInProcessTransport& transport_;
    UserId owner_user_id_;

    std::vector<std::unique_ptr<LocalWorker>> workers_;
};

}  // namespace vortyx::distributed
