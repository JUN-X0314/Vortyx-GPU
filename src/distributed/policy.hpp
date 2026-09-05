#pragma once

// Scheduling policies and the placement engine (Phase 12).
//
// SEPARATION OF RESPONSIBILITIES (the boundary the spec demands):
//   - The POLICY answers "which device for this shard?" as a PURE function
//     of (PlacementRequest, ClusterSnapshot). No locks, no registry, no
//     I/O, no clock — the same inputs always produce the same plan.
//   - The PLACEMENT result (PlacementPlan) carries the cluster revision it
//     was computed from; the orchestrator owns the stale-plan check and
//     the reservation against the live registry.
//   - The policy knows NOTHING about workers, transports or backends'
//     internals. Execution is not its business.
//
// Rejections are explicit and carry a STABLE code (the contract vocabulary,
// lowercase snake_case like Phase 11's error codes):
//   invalid_request         — malformed placement request (zero elements,
//                             zero shards, unknown op...).
//   cluster_empty           — the owner has NO schedulable healthy device
//                             at all.
//   unsupported_capability  — devices exist but none claims the requested
//                             operation/backend (unknown capability is
//                             never guessed into support).
//   no_device_available     — capable devices exist but not enough for the
//                             requested shard count and fallback is off.
//   insufficient_resource   — capable devices exist but their remaining
//                             capacity cannot hold the shards.
//   device_unhealthy        — the only matching devices are unhealthy/
//                             draining/offline (informational refinement;
//                             the candidate filter already excluded them).
//   stale_plan              — (set by the orchestrator, not the policy) the
//                             cluster changed between plan and execution.
//
// Phase 12 ships three deterministic policies:
//   RoundRobin        — rotates through candidates in registration order
//                       (a per-policy cursor; deterministic given the same
//                       request sequence).
//   LeastLoaded       — fewest allocated concurrent jobs, then fewest
//                       allocated memory, then registration order. A
//                       CAPACITY-AWARE policy (self-reported allocations —
//                       NOT a measured-performance policy; no performance
//                       claims exist anywhere in Phase 12).
//   CapabilityFit     — best fit: the candidate whose remaining memory
//                       most closely covers the shard (smallest slack),
//                       tie-break registration order.
//
// Fallback policy (documented, tested): when the request asks for more
// shards than there are capable devices, an allowed fallback COALESCES to
// one shard per capable device (never more shards than devices, never more
// shards than elements — no empty shards); fallback disabled rejects with
// no_device_available. Fallback NEVER silently drops the multi-device
// requirement when the caller disabled it, and a single-device cluster
// with multi-device request simply places one shard on that device when
// fallback is allowed.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "distributed/cluster.hpp"
#include "distributed/resource.hpp"
#include "distributed/shard.hpp"
#include "platform/identity.hpp"

namespace vortyx::distributed {

using vortyx::platform::DeviceId;  // reused platform identity (see device.hpp)
using vortyx::platform::JobId;
using vortyx::platform::UserId;

// Stable rejection codes (see the module header).
enum class PlacementRejection {
    None,
    InvalidRequest,
    ClusterEmpty,
    UnsupportedCapability,
    NoDeviceAvailable,
    InsufficientResource,
    DeviceUnhealthy,
    StalePlan,
};

const char* to_string(PlacementRejection code);

// ---------------------------------------------------------------------------
// Request / plan
// ---------------------------------------------------------------------------

struct PlacementRequest {
    JobId job_id;
    UserId owner_user_id;   // placement is ownership-scoped end to end

    vortyx::compute::ComputeOp operation = vortyx::compute::ComputeOp::VectorAdd;
    std::string requested_backend;      // "" = device's own preference
    std::uint64_t element_count = 0;    // the data-parallel domain size
    std::uint32_t requested_shard_count = 1;
    bool allow_fallback = true;         // coalesce when devices are fewer

    // Devices the plan must NOT use (e.g. the device a shard just failed
    // on — a retry is re-placed ELSEWHERE when another device exists).
    std::vector<DeviceId> excluded_devices;
};

// One placed shard.
struct ShardPlan {
    std::uint32_t shard_index = 0;
    ElementRange range;                 // the work slice
    DeviceId device_id;                 // where it will run
    ResourceVector resources;           // what will be reserved for it
};

struct PlacementPlan {
    bool accepted = false;
    PlacementRejection rejection = PlacementRejection::None;
    std::string message;                // human-readable reason when rejected

    // The snapshot revision this plan is based on — the stale-plan check
    // compares this against the registry's current revision.
    std::uint64_t cluster_revision = 0;

    std::vector<ShardPlan> shards;      // ascending shard_index, always
};

// ---------------------------------------------------------------------------
// The policy interface
// ---------------------------------------------------------------------------

class ISchedulingPolicy {
public:
    virtual ~ISchedulingPolicy() = default;

    // Human-readable policy name ("round_robin", "least_loaded",
    // "capability_fit") — the configuration vocabulary.
    virtual const char* name() const = 0;

    // Computes the placement plan. PURE: no registry access, no clock, no
    // mutation of shared state. RoundRobin's cursor is policy-internal
    // state and the documented exception (see its class comment).
    virtual PlacementPlan plan(const PlacementRequest& request,
                               const ClusterSnapshot& snapshot) = 0;
};

// Builds a policy by configuration name. Returns nullptr for unknown names
// (configuration errors are refused, never defaulted silently).
std::unique_ptr<ISchedulingPolicy> make_scheduling_policy(const std::string& name);

// The policy names make_scheduling_policy accepts, in registration order.
const std::vector<std::string>& known_scheduling_policies();

// ---------------------------------------------------------------------------
// The three policies
// ---------------------------------------------------------------------------

class RoundRobinPolicy final : public ISchedulingPolicy {
public:
    const char* name() const override { return "round_robin"; }
    PlacementPlan plan(const PlacementRequest& request, const ClusterSnapshot& snapshot) override;

private:
    std::size_t cursor_ = 0;  // deterministic rotation state
};

class LeastLoadedPolicy final : public ISchedulingPolicy {
public:
    const char* name() const override { return "least_loaded"; }
    PlacementPlan plan(const PlacementRequest& request, const ClusterSnapshot& snapshot) override;
};

class CapabilityFitPolicy final : public ISchedulingPolicy {
public:
    const char* name() const override { return "capability_fit"; }
    PlacementPlan plan(const PlacementRequest& request, const ClusterSnapshot& snapshot) override;
};

// The shared placement skeleton (validation, capability filter, shard
// sizing, per-shard device choice through a selection callback). Exposed
// for the policy implementations and directly testable; NOT a fourth
// policy. 'pick' receives the capacity-filtered candidate list (in
// registration order) and the resource request for one shard, and returns
// the chosen index — or a value >= candidates.size() meaning "nothing fits".
PlacementPlan plan_with_picker(const PlacementRequest& request, const ClusterSnapshot& snapshot,
                               const std::function<std::size_t(
                                   const std::vector<DeviceSnapshot>& candidates,
                                   const ResourceVector& needed,
                                   const std::vector<DeviceId>& taken)>& pick);

}  // namespace vortyx::distributed
