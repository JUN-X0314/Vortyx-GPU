#pragma once

// Failure classification and retry policy (Phase 12).
//
// FailureCode is the STABLE failure vocabulary of the distributed layer
// (lowercase snake_case, matching the Phase 11 contract error-code style).
// Every failed shard carries the code of WHY it failed; retry decisions
// and observability read codes, never error strings.
//
// The classifier (is_retryable) is a pure, documented function:
//   worker_execution_failed — the backend/runtime refused or failed the
//                             execution (e.g. requested backend unavailable
//                             on that device). RETRYABLE: another device
//                             may genuinely be able to run it, and the
//                             input data is intact.
//   device_lost             — the device went Offline/Failed/Unhealthy.
//                             RETRYABLE elsewhere (the classic case for
//                             re-placement).
//   lease_expired           — the reservation lapsed before execution
//                             started. RETRYABLE (re-place; the capacity
//                             was lost, the work was not done).
//   shard_timeout           — the execution exceeded its deadline and was
//                             abandoned. RETRYABLE (bounded by the policy).
//   cancelled               — owner-initiated cancellation. NOT retryable.
//   invalid_assignment      — the assignment did not validate (range
//                             mismatch, unknown device, foreign lease).
//                             NOT retryable as-is: it is a planner/worker
//                             contract bug, and retrying a deterministic
//                             bug deterministically repeats it.
//   duplicate_result        — a result for this shard was already
//                             recorded. NOT a failure of the work; recorded
//                             so duplicates are VISIBLE and never
//                             double-counted (aggregation idempotency).
//
// RetryPolicy: max_attempts (total, INCLUDING the first — "max_retries: 3"
// means at most 4 attempts is WRONG here by definition; the policy says
// attempts, tests pin it), the classifier above, and an exponential backoff
// whose delay is a pure function of the attempt number (retry_delay_ms) —
// evaluated against the injected clock by the caller, never by sleeping.
// INFINITE RETRY IS IMPOSSIBLE: the orchestrator stops at max_attempts and
// the policy type has no "unbounded" representation.

#include <cstdint>
#include <string>

namespace vortyx::distributed {

enum class FailureCode {
    None,                    // no failure (success)
    WorkerExecutionFailed,   // backend/runtime execution error
    DeviceLost,              // device offline/unhealthy/failed mid-flight
    LeaseExpired,            // reservation lapsed before/at execution
    ShardTimeout,            // execution deadline exceeded
    Cancelled,               // owner cancellation
    InvalidAssignment,       // assignment failed validation
    DuplicateResult,         // a result for this shard already exists
};

const char* to_string(FailureCode code);

// Parses a stable code name (the to_string spelling). False for unknown
// names (codes crossing a boundary are validated, never guessed).
bool failure_code_from_string(const std::string& name, FailureCode& out);

// The documented classifier. Pure function.
bool is_retryable(FailureCode code);

// True when the code represents a real failed attempt (everything except
// None and DuplicateResult). Pure.
bool is_failure(FailureCode code);

// ---------------------------------------------------------------------------
// Retry policy
// ---------------------------------------------------------------------------

struct RetryPolicy {
    // TOTAL execution attempts allowed per shard, including the first.
    // Minimum 1 (a single attempt, no retries). No unbounded mode exists.
    std::uint32_t max_attempts = 3;

    // Backoff base for attempt n (n counted from 1, the attempt that just
    // failed): delay = base_ms * 2^(n-1), capped at kMaxBackoffMs. The
    // orchestrator waits this long on the injected clock before the next
    // attempt — in tests the clock jumps, so backoff never costs real time.
    std::int64_t backoff_base_ms = 10;

    // The retryable/non-retryable classifier to use (the documented one
    // above by default; a test may inject a stricter one through a
    // subclass-free route: a policy with classify == nullptr uses the
    // documented classifier).
    // (Pointer keeps the struct a plain value type; nullptr = default.)
    bool (*classify)(FailureCode) = nullptr;
};

// The documented backoff: base_ms << (attempt - 1), clamped to
// kMaxBackoffMs, for attempt >= 1; attempt < 1 yields base_ms.
std::int64_t retry_delay_ms(const RetryPolicy& policy, std::uint32_t failed_attempt);

// True when another attempt is allowed after 'failed_attempts' failed
// attempts ('policy.max_attempts' is the ceiling). Pure.
bool retry_permitted(const RetryPolicy& policy, std::uint32_t failed_attempts);

inline constexpr std::int64_t kMaxBackoffMs = 60000;

}  // namespace vortyx::distributed
