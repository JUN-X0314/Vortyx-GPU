#pragma once

// WorkloadDescriptor (Phase 16) — the Adaptive Compute Fabric's workload
// metadata and intent representation.
//
// ═══════════════════════════════════════════════════════════════════════
// WHAT THIS IS (and is not): a WorkloadDescriptor is METADATA — which
// operation, how big, who owns it, what it prefers and what it refuses.
// It deliberately carries NO tensor payload, NO binary artifact, NO
// secret: the fabric plans work, it never transports data. The existing
// boundary vocabulary is REUSED, not redefined:
//
//   JobEnvelope (Phase 11)      — the control-plane submission contract
//   ClusterSnapshot (Phase 12)  — the placement input
//   DeviceCapabilities (Ph 12)  — the capability model (no new vocabulary)
//   ComputeOp (Phase 10)        — the executable operation vocabulary
//
// A descriptor can be derived DETERMINISTICALLY from a JobEnvelope plus
// the submitter identity (derive_workload_descriptor below) — the fabric
// is an additive planning layer over the existing contract, never a
// replacement for it.
//
// HONEST RESOURCE ESTIMATION: the per-shard memory requirement is not a
// descriptor field — it is COMPUTED at planning time with the Phase 12
// shard_memory_bytes rule (the one definition every layer already agrees
// on). Storing a second "estimated bytes" field would create two ways to
// disagree; deriving it leaves none.
//
// PRIORITY (minimal Phase 16 semantics): the descriptor carries a signed
// priority. Higher values are planned EARLIER among topologically ready
// nodes and win deterministic tie-breaks — nothing more. This is NOT a
// fairness/starvation-free scheduler claim; it is a planning-order
// preference, documented as such (see docs/fabric/planning.md). The
// JobEnvelope's reserved priority field flows through verbatim; Phase 16
// is the "later phase defines real semantics" that Phase 11 anticipated
// (the envelope comment documents exactly this consumer).
// ═══════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <vector>

#include "core/compute/task.hpp"   // ComputeOp — the shared op vocabulary
#include "platform/identity.hpp"   // DeviceId/JobId/UserId (reused)
#include "platform/job.hpp"        // JobEnvelope (reused, not redefined)
#include "platform/status.hpp"

namespace vortyx::fabric {

using vortyx::platform::DeviceId;
using vortyx::platform::JobEnvelope;
using vortyx::platform::Status;
using vortyx::platform::UserId;

// The default planning priority (the JobEnvelope default flows through).
inline constexpr std::int32_t kDefaultWorkloadPriority = 0;

// The shard-count preference bound (mirrors the service-level cap — one
// named bound, not a knob; the service may cap further).
inline constexpr std::uint32_t kMaxPreferredShardCount = 64;

struct WorkloadDescriptor {
    // Unique within a WorkloadGraph (validated there); syntactically a
    // platform id (is_valid_id) so it is safe in JSON, logs and URLs.
    std::string workload_id;

    // The owner whose devices are placement candidates (ownership-scoped
    // planning end to end — the Phase 12 visibility rule, reused).
    UserId owner_user_id;

    // The executable operation (the Phase 10/12 vocabulary only — an
    // operation no device can claim is planned as unsupported_capability,
    // never guessed into support).
    vortyx::compute::ComputeOp operation = vortyx::compute::ComputeOp::VectorAdd;

    // The data-parallel domain size [0, element_count). Validated > 0.
    std::uint64_t element_count = 0;

    // Backend preference ("" = the device's own preference decides).
    // Validated against the canonical backend vocabulary like the envelope.
    std::string requested_backend;

    // Planning-order preference (see the module header). NOT a scheduler
    // fairness claim.
    std::int32_t priority = kDefaultWorkloadPriority;

    // How many shards the caller would like (>= 1; capped). The planner
    // may coalesce when fewer capable devices exist ONLY when the caller
    // allows it — the same documented fallback rule as Phase 12 placement.
    std::uint32_t preferred_shard_count = 1;
    bool allow_fallback = true;

    // Locality hint (optional, honest): the device whose data is ALREADY
    // resident, when the caller actually knows one. It contributes a
    // deterministic bonus to that device's score — a metadata-based
    // preference, NOT a measured transfer cost (no cross-device transfer
    // engine exists; see docs/fabric/planning.md). Empty = no hint.
    DeviceId preferred_device;

    // Hard placement constraints: these devices are never candidates
    // (the retry re-placement path's "not the device that just failed").
    std::vector<DeviceId> excluded_devices;
};

// Validates a descriptor. Status::Ok, or Status::InvalidInput with 'error':
//   - invalid workload_id or owner_user_id (platform id rules)
//   - element_count == 0 or above the control-plane cap (kMaxJobElementCount)
//   - preferred_shard_count == 0 or above kMaxPreferredShardCount
//   - requested_backend non-empty but not a canonical backend name
//   - preferred_device non-empty but not a valid device id
//   - an excluded device id that is not a valid device id
Status validate_workload_descriptor(const WorkloadDescriptor& descriptor, std::string& error);

// Deterministic derivation from the existing submission contract: the
// envelope's id/operation/size/backend/priority flow through verbatim; the
// shard preference, fallback policy, locality hint and exclusions come
// from the caller (they are submission-context, not envelope, fields).
// Pure function of its inputs.
WorkloadDescriptor derive_workload_descriptor(const JobEnvelope& envelope,
                                              const UserId& owner_user_id,
                                              std::uint32_t preferred_shard_count,
                                              bool allow_fallback);

}  // namespace vortyx::fabric
