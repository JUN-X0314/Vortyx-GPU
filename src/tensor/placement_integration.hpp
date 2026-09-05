#pragma once

// Phase 12 integration — tensor placement over a real cluster (Phase 13).
//
// THE CONNECTION (read-only over Phase 12's own vocabulary — the distributed
// layer is not modified): a TensorGraph / single tensor op is a LOGICAL AI
// workload; the Phase 12 cluster snapshot is the placement input; the
// scheduler's job is to select a device whose declared capabilities can run
// the workload. Phase 13 does NOT partition one graph across devices and
// does NOT move tensors between devices — both are explicit non-goals
// (documented in docs/tensor/); what exists is the capability-based
// placement BASIS the spec asks for:
//
//   - TensorDeviceProfile: what one device can honestly do for tensor work.
//     Derived by tensor_profile_for_backends() from the device's OWN backend
//     claims (DeviceMetadata.backends — the Phase 11 self-description):
//       a device claiming "cpu"   gets the reference kernel surface (what
//                                 Vortyx 0.13's software really executes);
//       a device claiming "vulkan" additionally gets the int32 elementwise
//                                 surface routed through the REAL engine.
//     Nothing else is derived: no hardware Tensor Core claims, no fabricated
//     memory numbers (memory capacity stays the device's self-reported
//     ResourceVector from Phase 12).
//
//   - plan_tensor_placement(): a PURE function over (request, snapshot) —
//     the Phase 12 policy pattern: ownership-filtered candidates (the
//     snapshot's candidates_for, reused verbatim), capability match,
//     resource fit, deterministic first-fit by registration order, stable
//     rejection codes, and the snapshot revision carried into the plan for
//     stale-plan detection (the caller re-checks the registry revision —
//     the Phase 12 orchestrator discipline, unchanged).
//
//   - A selected device's execution goes through its OWN executor context
//     (the simulator/test rig wires one TensorExecutor per device); this
//     module never touches device internals.

#include <string>
#include <vector>

#include "distributed/cluster.hpp"  // ClusterSnapshot (Phase 12, reused)
#include "distributed/resource.hpp" // ResourceVector (Phase 12, reused)
#include "platform/identity.hpp"    // DeviceId / UserId (Phase 11, reused)
#include "tensor/capability.hpp"
#include "tensor/status.hpp"

namespace vortyx::tensor {

using vortyx::platform::DeviceId;  // reused (one identity system)
using vortyx::platform::UserId;

// What one device offers for tensor work (see the module header for the
// derivation rules).
struct TensorDeviceProfile {
    DeviceId device_id;
    TensorCapabilities capabilities;
    vortyx::distributed::ResourceVector capacity;  // the device's self-reported capacity
};

// Derives the honest tensor profile from a device's backend claims.
// Unknown/absent claims produce an EMPTY capability set (supports nothing —
// unknown capability is never guessed into support, the Phase 12 rule).
TensorDeviceProfile tensor_profile_for_backends(const DeviceId& device_id,
                                                const std::vector<std::string>& backend_claims,
                                                const vortyx::distributed::ResourceVector& capacity);

// Convenience: derive from a full Phase 12 snapshot entry.
TensorDeviceProfile tensor_profile_for_device(
    const vortyx::distributed::DeviceSnapshot& device);

// ---------------------------------------------------------------------------
// Placement request / plan
// ---------------------------------------------------------------------------

struct TensorPlacementRequest {
    UserId owner_user_id;                 // placement is ownership-scoped (Phase 12 rule)

    TensorRequirements requirements;      // ops/dtypes/rank/bytes the workload needs

    // Estimated peak tensor bytes for the workload (inputs + outputs + the
    // memory plan's intermediates when known). Checked against each device's
    // FREE memory (capacity - allocated) — scheduler resource accounting is
    // never bypassed (Phase 12 rule).
    std::int64_t estimated_memory_bytes = 0;

    // Optional explicit backend request ("" = no preference). A non-empty
    // value must be among the device's claimed backends.
    std::string requested_backend;

    // How many distinct devices the caller wants (Phase 13 executes the
    // graph on ONE device; values > 1 are planned as candidate LISTS for a
    // future distributed phase — the plan carries the ordered candidates).
    std::uint32_t requested_device_count = 1;
};

// Stable rejection codes (snake_case, mirroring the Phase 12 placement
// vocabulary style — a separate tensor-layer enum, not a copy of values).
enum class TensorPlacementRejection {
    None,
    InvalidRequest,          // malformed request (empty requirements, negative bytes)
    ClusterEmpty,            // the owner has no schedulable healthy device at all
    UnsupportedCapability,   // devices exist but none can run the required ops/dtypes
    InsufficientResource,    // capable devices exist without enough free memory
    DeviceUnhealthy,         // only unhealthy/draining/offline devices matched
};

const char* to_string(TensorPlacementRejection rejection);

struct TensorPlacementPlan {
    bool accepted = false;
    TensorPlacementRejection rejection = TensorPlacementRejection::None;
    std::string message;  // human-readable reason when rejected (never empty on failure)

    // The snapshot revision the plan is based on — stale-plan detection is
    // the CALLER's re-check against the registry (the Phase 12 discipline).
    std::uint64_t cluster_revision = 0;

    // The ordered candidate devices (registration order; the first entry is
    // THE placement for Phase 13 execution). Empty when rejected.
    std::vector<DeviceId> selected_devices;
};

// Pure placement planner (see the module header). Deterministic.
TensorPlacementPlan plan_tensor_placement(const TensorPlacementRequest& request,
                                          const vortyx::distributed::ClusterSnapshot& snapshot);

}  // namespace vortyx::tensor
