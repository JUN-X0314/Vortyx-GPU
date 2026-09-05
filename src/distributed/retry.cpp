// Failure classification and retry policy implementation (Phase 12).

#include "distributed/retry.hpp"

namespace vortyx::distributed {

const char* to_string(FailureCode code) {
    switch (code) {
        case FailureCode::None: return "none";
        case FailureCode::WorkerExecutionFailed: return "worker_execution_failed";
        case FailureCode::DeviceLost: return "device_lost";
        case FailureCode::LeaseExpired: return "lease_expired";
        case FailureCode::ShardTimeout: return "shard_timeout";
        case FailureCode::Cancelled: return "cancelled";
        case FailureCode::InvalidAssignment: return "invalid_assignment";
        case FailureCode::DuplicateResult: return "duplicate_result";
    }
    return "unknown";
}

bool failure_code_from_string(const std::string& name, FailureCode& out) {
    if (name == "none") { out = FailureCode::None; return true; }
    if (name == "worker_execution_failed") { out = FailureCode::WorkerExecutionFailed; return true; }
    if (name == "device_lost") { out = FailureCode::DeviceLost; return true; }
    if (name == "lease_expired") { out = FailureCode::LeaseExpired; return true; }
    if (name == "shard_timeout") { out = FailureCode::ShardTimeout; return true; }
    if (name == "cancelled") { out = FailureCode::Cancelled; return true; }
    if (name == "invalid_assignment") { out = FailureCode::InvalidAssignment; return true; }
    if (name == "duplicate_result") { out = FailureCode::DuplicateResult; return true; }
    return false;
}

bool is_retryable(FailureCode code) {
    switch (code) {
        case FailureCode::WorkerExecutionFailed:
        case FailureCode::DeviceLost:
        case FailureCode::LeaseExpired:
        case FailureCode::ShardTimeout: return true;
        case FailureCode::None:
        case FailureCode::Cancelled:
        case FailureCode::InvalidAssignment:
        case FailureCode::DuplicateResult: return false;
    }
    return false;
}

bool is_failure(FailureCode code) {
    return code != FailureCode::None && code != FailureCode::DuplicateResult;
}

std::int64_t retry_delay_ms(const RetryPolicy& policy, std::uint32_t failed_attempt) {
    std::int64_t delay = policy.backoff_base_ms > 0 ? policy.backoff_base_ms : 0;
    if (failed_attempt >= 1) {
        // delay = base * 2^(failed_attempt - 1), with an overflow-safe clamp.
        const std::uint32_t shifts = failed_attempt - 1;
        for (std::uint32_t i = 0; i < shifts && delay < kMaxBackoffMs; ++i) {
            delay *= 2;
        }
    }
    return delay > kMaxBackoffMs ? kMaxBackoffMs : delay;
}

bool retry_permitted(const RetryPolicy& policy, std::uint32_t failed_attempts) {
    if (policy.max_attempts < 1) return false;  // no attempt was allowed at all
    return failed_attempts < policy.max_attempts;
}

}  // namespace vortyx::distributed
