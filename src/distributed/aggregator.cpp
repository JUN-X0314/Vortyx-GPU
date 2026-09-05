// Result aggregation implementation (Phase 12).

#include "distributed/aggregator.hpp"

namespace vortyx::distributed {

const char* to_string(AggregateOutcome outcome) {
    switch (outcome) {
        case AggregateOutcome::Accepted: return "accepted";
        case AggregateOutcome::Duplicate: return "duplicate";
        case AggregateOutcome::Unexpected: return "unexpected";
    }
    return "unknown";
}

ResultAggregator::ResultAggregator(JobId job_id, std::uint32_t expected_count,
                                   std::uint64_t element_count)
    : job_id_(std::move(job_id)),
      expected_count_(expected_count),
      element_count_(element_count) {
    results_.assign(expected_count_, ShardResult{});
}

AggregateOutcome ResultAggregator::accept(const ShardResult& result) {
    if (result.shard_index >= expected_count_) {
        return AggregateOutcome::Unexpected;  // not part of this plan
    }
    ShardResult& slot = results_[result.shard_index];
    if (slot.shard_id.empty()) {
        // Empty slot (every real result carries a shard_id): first wins.
        slot = result;
        return AggregateOutcome::Accepted;
    }
    ++duplicates_;  // visible in the aggregate, never overwrites, never double-counts
    return AggregateOutcome::Duplicate;
}

bool ResultAggregator::complete() const {
    for (const ShardResult& slot : results_) {
        if (slot.shard_id.empty()) return false;
    }
    return true;
}

DistributedResult ResultAggregator::assemble() const {
    DistributedResult out;
    out.job_id = job_id_;
    out.shard_count = expected_count_;

    std::vector<std::int32_t> assembled;
    bool all_succeeded = expected_count_ > 0;
    bool saw_any = false;

    for (std::uint32_t i = 0; i < expected_count_; ++i) {
        const ShardResult& slot = results_[i];
        if (slot.shard_id.empty()) {
            all_succeeded = false;  // missing result: not complete
            continue;
        }
        saw_any = true;
        bool backend_known = false;
        for (const std::string& used : out.backends_used) {
            if (used == slot.backend) backend_known = true;
        }
        if (!backend_known && !slot.backend.empty()) {
            out.backends_used.push_back(slot.backend);
        }
        if (slot.completed) {
            ++out.succeeded;
            // Deterministic reassembly: each shard writes its slice at its
            // own offset. Shards arrive in index order HERE, so the bytes
            // land identically no matter the completion order.
            if (assembled.size() < slot.element_end) assembled.resize(slot.element_end);
            for (std::uint64_t j = 0; j < slot.data.size(); ++j) {
                assembled[static_cast<std::size_t>(slot.element_begin + j)] = slot.data[j];
            }
        } else if (slot.failure_code == FailureCode::Cancelled) {
            ++out.cancelled;
            all_succeeded = false;
        } else {
            ++out.failed;
            all_succeeded = false;
            out.failures.push_back(slot);
        }
    }
    if (!saw_any) all_succeeded = false;

    out.completed = all_succeeded;
    if (out.completed) out.data = std::move(assembled);
    out.duplicates = duplicates_;
    return out;
}

}  // namespace vortyx::distributed
