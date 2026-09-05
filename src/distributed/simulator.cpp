// Local multi-device simulator implementation (Phase 12).

#include "distributed/simulator.hpp"

#include "core/version.hpp"  // VORTYX_VERSION_STRING (the node's honest self-report)

namespace vortyx::distributed {

LocalMultiDeviceSimulator::LocalMultiDeviceSimulator(IDeviceRegistry& registry,
                                                     LocalInProcessTransport& transport,
                                                     UserId owner_user_id)
    : registry_(registry), transport_(transport), owner_user_id_(std::move(owner_user_id)) {}

LocalMultiDeviceSimulator::~LocalMultiDeviceSimulator() {
    // Workers stop their runtimes in their destructors; the explicit loop
    // keeps teardown order obvious.
    for (std::unique_ptr<LocalWorker>& worker : workers_) {
        worker->stop();
    }
}

vortyx::platform::Status LocalMultiDeviceSimulator::add_device(const SimulatorDeviceConfig& config,
                                                               bool& created, std::string& error) {
    // Validate the configuration FIRST (an invalid claim never reaches the
    // registry or spawns a worker).
    if (config.device_id.empty()) {
        error = "simulated device needs a device_id";
        return vortyx::platform::Status::InvalidInput;
    }
    if (!resource_vector_valid(config.capacity)) {
        error = "simulated device capacity must not contain negative fields";
        return vortyx::platform::Status::InvalidInput;
    }
    if (config.capacity.concurrent_jobs > config.max_concurrent_shards) {
        error = "max_concurrent_shards must cover the declared concurrent_jobs capacity";
        return vortyx::platform::Status::InvalidInput;
    }

    // Self-reported capabilities: backends come from an honest probe of
    // THIS host's runtime availability (a compiled-in but unusable
    // "vulkan" is never claimed — the same honesty rule the Scheduler
    // applies at selection time); operations and capacity from the
    // configuration.
    DeviceCapabilities capabilities;
    capabilities.metadata.protocol_version = vortyx::platform::kProtocolVersion;
    capabilities.metadata.software_version = VORTYX_VERSION_STRING;
    capabilities.metadata.display_name = config.display_name;
    capabilities.metadata.operations.reserve(config.operations.size());
    for (const vortyx::compute::ComputeOp op : config.operations) {
        capabilities.metadata.operations.push_back(vortyx::compute::workload_label(op));
    }
    {
        vortyx::compute::Runtime probe;
        if (probe.initialize() != vortyx::compute::Status::Ok) {
            error = "backend probe runtime failed to initialize";
            return vortyx::platform::Status::Internal;
        }
        for (const std::string& name : probe.backend_names()) {
            if (probe.has_backend(name)) capabilities.metadata.backends.push_back(name);
        }
        probe.shutdown();
    }
    capabilities.capacity = config.capacity;
    capabilities.max_concurrent_shards = config.max_concurrent_shards;

    // The registry decides fresh-vs-replay FIRST: an idempotent replay
    // must not spawn a second worker for the same device (one worker per
    // device — the transport refuses duplicates and the honest outcome is
    // simply Ok).
    DeviceDescriptor descriptor;
    const vortyx::platform::Status status =
        registry_.register_device(config.device_id, owner_user_id_, capabilities, descriptor,
                                  created);
    if (status != vortyx::platform::Status::Ok) {
        error = "registry registration failed";
        return status;
    }
    if (!created) {
        return vortyx::platform::Status::Ok;  // replay: the device already lives
    }

    // FRESH registration: the worker (and its EXCLUSIVE Runtime) is created
    // before activation so the record never describes a device that cannot
    // execute.
    std::unique_ptr<LocalWorker> worker =
        std::make_unique<LocalWorker>(config.device_id, config.operations);
    std::string start_error;
    if (worker->start(start_error) != vortyx::compute::Status::Ok) {
        // Roll the record back honestly: a registered device with no
        // worker would be a lie in the registry.
        registry_.unregister_device(owner_user_id_, config.device_id);
        error = "simulated device runtime failed to start: " + start_error;
        return vortyx::platform::Status::Internal;
    }

    // Activation: Registering -> Ready (the documented table), then a
    // first heartbeat stamps health Healthy (fresh evidence).
    if (registry_.update_device_state(owner_user_id_, config.device_id,
                                      DeviceState::Ready) != vortyx::platform::Status::Ok) {
        worker->stop();
        registry_.unregister_device(owner_user_id_, config.device_id);
        error = "device activation failed";
        return vortyx::platform::Status::Internal;
    }
    registry_.heartbeat_device(owner_user_id_, config.device_id);

    if (!transport_.attach(worker.get())) {
        worker->stop();
        registry_.unregister_device(owner_user_id_, config.device_id);
        error = "transport already has a worker for this device";
        return vortyx::platform::Status::Conflict;
    }
    workers_.push_back(std::move(worker));
    return vortyx::platform::Status::Ok;
}

std::vector<LocalWorker*> LocalMultiDeviceSimulator::workers() {
    std::vector<LocalWorker*> out;
    out.reserve(workers_.size());
    for (std::unique_ptr<LocalWorker>& worker : workers_) out.push_back(worker.get());
    return out;
}

}  // namespace vortyx::distributed
