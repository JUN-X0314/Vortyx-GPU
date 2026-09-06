#pragma once

// Platform job model (Phase 11) — the control-plane contract for one unit of
// remotely-managed compute work.
//
// ═══════════════════════════════════════════════════════════════════════
// THE BOUNDARY THAT MUST NOT BLUR (Phase 11 core rule):
//
//   vortyx::compute::ComputeTask   — LOCAL execution semantics. Carries the
//                                    actual input data (a, b, scalar) and
//                                    runs through the unchanged local path
//                                    (Virtual GPU → Runtime → Backend).
//
//   vortyx::platform::JobEnvelope  — REMOTE TRANSPORT semantics. A
//                                    provider-neutral description of work
//                                    submitted to a control plane: WHICH
//                                    operation, HOW BIG, who wants it —
//                                    and deliberately NO data payload.
//                                    ComputeTask is never serialized into
//                                    a JobEnvelope and never travels the
//                                    network in Phase 11.
//
// The two types share exactly one thing: the operation VOCABULARY
// (vortyx::compute::ComputeOp / workload_label), so the op names cannot
// drift between the engine and the contract. Everything else stays separate
// on purpose. When Phase 13 partitions a task, the partitioning happens on
// the local side; the platform contract only ever sees job-level metadata.
// ═══════════════════════════════════════════════════════════════════════
//
// Job lifecycle (CONTROL PLANE — deliberately NOT the local TaskQueue's
// TaskState): the Phase 6 TaskState is the lifecycle of one task inside one
// local FIFO queue (Invalid/Queued/Running/Completed/Failed, no Cancelled —
// documented there). JobStatus below is the lifecycle of a remotely
// submitted job and DOES have Cancelled, because cancellation is a
// control-plane operation the local queue does not implement and has never
// claimed. Neither enum maps onto the other and neither is changed here.
//
//   Queued ──▶ Running ──▶ Completed
//      │          │  └────▶ Failed
//      └──────────┴───────▶ Cancelled
//
// Valid transitions (enforced by the store, the API layer and Supabase-side
// expectations alike):
//   Queued  -> Running, Cancelled
//   Running -> Completed, Failed, Cancelled
//   Completed / Failed / Cancelled : terminal, no further transitions.
// The transition table is a pure function (job_status_transition_valid) so
// every layer can agree on it and tests can pin it.

#include <cstdint>
#include <optional>
#include <string>

#include "core/compute/task.hpp"  // ComputeOp — the shared op vocabulary only
#include "platform/identity.hpp"  // JobId
#include "platform/metadata.hpp"  // kProtocolVersion
#include "platform/status.hpp"

namespace vortyx::platform {

// ---------------------------------------------------------------------------
// Clock helper
// ---------------------------------------------------------------------------

// Current wall-clock time as epoch milliseconds (UTC). Used by stores to
// stamp server-side timestamps; optional<int64_t> everywhere means "not set
// yet / not reported" — a missing timestamp is never written as a fake 0.
std::int64_t now_epoch_ms();

// ---------------------------------------------------------------------------
// Job status
// ---------------------------------------------------------------------------

enum class JobStatus {
    Queued,     // accepted by the control plane, not started
    Running,    // picked up for execution (by a device agent — Phase 12+)
    Completed,  // finished successfully
    Failed,     // finished with an error (the error text is required)
    Cancelled,  // cancelled before completion (owner action)
};

const char* to_string(JobStatus status);

// True when the status can no longer change.
bool job_status_is_terminal(JobStatus status);

// The documented transition table (pure function):
//   Queued -> Running | Cancelled; Running -> Completed | Failed |
//   Cancelled; terminal states allow nothing.
bool job_status_transition_valid(JobStatus from, JobStatus to);

// ---------------------------------------------------------------------------
// Job envelope (submission contract)
// ---------------------------------------------------------------------------

struct JobEnvelope {
    // Client-generated unique id — the idempotency key of the submission
    // (resubmitting the same id with the same payload never creates a second
    // job; resubmitting it with a DIFFERENT payload is a Conflict).
    JobId job_id;

    // Which operation to run — the shared Phase 10 vocabulary (labels:
    // "vector_add", "vector_multiply", "vector_scale"). The payload data
    // itself is NOT part of the envelope (see the module documentation).
    vortyx::compute::ComputeOp operation = vortyx::compute::ComputeOp::VectorAdd;

    // Size of the data-parallel domain [0, element_count) this job refers
    // to. Metadata only — the control plane never sees or stores the data
    // in Phase 11. Must be > 0 (a zero-element job has nothing to compute —
    // the same refusal rule as the local ComputeTask).
    std::uint64_t element_count = 0;

    // Explicit backend request. Empty = no preference expressed. A non-empty
    // value must be a canonical backend name ("cpu", "vulkan"). Phase 11 has
    // no remote executor: the requested backend is RECORDED, never silently
    // remapped, and honoring/refusing it is Phase 12+ execution policy.
    std::string requested_backend;

    // Phase 16 defines the real semantics this field was reserved for: the
    // Adaptive Compute Fabric's planner consumes the numeric value as a
    // PLANNING-ORDER preference (higher values plan earlier among
    // topologically ready nodes; see src/fabric/workload.hpp and
    // docs/fabric/planning.md). It is a deterministic tie-breaking input —
    // NOT a fairness/starvation-free scheduling guarantee. The value is
    // still carried verbatim everywhere; consumers outside the fabric
    // continue to ignore it.
    std::int32_t priority = 0;

    // The control-plane protocol this submission speaks (kProtocolVersion).
    std::string protocol_version = kProtocolVersion;

    // Client-reported creation time (epoch ms). Optional; the store always
    // records its own server-side created_at regardless.
    std::optional<std::int64_t> created_at_ms;
};

// Validates an envelope. Status::Ok, or Status::InvalidInput with 'error':
//   - invalid job_id (syntax — see identity.hpp)
//   - element_count == 0 or above the documented cap (kMaxJobElementCount)
//   - requested_backend non-empty but not a canonical backend name
//   - protocol_version != kProtocolVersion
// 'priority' is reserved and intentionally carries no validation policy
// beyond its type.
Status validate_job_envelope(const JobEnvelope& envelope, std::string& error);

// Upper bound for element_count in the CONTROL-PLANE contract. The current
// engine operates on int32 element domains, so a job beyond the int32 range
// could never be executed honestly; accepting it now would sell a promise
// the engine cannot keep. (Larger domains become possible when a future
// phase widens the engine — then this constant changes WITH it.)
inline constexpr std::uint64_t kMaxJobElementCount = 2147483647ULL;

// ---------------------------------------------------------------------------
// Result envelope (execution outcome contract)
// ---------------------------------------------------------------------------

// Provider-neutral description of one job's execution OUTCOME. Metadata
// only — no result payload storage or streaming exists in Phase 11.
struct ResultEnvelope {
    JobId job_id;

    // Terminal outcome recorded by the executor: Completed or Failed
    // (Cancellation is an owner action through the cancel path, never a
    // result; Queued/Running are not outcomes).
    JobStatus status = JobStatus::Failed;

    // Backend the job (would be) executed on. Honest optional: empty means
    // the executor did not report one — never filled in by guesswork.
    std::string backend;

    // Human-readable failure reason. Required when status == Failed; must
    // be empty when Completed.
    std::string error;

    // Size of the produced result domain, when known (for the current
    // elementwise ops: equal to the job's element_count). Optional metadata.
    std::optional<std::uint64_t> result_element_count;
};

// Validates a result envelope. Status::Ok, or Status::InvalidInput:
//   - status must be Completed or Failed (anything else is not an outcome)
//   - a Failed result REQUIRES a non-empty error (a failure without a
//     reason is a hidden failure — forbidden by the project honesty rules)
//   - a Completed result must NOT carry an error string
//   - backend, when non-empty, must be a canonical backend name
Status validate_result_envelope(const ResultEnvelope& envelope, std::string& error);

}  // namespace vortyx::platform
