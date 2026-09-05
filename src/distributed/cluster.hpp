#pragma once

// Cluster snapshot (Phase 12) — the immutable scheduling input.
//
// Schedulers never read the live registry while planning. They plan against
// a SNAPSHOT: a value copy of every device's placement-relevant state plus
// the cluster revision the snapshot was taken at. Two consequences:
//
//   1. Planning is a PURE function of (request, snapshot) — deterministic,
//      unit-testable without any registry, and lock-free for the registry.
//   2. A plan carries the revision it was based on. The orchestrator
//      compares that revision against the registry's CURRENT revision right
//      before executing the plan; a mismatch is a STALE PLAN — the cluster
//      changed under the planner (a device left, a lease was granted, ...).
//      The documented policy: re-plan a bounded number of times; refuse
//      with the stable stale-plan error when it keeps racing. Stale plans
//      are never force-executed.
//
// The snapshot is a plain value type (copyable, no identity): it describes
// the cluster, it is not the cluster.

#include <cstdint>
#include <string>
#include <vector>

#include "distributed/device.hpp"
#include "distributed/topology.hpp"

namespace vortyx::distributed {

using vortyx::platform::DeviceId;  // reused platform identity (see device.hpp)
using vortyx::platform::UserId;

// The placement-relevant view of one device (a projection of
// DeviceDescriptor — snapshots do not carry registry bookkeeping fields
// schedulers must not see, like lease records).
struct DeviceSnapshot {
    DeviceId device_id;
    UserId owner_user_id;
    DeviceCapabilities capabilities;

    DeviceState state = DeviceState::Registering;
    DeviceHealth health = DeviceHealth::Unknown;
    ResourceVector allocated;             // currently reserved resources
    std::int64_t last_heartbeat_ms = 0;   // registry-clock monotonic ms
    std::int64_t running_shards = 0;

    // Free capacity right now: capacity - allocated, field-wise. Computed
    // here so every consumer agrees on the definition.
    ResourceVector available() const;
};

// The immutable cluster view.
struct ClusterSnapshot {
    // Monotonically increasing revision of the source registry at snapshot
    // time (revision 0 = an empty, never-mutated registry).
    std::uint64_t revision = 0;

    std::vector<DeviceSnapshot> devices;  // registration order (deterministic)
    TopologyView topology;                // provider-neutral topology view

    // Placement-candidate devices: schedulable state, Healthy health,
    // owned by 'owner_user_id'. Ownership is enforced HERE — a scheduler
    // never even SEES another user's devices (the Phase 11 visibility rule
    // applied to the cluster). Order = registration order (deterministic).
    std::vector<DeviceSnapshot> candidates_for(const UserId& owner_user_id) const;

    // The whole visible cluster for this owner (any state), registration
    // order — the listing/observability view.
    std::vector<DeviceSnapshot> visible_for(const UserId& owner_user_id) const;

    // True when no device in the snapshot is a placement candidate for the
    // owner (the "cluster empty / nothing available" case).
    bool empty_for(const UserId& owner_user_id) const;
};

}  // namespace vortyx::distributed
