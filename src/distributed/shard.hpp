#pragma once

// Job sharding (Phase 12) — one logical job, several deterministic shards.
//
// THE PARTITION MODEL (WorkPartition): a shard's work description is a
// WorkPartition — a tagged description of WHICH slice of the logical
// workload this shard executes. Phase 12 implements exactly one partition
// kind, ElementRange (a contiguous [begin, end) range of the elementwise
// data-parallel domain [0, element_count) documented on ComputeTask since
// Phase 10) — the honest partition for the operations the engine actually
// has. The kind tag exists so a future operation with a different
// partition shape adds its own kind instead of overloading the range; no
// partition kind is invented here for operations that do not exist.
//
// DETERMINISM + INVARIANTS (partition_element_count below, pinned by
// property-style tests):
//   - Same (element_count, shard_count) always yields the same plan.
//   - Ranges are contiguous, ascending, non-overlapping, and their union
//     is exactly [0, element_count): every element in exactly one shard.
//   - No zero-length shards: shard_count is capped at element_count (K > N
//     yields N one-element shards — splitting 3 elements over 8 devices
//     must not invent 5 empty shards; the placement layer sizes the
//     device set separately).
//   - Shard ids are deterministic: "<job_id>-s<index>" (index < shard_count,
//     plain decimal). The charset matches the platform id rules; a job_id
//     so long that its shard ids would exceed the id cap is refused
//     EXPLICITLY (never silently truncated).
//
// STATE MACHINE (shard level — deliberately separate from the Phase 11
// JobStatus and from the local TaskQueue's TaskState):
//
//   Pending -> Assigned -> Running -> Completed
//                |           |
//                |           +-> Retrying -> Assigned ...
//                |           +-> Failed (terminal)
//                |           +-> Cancelled (terminal)
//                +-> Cancelled
//   Failed / Cancelled / Completed are terminal.
//
// A Retrying shard returns to Assigned when a new placement is found;
// retry exhaustion moves it to Failed. The transition table is the pure
// function shard_state_transition_valid and nothing else may change a
// shard's state.

#include <cstdint>
#include <string>
#include <vector>

#include "platform/identity.hpp"  // JobId (reused)
#include "platform/status.hpp"

namespace vortyx::distributed {

using vortyx::platform::JobId;  // reused platform identity (see device.hpp)
using vortyx::platform::DeviceId;

// ---------------------------------------------------------------------------
// Work partition
// ---------------------------------------------------------------------------

// The partition kinds Phase 12 knows. Exactly one is implemented; the tag
// keeps the door open without pretending anything exists.
enum class PartitionKind {
    ElementRange,  // a contiguous [begin, end) slice of the element domain
};

const char* to_string(PartitionKind kind);

// A contiguous half-open range [begin, end) of the data-parallel domain.
// Invariant: begin < end (no empty shards), end <= element_count.
struct ElementRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;

    std::uint64_t size() const { return end - begin; }

    // Value equality (determinism checks compare plans/ranges directly).
    friend bool operator==(const ElementRange& a, const ElementRange& b) {
        return a.begin == b.begin && a.end == b.end;
    }
    friend bool operator!=(const ElementRange& a, const ElementRange& b) { return !(a == b); }
};

// The work description of one shard. Phase 12 always carries an
// ElementRange; other kinds would add their payload next to it (and their
// kind tag) — never smuggled through the range fields.
struct WorkPartition {
    PartitionKind kind = PartitionKind::ElementRange;
    ElementRange element_range;  // meaningful when kind == ElementRange
};

// Splits 'element_count' elements into at most 'shard_count' contiguous
// ranges. Deterministic. 'shard_count' must be >= 1 and element_count >= 1;
// the effective shard count is min(shard_count, element_count). Returns
// Status::InvalidInput ('error' filled) for a zero element_count, a zero
// shard_count, or an element_count/shard_count pair whose ranges could not
// be formed (cannot happen for the checked inputs — the check is the
// documented guard).
vortyx::platform::Status partition_element_count(std::uint64_t element_count,
                                                 std::uint32_t shard_count,
                                                 std::vector<ElementRange>& out_ranges,
                                                 std::string& error);

// The deterministic shard id of shard 'index' (0-based) of 'job_id'.
// Returns Status::InvalidInput when the derived id would exceed the
// platform id cap (kMaxIdLength) — refused, never truncated.
vortyx::platform::Status make_shard_id(const JobId& job_id, std::uint32_t index,
                                       std::string& out_id, std::string& error);

// True when 'candidate' is a syntactically valid derived shard id
// ("<job>-s<number>"). Used by tests and the contract layer to reject
// spoofed shard ids.
bool is_derived_shard_id(const JobId& job_id, const std::string& candidate);

// ---------------------------------------------------------------------------
// Shard state machine
// ---------------------------------------------------------------------------

enum class ShardState {
    Pending,    // created, not yet placed
    Assigned,   // placed on a device (lease held), not started
    Running,    // worker is executing it
    Completed,  // finished successfully (terminal)
    Failed,     // terminally failed: retries exhausted or non-retryable (terminal)
    Retrying,   // failed with retries remaining; waiting for re-placement
    Cancelled,  // cancelled before/while executing (terminal)
};

const char* to_string(ShardState state);

bool shard_state_is_terminal(ShardState state);

// The documented table (see the module header diagram). Pure function.
bool shard_state_transition_valid(ShardState from, ShardState to);

// ---------------------------------------------------------------------------
// Shard record
// ---------------------------------------------------------------------------

struct JobShard {
    std::string shard_id;      // deterministic ("<job>-s<index>")
    JobId parent_job_id;       // the logical job this shard belongs to
    std::uint32_t index = 0;   // shard position in the plan (ordering key)
    WorkPartition work;        // what to execute
    DeviceId assigned_device;  // current target ("" while Pending)
    std::string assigned_lease_id;  // the reservation holding capacity ("")

    ShardState state = ShardState::Pending;
    std::uint32_t attempt = 0;      // 0 = never dispatched; increments per execution
    std::uint32_t retry_count = 0;  // completed retries (attempts - 1)

    // Last failure, when any (observability; set on Retrying and Failed).
    std::string last_error;
    std::string last_failure_code;  // the stable FailureCode (retry.hpp)

    // The earliest time the NEXT attempt may start (orchestrator clock):
    // now + the retry policy's backoff for the attempt that just failed.
    // Observability of the backoff stamp; the synchronous Phase 12
    // executor re-places immediately and does not block on it (documented).
    std::int64_t next_attempt_eligible_ms = 0;
};

}  // namespace vortyx::distributed
