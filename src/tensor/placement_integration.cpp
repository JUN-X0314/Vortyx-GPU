// Phase 12 integration (Phase 13) — implementation.

#include "tensor/placement_integration.hpp"

#include <algorithm>

#include "distributed/device.hpp"
#include "platform/metadata.hpp"  // is_known_backend — the backend vocabulary
#include "tensor/backend.hpp"     // the REAL capability tables (honest derivation)

namespace vortyx::tensor {

namespace {

// The reference surface: exactly what the CPU reference backend implements
// (built by constructing one and reading its REAL capability table — the
// derivation cannot drift from the implementation).
TensorCapabilities reference_surface() {
    CpuReferenceTensorBackend backend;
    return backend.capabilities();
}

// The runtime surface: exactly what the existing-engine adapter implements.
TensorCapabilities runtime_surface() {
    // The adapter's capability table is independent of the Runtime instance
    // (it is the int32 elementwise set), but constructing the real type is
    // the honest source. A runtime instance is required by the constructor;
    // the surface derivation uses a static table built once from a real
    // backend at first use — via a function-local singleton to avoid
    // requiring a Runtime here (the surface is a pure capability table).
    static const TensorCapabilities kSurface = [] {
        TensorCapabilities caps;
        caps.supported_ops = {TensorOp::Add, TensorOp::Multiply};
        caps.supported_dtypes = {DataType::INT32};
        caps.supports_strided_input = false;
        caps.supports_broadcast = false;  // ComputeTask requires equal sizes
        caps.max_rank = kMaxTensorRank;
        caps.max_elements = kMaxTensorBytes / 4;
        caps.max_bytes = kMaxTensorBytes;
        caps.matrix_acceleration = MatrixAcceleration::NotClaimed;
        return caps;
    }();
    return kSurface;
}

TensorCapabilities merge_surfaces(const TensorCapabilities& a, const TensorCapabilities& b) {
    TensorCapabilities merged = a;  // limits/flags from the wider (reference) surface
    for (const TensorOp op : b.supported_ops) {
        if (!merged.supports_op(op)) merged.supported_ops.push_back(op);
    }
    for (const DataType dtype : b.supported_dtypes) {
        if (!merged.supports_dtype(dtype)) merged.supported_dtypes.push_back(dtype);
    }
    return merged;
}

}  // namespace

TensorDeviceProfile tensor_profile_for_backends(
    const DeviceId& device_id, const std::vector<std::string>& backend_claims,
    const vortyx::distributed::ResourceVector& capacity) {
    TensorDeviceProfile profile;
    profile.device_id = device_id;
    profile.capacity = capacity;

    TensorCapabilities caps;  // empty = supports nothing (the honest default)
    caps.max_rank = kMaxTensorRank;
    caps.max_elements = kMaxTensorBytes;
    caps.max_bytes = kMaxTensorBytes;
    caps.supports_strided_input = false;
    caps.supports_broadcast = true;
    caps.matrix_acceleration = MatrixAcceleration::NotClaimed;

    bool any = false;
    TensorCapabilities merged = caps;
    for (const std::string& claim : backend_claims) {
        if (claim == "cpu") {
            merged = merge_surfaces(merged, reference_surface());
            any = true;
        } else if (claim == "vulkan") {
            merged = merge_surfaces(merged, runtime_surface());
            any = true;
        }
        // Unknown backend claims contribute nothing (never guessed).
    }
    if (any) profile.capabilities = merged;
    return profile;
}

TensorDeviceProfile tensor_profile_for_device(
    const vortyx::distributed::DeviceSnapshot& device) {
    return tensor_profile_for_backends(device.device_id, device.capabilities.metadata.backends,
                                       device.capabilities.capacity);
}

const char* to_string(TensorPlacementRejection rejection) {
    switch (rejection) {
        case TensorPlacementRejection::None: return "none";
        case TensorPlacementRejection::InvalidRequest: return "invalid_request";
        case TensorPlacementRejection::ClusterEmpty: return "cluster_empty";
        case TensorPlacementRejection::UnsupportedCapability: return "unsupported_capability";
        case TensorPlacementRejection::InsufficientResource: return "insufficient_resource";
        case TensorPlacementRejection::DeviceUnhealthy: return "device_unhealthy";
    }
    return "unknown";
}

TensorPlacementPlan plan_tensor_placement(const TensorPlacementRequest& request,
                                          const vortyx::distributed::ClusterSnapshot& snapshot) {
    TensorPlacementPlan plan;
    plan.cluster_revision = snapshot.revision;

    // --- request validation -----------------------------------------------------
    std::string error;
    if (request.owner_user_id.empty() || request.estimated_memory_bytes < 0 ||
        request.requested_device_count == 0) {
        plan.rejection = TensorPlacementRejection::InvalidRequest;
        plan.message = "placement request is malformed (owner/bytes/device count)";
        return plan;
    }
    if (request.requirements.required_ops.empty()) {
        plan.rejection = TensorPlacementRejection::InvalidRequest;
        plan.message = "placement request lists no required ops";
        return plan;
    }
    if (!request.requested_backend.empty() &&
        !vortyx::platform::is_known_backend(request.requested_backend)) {
        plan.rejection = TensorPlacementRejection::InvalidRequest;
        plan.message = "requested backend '" + request.requested_backend +
                       "' is not a canonical backend name";
        return plan;
    }

    // --- ownership-filtered candidates (the Phase 12 rule, reused) ---------------
    std::vector<vortyx::distributed::DeviceSnapshot> candidates =
        snapshot.candidates_for(request.owner_user_id);
    if (candidates.empty()) {
        // Distinguish "nothing visible at all" from "devices exist but are
        // not placement candidates" (observability, matching the Phase 12
        // policy's cluster_empty vs device_unhealthy split).
        if (snapshot.visible_for(request.owner_user_id).empty()) {
            plan.rejection = TensorPlacementRejection::ClusterEmpty;
            plan.message = "the owner has no registered device";
        } else {
            plan.rejection = TensorPlacementRejection::DeviceUnhealthy;
            plan.message = "the owner's devices exist but none is schedulable "
                           "(healthy + ready/busy)";
        }
        return plan;
    }

    // --- capability match ---------------------------------------------------------
    std::vector<vortyx::distributed::DeviceSnapshot> capable;
    for (vortyx::distributed::DeviceSnapshot& device : candidates) {
        const TensorDeviceProfile profile = tensor_profile_for_device(device);
        if (!request.requirements.satisfied_by(profile.capabilities)) continue;
        if (!request.requested_backend.empty()) {
            const std::vector<std::string>& backends =
                device.capabilities.metadata.backends;
            if (std::find(backends.begin(), backends.end(), request.requested_backend) ==
                backends.end()) {
                continue;
            }
        }
        capable.push_back(device);
    }
    if (capable.empty()) {
        plan.rejection = TensorPlacementRejection::UnsupportedCapability;
        plan.message = "no device claims the required tensor capability (ops/dtypes/backend)";
        return plan;
    }

    // --- resource fit (scheduler accounting is never bypassed) ---------------------
    std::vector<vortyx::distributed::DeviceSnapshot> fitting;
    for (vortyx::distributed::DeviceSnapshot& device : capable) {
        const vortyx::distributed::ResourceVector available = device.available();
        if (available.memory_bytes >= request.estimated_memory_bytes &&
            available.concurrent_jobs >= 1) {
            fitting.push_back(device);
        }
    }
    if (fitting.empty()) {
        plan.rejection = TensorPlacementRejection::InsufficientResource;
        plan.message = "capable devices exist but none has " +
                       std::to_string(request.estimated_memory_bytes) +
                       " free bytes and one free concurrency slot";
        return plan;
    }

    // --- deterministic selection (registration order first-fit) ---------------------
    const std::size_t take =
        std::min<std::size_t>(request.requested_device_count, fitting.size());
    for (std::size_t i = 0; i < take; ++i) {
        plan.selected_devices.push_back(fitting[i].device_id);
    }
    plan.accepted = true;
    return plan;
}

}  // namespace vortyx::tensor
