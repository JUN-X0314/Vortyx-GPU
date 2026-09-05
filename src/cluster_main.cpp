// vortyx_cluster — the Phase 12 diagnostic tool.
//
// Builds a local multi-device cluster through the REAL distributed path
// (LocalDeviceRegistry + LocalInProcessTransport + LocalWorker runtimes +
// DistributedOrchestrator), runs a distributed job end to end (placement
// -> leases -> shard execution on the existing compute runtime ->
// deterministic aggregation -> platform-store mirroring), then prints the
// observable state: the cluster view, the job record and the result
// counts. It exists so distributed state is never a black box.
//
// This is a diagnostic, not a benchmark: it makes no performance claims,
// and the simulated devices' capacities are the explicit configuration
// printed below (never a hardware measurement).

#include <iostream>

#include "core/version.hpp"  // VORTYX_VERSION_STRING
#include "distributed/distributed.hpp"
#include "platform/platform.hpp"

int main() {
    std::cout << "Vortyx Cluster (Phase 12 diagnostic, config " << VORTYX_BUILD_CONFIG << ")\n";
    std::cout << "version " << VORTYX_VERSION_STRING << ", protocol "
              << vortyx::platform::kProtocolVersion << "\n\n";

    // ---- the local cluster: registry + transport + clock -------------------
    auto clock = std::make_shared<vortyx::distributed::SteadyClock>();
    vortyx::distributed::LocalDeviceRegistry registry(clock);
    vortyx::distributed::LocalInProcessTransport transport;

    vortyx::distributed::DistributedConfig config;  // safe defaults, distributed ON here
    config.enabled = true;

    vortyx::distributed::DistributedOrchestrator::Deps deps;
    deps.registry = &registry;
    deps.transport = &transport;
    deps.clock = clock;
    deps.platform_store = nullptr;  // the tool runs the pure local flow

    std::unique_ptr<vortyx::distributed::DistributedOrchestrator> orchestrator;
    std::string error;
    if (vortyx::distributed::DistributedOrchestrator::create(deps, config, orchestrator, error) !=
        vortyx::platform::Status::Ok) {
        std::cerr << "orchestrator creation failed: " << error << "\n";
        return 1;
    }

    // ---- four virtual devices with different capacities --------------------
    const vortyx::distributed::UserId owner = "local-operator";
    vortyx::distributed::LocalMultiDeviceSimulator simulator(registry, transport, owner);

    struct Spec {
        const char* id;
        const char* name;
        std::int64_t memory_mb;
        std::int64_t jobs;
    };
    const Spec specs[] = {
        {"device-0", "sim-small", 8, 1},
        {"device-1", "sim-medium", 16, 2},
        {"device-2", "sim-medium", 16, 2},
        {"device-3", "sim-large", 64, 4},
    };
    for (const Spec& spec : specs) {
        vortyx::distributed::SimulatorDeviceConfig device;
        device.device_id = spec.id;
        device.display_name = spec.name;
        device.capacity.compute_units = 0;  // compute units are not fabricated
        device.capacity.memory_bytes = spec.memory_mb * 1024 * 1024;
        device.capacity.concurrent_jobs = spec.jobs;
        device.max_concurrent_shards = spec.jobs;
        bool created = false;
        if (simulator.add_device(device, created, error) != vortyx::platform::Status::Ok) {
            std::cerr << "device '" << spec.id << "' failed: " << error << "\n";
            return 1;
        }
    }

    // ---- the cluster view (the "vortyx devices/cluster" output) ------------
    std::cout << "== cluster ==\n"
              << vortyx::distributed::to_debug_string(orchestrator->cluster_snapshot(owner))
              << "\n\n";

    // ---- one distributed job over 4 shards ---------------------------------
    vortyx::distributed::DistributedJobRequest request;
    request.envelope.job_id = vortyx::platform::generate_job_id();
    request.envelope.operation = vortyx::compute::ComputeOp::VectorAdd;
    request.envelope.element_count = 40000;
    request.envelope.requested_backend = "cpu";
    request.requested_shard_count = 4;

    const std::size_t n = 40000;
    request.task.a.resize(n);
    request.task.b.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        request.task.a[i] = static_cast<std::int32_t>(i);
        request.task.b[i] = static_cast<std::int32_t>(2 * i + 1);
    }

    vortyx::distributed::DistributedJobRecord job;
    bool created = false;
    const vortyx::platform::AuthContext auth = vortyx::platform::make_authenticated(owner);
    if (orchestrator->submit(auth, request, job, created) != vortyx::platform::Status::Ok) {
        std::cerr << "submit failed\n";
        return 1;
    }

    std::cout << "== job ==\n" << vortyx::distributed::to_debug_string(job) << "\n\n";

    // ---- the verification the tool exists to show --------------------------
    if (job.status == vortyx::distributed::DistributedJobStatus::Completed) {
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (job.result.data[i] != request.task.a[i] + request.task.b[i]) ++mismatches;
        }
        std::cout << "== result ==\n";
        std::cout << "status: completed\n";
        std::cout << "shards: " << job.result.succeeded << "/" << job.result.shard_count
                  << " succeeded, data elements: " << job.result.data.size() << "\n";
        std::cout << "verification against the host reference: "
                  << (mismatches == 0 ? "PASS (bit-exact)" : "FAIL") << "\n";
        std::cout << "backends used:";
        for (const std::string& backend : job.result.backends_used) {
            std::cout << " " << backend;
        }
        std::cout << "\n";
    } else {
        std::cout << "== result ==\nstatus: " << vortyx::distributed::to_string(job.status)
                  << " (" << job.error << ")\n";
    }
    return job.status == vortyx::distributed::DistributedJobStatus::Completed ? 0 : 1;
}
