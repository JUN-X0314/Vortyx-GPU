#pragma once

// Distributed job state (Phase 12) — the LOGICAL job layer above shards.
//
// SEPARATION FROM EXISTING VOCABULARIES (no redefinition, no duplication):
//   - Phase 11 platform::JobStatus (queued/running/completed/failed/
//     cancelled) is the CONTROL-PLANE lifecycle and stays untouched. The
//     distributed layer maps onto it (map_to_platform_job_status below) —
//     it never extends or reinterprets it.
//   - The local TaskQueue's TaskState is the single-task local lifecycle —
//     unrelated, untouched.
//   - The shard state machine (shard.hpp) is the per-shard lifecycle.
//
// Phase 12 job states:
//
//   Queued ──> Planning ──> Scheduled ──> Running ──> Completed
//      │           │            │            │
//      └───────────┴────────────┴────────────┴──> Cancelled
//                                (Running ──> Failed)
//
//   Queued     — accepted, not yet planned.
//   Planning   — sharding/placement in progress (shards exist, unplaced or
//                mid-placement).
//   Scheduled  — every shard placed (lease held), execution not started.
//   Running    — at least one shard executing.
//   Completed  — every shard Completed (the only success).
//   Failed     — at least one shard terminally Failed. PARTIAL FAILURE IS
//                A FAILURE: "8 shards, 7 succeeded, 1 failed" is Failed
//                with the succeeded count carried honestly in the job's
//                DistributedResult — success is never faked by omitting a
//                state (the alternative, a separate PartiallyCompleted
//                state, would multiply the Phase 11 mapping for no
//                contract benefit; the aggregate result already says
//                exactly what happened).
//   Cancelled  — owner cancelled; no shard is Pending/Assigned/Running/
//                Retrying anymore (succeeded shards stay Completed).
//
// DERIVATION: the job status is COMPUTED from the shard states by
// derive_job_status (pure, deterministic, tested) — the job never drifts
// from its shards. The orchestrator applies the derived status through the
// transition table above (also enforced by derive-then-check).

#include <string>
#include <vector>

#include "distributed/shard.hpp"
#include "platform/job.hpp"  // platform::JobStatus (the control-plane target)
#include "platform/status.hpp"

namespace vortyx::distributed {

enum class DistributedJobStatus {
    Queued,
    Planning,
    Scheduled,
    Running,
    Completed,
    Failed,
    Cancelled,
};

const char* to_string(DistributedJobStatus status);

bool distributed_job_status_is_terminal(DistributedJobStatus status);

// The documented table (see the module header). Pure function.
bool distributed_job_transition_valid(DistributedJobStatus from, DistributedJobStatus to);

// Derives the job status from the shard states (pure; ignores the current
// job status — the orchestrator applies the result through the transition
// table, so an illegal derived transition is a loud error, not a silent
// overwrite). Deterministic rules:
//   - any shard Pending                          -> Planning
//   - else any Retrying                          -> Planning
//   - else any Assigned                          -> Scheduled
//   - else any Running                           -> Running
//   - else (every shard terminal)
//       any Failed                               -> Failed
//       any Cancelled                            -> Cancelled
//       else                                     -> Completed
//   - an empty shard list derives Queued (nothing has happened yet).
DistributedJobStatus derive_job_status(const std::vector<JobShard>& shards);

// The control-plane mapping (pure). Phase 11 JobStatus has no partial/
// planning states, so the mapping is the honest collapse:
//   Queued -> queued; Planning/Scheduled/Running -> running;
//   Completed -> completed; Failed -> failed; Cancelled -> cancelled.
vortyx::platform::JobStatus map_to_platform_job_status(DistributedJobStatus status);

}  // namespace vortyx::distributed
