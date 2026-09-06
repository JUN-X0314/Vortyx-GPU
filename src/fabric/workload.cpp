// WorkloadDescriptor implementation (Phase 16) — see workload.hpp.

#include "fabric/workload.hpp"

#include "platform/metadata.hpp"  // is_known_backend (the contract vocabulary)

namespace vortyx::fabric {

Status validate_workload_descriptor(const WorkloadDescriptor& descriptor, std::string& error) {
    if (vortyx::platform::validate_id("workload_id", descriptor.workload_id, error) !=
        Status::Ok) {
        return Status::InvalidInput;
    }
    if (vortyx::platform::validate_id("owner_user_id", descriptor.owner_user_id, error) !=
        Status::Ok) {
        return Status::InvalidInput;
    }
    if (descriptor.element_count == 0) {
        error = "element_count must be > 0 (a zero-element workload has nothing to compute)";
        return Status::InvalidInput;
    }
    if (descriptor.element_count > vortyx::platform::kMaxJobElementCount) {
        error = "element_count exceeds the control-plane cap";
        return Status::InvalidInput;
    }
    if (descriptor.preferred_shard_count == 0 ||
        descriptor.preferred_shard_count > kMaxPreferredShardCount) {
        error = "preferred_shard_count must be in [1, " +
                std::to_string(kMaxPreferredShardCount) + "]";
        return Status::InvalidInput;
    }
    if (!descriptor.requested_backend.empty() &&
        !vortyx::platform::is_known_backend(descriptor.requested_backend)) {
        error = "requested_backend '" + descriptor.requested_backend +
                "' is not a canonical backend name";
        return Status::InvalidInput;
    }
    if (!descriptor.preferred_device.empty() &&
        vortyx::platform::validate_id("preferred_device", descriptor.preferred_device, error) !=
            Status::Ok) {
        return Status::InvalidInput;
    }
    for (const DeviceId& excluded : descriptor.excluded_devices) {
        if (vortyx::platform::validate_id("excluded_device", excluded, error) != Status::Ok) {
            return Status::InvalidInput;
        }
    }
    return Status::Ok;
}

WorkloadDescriptor derive_workload_descriptor(const JobEnvelope& envelope,
                                              const UserId& owner_user_id,
                                              std::uint32_t preferred_shard_count,
                                              bool allow_fallback) {
    WorkloadDescriptor descriptor;
    descriptor.workload_id = envelope.job_id;
    descriptor.owner_user_id = owner_user_id;
    descriptor.operation = envelope.operation;
    descriptor.element_count = envelope.element_count;
    descriptor.requested_backend = envelope.requested_backend;
    descriptor.priority = envelope.priority;  // Phase 16 consumes the reserved field
    descriptor.preferred_shard_count = preferred_shard_count;
    descriptor.allow_fallback = allow_fallback;
    // preferred_device / excluded_devices are caller context — an envelope
    // has no such fields; they stay empty in the derivation.
    return descriptor;
}

}  // namespace vortyx::fabric
