#pragma once

// Device model (Phase 12) — one logical Vortyx compute node in a cluster.
//
// RELATIONSHIP TO PHASE 11 (no duplication): the IDENTITY of a device is
// vortyx::platform::DeviceId, and its SELF-DESCRIBED software surface
// (protocol version, software version, operating system, architecture,
// backend names, operation labels, display name) is
// vortyx::platform::DeviceMetadata — reused verbatim, never redefined here.
// What Phase 12 ADDS on top is what a scheduler needs to place work:
//   - a device STATE MACHINE (registration/liveness/drain/failure),
//   - a HEALTH classification (an explicit judgment, never a measurement),
//   - CAPACITY and ALLOCATION as ResourceVectors,
//   - the monotonic liveness timestamp (last heartbeat, from an IClock),
//   - the device's OWNER (a platform::UserId — ownership rules are the
//     platform layer's, unchanged).
//
// Capability honesty (Phase 11 rules carried forward):
//   - backends / operations lists are SELF-REPORTED and validated against
//     the platform vocabulary (is_known_backend / is_known_operation).
//     An empty list means "claims nothing" — the scheduler treats unknown
//     capability as UNSUITABLE, never as "probably fine" (unknown
//     capability is never guessed into support).
//   - capacity numbers are self-reported configuration (the local
//     simulator's inputs). Nothing here measures hardware; nothing
//     fabricates a value it does not know (no fake telemetry).
//   - no hardware fingerprint, no MAC address, no serial number — the
//     Phase 11 identity rules are unchanged.
//
// State machine (minimal set for a scheduler-facing registry; "unknown" is
// the state of an id that is NOT in the registry, so it is not an enum
// value — a lookup miss, not a stored state):
//
//   Registering -> Ready | Failed | Offline
//   Ready       -> Busy | Draining | Offline | Failed
//   Busy        -> Ready | Draining | Offline | Failed
//   Draining    -> Offline | Failed
//   Offline     -> Registering | Ready   (re-registration; or heartbeat
//                                          proof-of-life recovery)
//   Failed      -> Registering | Offline (explicit re-registration or
//                                          retirement)
//
// The table is the pure function device_state_transition_valid below; the
// registry enforces it on every state update. There is NO path from a
// terminal-ish state back to Ready without an explicit proof (re-registration
// or a heartbeat arriving) — a device never silently revives.

#include <cstdint>
#include <string>
#include <vector>

#include "distributed/clock.hpp"
#include "distributed/resource.hpp"
#include "platform/identity.hpp"   // DeviceId, UserId (reused, not redefined)
#include "platform/metadata.hpp"   // DeviceMetadata (reused, not redefined)
#include "platform/status.hpp"

namespace vortyx::distributed {

// The platform identity/ownership vocabulary is REUSED here (one source of
// truth); the using-declarations follow the Scheduler's (Phase 7) pattern
// for borrowing a sibling namespace's names.
using vortyx::platform::DeviceId;
using vortyx::platform::Status;
using vortyx::platform::UserId;

// ---------------------------------------------------------------------------
// Device lifecycle state
// ---------------------------------------------------------------------------

enum class DeviceState {
    Registering,  // accepted into the registry, not yet schedulable
    Ready,        // schedulable: healthy, idle, accepting assignments
    Busy,         // executing at least one shard (still schedulable up to
                  // its capacity limits; the state records observed work)
    Draining,     // finishing existing work; new placements refused
    Offline,      // not reporting / retired by the owner / drained out
    Failed,       // a device-level failure was reported (execution or liveness)
};

const char* to_string(DeviceState state);

// The documented transition table (pure function — see the module header).
bool device_state_transition_valid(DeviceState from, DeviceState to);

// True when the device may receive NEW shard placements. Draining, Offline,
// Failed and Registering devices are never placement candidates. Pure.
bool device_state_schedulable(DeviceState state);

// True when the state can no longer change by itself (registry removal is
// not a state transition — none of these states is truly terminal; Failed
// devices return through explicit re-registration).
bool device_state_is_failure(DeviceState state);

// ---------------------------------------------------------------------------
// Device health (an explicit classification, never a measurement)
// ---------------------------------------------------------------------------

enum class DeviceHealth {
    Healthy,    // judged usable (registration/heartbeat/execution evidence)
    Unhealthy,  // judged degraded: missed heartbeats or recent failures —
                // excluded from placement while unhealthy
    Unknown,    // no evidence either way (fresh registration); NOT treated
                // as healthy — unknown capability/health is never guessed
};

const char* to_string(DeviceHealth health);

// ---------------------------------------------------------------------------
// Capabilities (what the device claims it can do + what it can hold)
// ---------------------------------------------------------------------------

struct DeviceCapabilities {
    // The Phase 11 self-description, reused verbatim (protocol/software
    // versions, OS, architecture, display name, backends, operations).
    vortyx::platform::DeviceMetadata metadata;

    // Self-reported schedulable capacity (simulator config / explicit
    // configuration — never measured here).
    ResourceVector capacity;

    // How many shard executions this device accepts at once (the
    // ConcurrentJobs resource above is the schedulable view of the same
    // fact; this field is the device's own declaration the registry checks
    // reservation requests against). Kept as a separate named field so the
    // declaration and the accounting stay inspectable and testable.
    std::int64_t max_concurrent_shards = 0;

    // Preferred execution backend when a job expresses no preference: the
    // FIRST entry of metadata.backends (deterministic, self-reported). This
    // field is derived, not separately configured.
    std::string preferred_backend() const;
};

// Validates capabilities against the Phase 11 vocabulary:
//   - metadata must pass validate_device_metadata (protocol/software
//     versions, known backend/operation names, no duplicates, caps)
//   - capacity fields >= 0
//   - max_concurrent_shards >= 0 (0 = accepts no concurrent shard work)
Status validate_device_capabilities(const DeviceCapabilities& capabilities,
                                    std::string& error);

// True when this device claims it can execute 'operation' on 'backend'
// ("" backend = no preference: any claimed backend). Claims only — an empty
// operations list claims nothing and matches nothing. Pure.
bool device_supports(const DeviceCapabilities& capabilities,
                     vortyx::compute::ComputeOp operation, const std::string& backend);

// ---------------------------------------------------------------------------
// Device descriptor (registry state of one device)
// ---------------------------------------------------------------------------

struct DeviceDescriptor {
    DeviceId device_id;                 // platform identity, reused
    UserId owner_user_id;               // platform ownership, reused
    DeviceCapabilities capabilities;

    DeviceState state = DeviceState::Registering;
    DeviceHealth health = DeviceHealth::Unknown;

    // Resources currently RESERVED on this device (leases included).
    ResourceVector allocated;

    // Monotonic liveness stamp from the registry's clock (ms). Set at
    // registration, refreshed by heartbeat. Never fabricated: a device that
    // never reported carries the registration stamp only.
    std::int64_t last_heartbeat_ms = 0;
    std::int64_t registered_at_ms = 0;

    // Number of shards currently assigned/running (observability).
    std::int64_t running_shards = 0;
};

}  // namespace vortyx::distributed
