#pragma once

// Result aggregation (Phase 12) — many shard results, one logical result.
//
// DETERMINISTIC ORDERING: shard outputs are reassembled in SHARD INDEX
// order into the logical result, regardless of the order results arrive.
// Shard 3 finishing before shard 1 changes nothing about the final bytes.
//
// DUPLICATE SAFETY (idempotency): a shard may in principle report twice
// (an ambiguous failure where the work actually succeeded). The aggregator
// records the FIRST result for a shard; any further result for the same
// shard is counted as a DUPLICATE — visible in the aggregate, never
// double-counted, never overwriting the first verdict.
//
// PARTIAL FAILURE HONESTY: the aggregate distinguishes succeeded, failed
// and cancelled shards by count and by shard id. A job whose shards are
// not all Completed is NOT reported Completed — the aggregate's status is
// completed only when every expected shard succeeded (the "8 shards, 7
// succeeded, 1 failed" case is a failed aggregate that still carries the
// 7 successful shards' counts and the failed shard's reason).
//
// RESULT DATA LOCATION (Phase 12 contract): the assembled payload lives in
// the DistributedResult returned to the LOCAL caller. The Phase 11 control
// plane receives metadata only (ResultEnvelope: outcome, backend,
// element count) — result payloads are never stored remotely, exactly as
// in Phase 11.

#include <string>
#include <vector>

#include "distributed/retry.hpp"
#include "distributed/worker.hpp"  // ShardResult
#include "platform/identity.hpp"

namespace vortyx::distributed {

using vortyx::platform::JobId;  // reused platform identity (see device.hpp)

// What accepting one result did.
enum class AggregateOutcome {
    Accepted,    // first result for this shard
    Duplicate,   // a result already existed (first verdict kept)
    Unexpected,  // the shard is not part of the expected plan
};

const char* to_string(AggregateOutcome outcome);

// The logical result of one distributed job.
struct DistributedResult {
    JobId job_id;

    bool completed = false;      // true only when EVERY expected shard succeeded
    std::uint32_t shard_count = 0;      // expected shards
    std::uint32_t succeeded = 0;
    std::uint32_t failed = 0;
    std::uint32_t cancelled = 0;
    std::uint32_t duplicates = 0;       // duplicate results observed

    // Failed shard ids with their stable failure codes and reasons
    // (shard-index order — deterministic).
    std::vector<ShardResult> failures;

    // The reassembled output (size == element_count) when completed; empty
    // otherwise. Assembled in shard-index order.
    std::vector<std::int32_t> data;

    // Backends that actually executed at least one shard (first-use order,
    // deduplicated — observability, never a performance claim).
    std::vector<std::string> backends_used;
};

class ResultAggregator {
public:
    // 'expected_count' is the planned shard count; 'element_count' the
    // logical domain size (the assembled data's size).
    ResultAggregator(JobId job_id, std::uint32_t expected_count, std::uint64_t element_count);

    // Records one result. Duplicate-safe (see the module header).
    AggregateOutcome accept(const ShardResult& result);

    // True when every expected shard has a first result recorded.
    bool complete() const;

    // The deterministic aggregate. Callable any time (the counts are
    // always honest; 'completed' requires full success).
    DistributedResult assemble() const;

private:
    JobId job_id_;
    std::uint32_t expected_count_;
    std::uint64_t element_count_;

    // Indexed by shard index: empty = no result yet (first-wins slots).
    std::vector<ShardResult> results_;
    std::uint32_t duplicates_ = 0;
};

}  // namespace vortyx::distributed
