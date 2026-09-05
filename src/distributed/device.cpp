// Device model implementation (Phase 12).

#include "distributed/device.hpp"

namespace vortyx::distributed {

const char* to_string(DeviceState state) {
    switch (state) {
        case DeviceState::Registering: return "registering";
        case DeviceState::Ready: return "ready";
        case DeviceState::Busy: return "busy";
        case DeviceState::Draining: return "draining";
        case DeviceState::Offline: return "offline";
        case DeviceState::Failed: return "failed";
    }
    return "unknown";
}

bool device_state_transition_valid(DeviceState from, DeviceState to) {
    switch (from) {
        case DeviceState::Registering:
            return to == DeviceState::Ready || to == DeviceState::Failed ||
                   to == DeviceState::Offline;
        case DeviceState::Ready:
            return to == DeviceState::Busy || to == DeviceState::Draining ||
                   to == DeviceState::Offline || to == DeviceState::Failed;
        case DeviceState::Busy:
            return to == DeviceState::Ready || to == DeviceState::Draining ||
                   to == DeviceState::Offline || to == DeviceState::Failed;
        case DeviceState::Draining:
            // A draining device finishes its work and leaves service; only
            // completion (offline) or failure ends the drain. Draining is
            // NOT reversible to Ready/Busy through the generic transition —
            // an owner aborts a drain by explicit re-registration.
            return to == DeviceState::Offline || to == DeviceState::Failed;
        case DeviceState::Offline:
            // Recovery requires proof: an explicit re-registration
            // (Registering) or a heartbeat that demonstrates liveness
            // (Ready — applied by the registry's heartbeat path only).
            return to == DeviceState::Registering || to == DeviceState::Ready;
        case DeviceState::Failed:
            // A failed device is retired (Offline) or explicitly
            // re-registered after remediation. Never silently Ready.
            return to == DeviceState::Registering || to == DeviceState::Offline;
    }
    return false;
}

bool device_state_schedulable(DeviceState state) {
    // Busy devices REMAIN placement candidates while they have free
    // capacity (that is the point of the resource model); everything else
    // is excluded from new placements.
    return state == DeviceState::Ready || state == DeviceState::Busy;
}

bool device_state_is_failure(DeviceState state) {
    return state == DeviceState::Failed;
}

const char* to_string(DeviceHealth health) {
    switch (health) {
        case DeviceHealth::Healthy: return "healthy";
        case DeviceHealth::Unhealthy: return "unhealthy";
        case DeviceHealth::Unknown: return "unknown";
    }
    return "unknown";
}

std::string DeviceCapabilities::preferred_backend() const {
    // Derived, deterministic: the first self-reported backend, or nothing.
    if (metadata.backends.empty()) return std::string();
    return metadata.backends.front();
}

Status validate_device_capabilities(const DeviceCapabilities& capabilities,
                                    std::string& error) {
    Status status = vortyx::platform::validate_device_metadata(capabilities.metadata, error);
    if (status != vortyx::platform::Status::Ok) return status;

    if (!resource_vector_valid(capabilities.capacity)) {
        error = "device capacity must not contain negative fields";
        return vortyx::platform::Status::InvalidInput;
    }
    if (capabilities.max_concurrent_shards < 0) {
        error = "max_concurrent_shards must not be negative";
        return vortyx::platform::Status::InvalidInput;
    }
    error.clear();
    return vortyx::platform::Status::Ok;
}

bool device_supports(const DeviceCapabilities& capabilities,
                     vortyx::compute::ComputeOp operation, const std::string& backend) {
    // Claims only: an empty list claims nothing and matches nothing.
    const std::string label = vortyx::compute::workload_label(operation);
    bool operation_claimed = false;
    for (const std::string& claimed : capabilities.metadata.operations) {
        if (claimed == label) {
            operation_claimed = true;
            break;
        }
    }
    if (!operation_claimed) return false;

    if (backend.empty()) return true;  // no preference: operation match suffices
    for (const std::string& claimed : capabilities.metadata.backends) {
        if (claimed == backend) return true;
    }
    return false;
}

}  // namespace vortyx::distributed
