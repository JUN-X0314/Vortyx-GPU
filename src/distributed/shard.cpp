// Job sharding implementation (Phase 12).

#include "distributed/shard.hpp"

#include "platform/identity.hpp"  // kMaxIdLength, is_valid_id

namespace vortyx::distributed {

const char* to_string(PartitionKind kind) {
    switch (kind) {
        case PartitionKind::ElementRange: return "element_range";
    }
    return "unknown";
}

vortyx::platform::Status partition_element_count(std::uint64_t element_count,
                                                 std::uint32_t shard_count,
                                                 std::vector<ElementRange>& out_ranges,
                                                 std::string& error) {
    if (element_count == 0) {
        error = "cannot partition an empty element domain";
        return vortyx::platform::Status::InvalidInput;
    }
    if (shard_count == 0) {
        error = "shard_count must be at least 1";
        return vortyx::platform::Status::InvalidInput;
    }

    // Cap: no zero-length shards, ever. K > N yields N one-element shards.
    const std::uint64_t effective =
        shard_count < element_count ? static_cast<std::uint64_t>(shard_count) : element_count;

    out_ranges.clear();
    out_ranges.reserve(static_cast<std::size_t>(effective));

    // Contiguous, non-overlapping, exact-coverage split. The standard
    // balanced split: shard i covers [i*n/k, (i+1)*n/k) — rounding is
    // absorbed inside the ranges, so coverage is exact and no shard is
    // empty (effective <= element_count guarantees i*n/k < n for every i).
    for (std::uint64_t i = 0; i < effective; ++i) {
        ElementRange range;
        range.begin = (i * element_count) / effective;
        range.end = ((i + 1) * element_count) / effective;
        out_ranges.push_back(range);
    }

    error.clear();
    return vortyx::platform::Status::Ok;
}

vortyx::platform::Status make_shard_id(const JobId& job_id, std::uint32_t index,
                                       std::string& out_id, std::string& error) {
    vortyx::platform::Status status = vortyx::platform::validate_id("job_id", job_id, error);
    if (status != vortyx::platform::Status::Ok) return status;

    out_id = job_id + "-s" + std::to_string(index);
    if (out_id.size() > vortyx::platform::kMaxIdLength) {
        error = "derived shard id exceeds the id length cap; use a shorter job_id";
        return vortyx::platform::Status::InvalidInput;
    }
    error.clear();
    return vortyx::platform::Status::Ok;
}

bool is_derived_shard_id(const JobId& job_id, const std::string& candidate) {
    // Valid platform id, starts with "<job>-s", and the remainder is a
    // plain non-negative decimal without leading '+/-'. Re-deriving for
    // every index is unnecessary: the shape check plus the prefix equality
    // is the contract (tests additionally pin exact derivation).
    if (candidate.size() <= job_id.size() + 2) return false;
    if (!vortyx::platform::is_valid_id(candidate)) return false;
    if (candidate.compare(0, job_id.size() + 2, job_id + "-s") != 0) return false;
    const std::string suffix = candidate.substr(job_id.size() + 2);
    if (suffix.empty()) return false;
    for (const char c : suffix) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

const char* to_string(ShardState state) {
    switch (state) {
        case ShardState::Pending: return "pending";
        case ShardState::Assigned: return "assigned";
        case ShardState::Running: return "running";
        case ShardState::Completed: return "completed";
        case ShardState::Failed: return "failed";
        case ShardState::Retrying: return "retrying";
        case ShardState::Cancelled: return "cancelled";
    }
    return "unknown";
}

bool shard_state_is_terminal(ShardState state) {
    switch (state) {
        case ShardState::Completed:
        case ShardState::Failed:
        case ShardState::Cancelled: return true;
        case ShardState::Pending:
        case ShardState::Assigned:
        case ShardState::Running:
        case ShardState::Retrying: return false;
    }
    return false;
}

bool shard_state_transition_valid(ShardState from, ShardState to) {
    switch (from) {
        case ShardState::Pending:
            return to == ShardState::Assigned || to == ShardState::Cancelled;
        case ShardState::Assigned:
            return to == ShardState::Running || to == ShardState::Cancelled ||
                   to == ShardState::Pending;  // unplaced by a stale-plan re-plan
        case ShardState::Running:
            return to == ShardState::Completed || to == ShardState::Failed ||
                   to == ShardState::Retrying || to == ShardState::Cancelled;
        case ShardState::Retrying:
            return to == ShardState::Assigned || to == ShardState::Failed ||
                   to == ShardState::Cancelled;
        case ShardState::Completed:
        case ShardState::Failed:
        case ShardState::Cancelled:
            return false;  // terminal: no transition out, ever
    }
    return false;
}

}  // namespace vortyx::distributed
