// Distributed job-machinery tests (Phase 12) — the shard state machine,
// the job status derivation and mapping, the retry policy and the
// duplicate-safe result aggregation.
//
// Everything here is a pure function test: no registry, no transport, no
// clock, no threads — the state semantics ARE the contract.

#include <iostream>
#include <string>
#include <vector>

#include "distributed/distributed.hpp"

using namespace vortyx::distributed;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

JobShard shard_at(std::uint32_t index, ShardState state) {
    JobShard shard;
    shard.shard_id = "job-s" + std::to_string(index);
    shard.parent_job_id = "job";
    shard.index = index;
    shard.state = state;
    return shard;
}

}  // namespace

int main() {
    // =====================================================================
    // 1. Shard state machine: every legal transition, key refusals
    // =====================================================================
    {
        check(shard_state_transition_valid(ShardState::Pending, ShardState::Assigned),
              "pending -> assigned");
        check(shard_state_transition_valid(ShardState::Pending, ShardState::Cancelled),
              "pending -> cancelled");
        check(shard_state_transition_valid(ShardState::Assigned, ShardState::Running),
              "assigned -> running");
        check(shard_state_transition_valid(ShardState::Assigned, ShardState::Cancelled),
              "assigned -> cancelled");
        check(shard_state_transition_valid(ShardState::Assigned, ShardState::Pending),
              "assigned -> pending (stale-plan re-plan puts it back)");
        check(shard_state_transition_valid(ShardState::Running, ShardState::Completed),
              "running -> completed");
        check(shard_state_transition_valid(ShardState::Running, ShardState::Failed),
              "running -> failed");
        check(shard_state_transition_valid(ShardState::Running, ShardState::Retrying),
              "running -> retrying");
        check(shard_state_transition_valid(ShardState::Running, ShardState::Cancelled),
              "running -> cancelled");
        check(shard_state_transition_valid(ShardState::Retrying, ShardState::Assigned),
              "retrying -> assigned (re-placed)");
        check(shard_state_transition_valid(ShardState::Retrying, ShardState::Failed),
              "retrying -> failed (exhausted)");

        check(!shard_state_transition_valid(ShardState::Pending, ShardState::Running),
              "pending -> running is refused (must be assigned first)");
        check(!shard_state_transition_valid(ShardState::Pending, ShardState::Completed),
              "pending -> completed is refused");
        check(!shard_state_transition_valid(ShardState::Retrying, ShardState::Completed),
              "retrying -> completed is refused (it did not run)");
        check(!shard_state_transition_valid(ShardState::Completed, ShardState::Running),
              "completed is terminal (no revival)");
        check(!shard_state_transition_valid(ShardState::Completed, ShardState::Failed),
              "completed -> failed is refused");
        check(!shard_state_transition_valid(ShardState::Failed, ShardState::Retrying),
              "failed is terminal (retry exhaustion is final)");
        check(!shard_state_transition_valid(ShardState::Cancelled, ShardState::Running),
              "cancelled is terminal");

        check(shard_state_is_terminal(ShardState::Completed) &&
                  shard_state_is_terminal(ShardState::Failed) &&
                  shard_state_is_terminal(ShardState::Cancelled),
              "exactly three terminal states");
        check(std::string(to_string(ShardState::Retrying)) == "retrying", "labels are stable");
    }

    // =====================================================================
    // 2. Job status derivation: the deterministic rules
    // =====================================================================
    {
        check(derive_job_status({}) == DistributedJobStatus::Queued,
              "no shards -> queued (nothing has happened)");

        check(derive_job_status({shard_at(0, ShardState::Pending)}) ==
                  DistributedJobStatus::Planning,
              "any pending -> planning");
        check(derive_job_status({shard_at(0, ShardState::Completed),
                                 shard_at(1, ShardState::Retrying)}) ==
                  DistributedJobStatus::Planning,
              "any retrying -> planning (even with successes)");
        check(derive_job_status({shard_at(0, ShardState::Assigned),
                                 shard_at(1, ShardState::Completed)}) ==
                  DistributedJobStatus::Scheduled,
              "any assigned -> scheduled");
        check(derive_job_status({shard_at(0, ShardState::Running),
                                 shard_at(1, ShardState::Completed)}) ==
                  DistributedJobStatus::Running,
              "any running -> running");

        check(derive_job_status({shard_at(0, ShardState::Completed),
                                 shard_at(1, ShardState::Completed)}) ==
                  DistributedJobStatus::Completed,
              "all completed -> completed (the only success)");

        // Partial failure is a failure — never disguised.
        check(derive_job_status({shard_at(0, ShardState::Completed),
                                 shard_at(1, ShardState::Failed)}) ==
                  DistributedJobStatus::Failed,
              "some completed + one failed -> failed (7/8 success is NOT success)");
        check(derive_job_status({shard_at(0, ShardState::Failed),
                                 shard_at(1, ShardState::Failed)}) ==
                  DistributedJobStatus::Failed,
              "all failed -> failed");
        check(derive_job_status({shard_at(0, ShardState::Cancelled),
                                 shard_at(1, ShardState::Cancelled)}) ==
                  DistributedJobStatus::Cancelled,
              "all cancelled -> cancelled");
        check(derive_job_status({shard_at(0, ShardState::Completed),
                                 shard_at(1, ShardState::Cancelled)}) ==
                  DistributedJobStatus::Cancelled,
              "completed + cancelled -> cancelled");
        check(derive_job_status({shard_at(0, ShardState::Completed),
                                 shard_at(1, ShardState::Cancelled),
                                 shard_at(2, ShardState::Failed)}) ==
                  DistributedJobStatus::Failed,
              "failure outranks cancellation among terminal-only sets");
        check(derive_job_status({shard_at(0, ShardState::Running),
                                 shard_at(1, ShardState::Failed)}) ==
                  DistributedJobStatus::Running,
              "an unfinished shard outranks a failed one (the job is still working)");

        // Terminality + transitions.
        check(distributed_job_status_is_terminal(DistributedJobStatus::Completed) &&
                  distributed_job_status_is_terminal(DistributedJobStatus::Failed) &&
                  distributed_job_status_is_terminal(DistributedJobStatus::Cancelled),
              "three terminal job states");
        check(distributed_job_transition_valid(DistributedJobStatus::Queued,
                                               DistributedJobStatus::Planning),
              "queued -> planning");
        check(distributed_job_transition_valid(DistributedJobStatus::Running,
                                               DistributedJobStatus::Planning),
              "running -> planning (re-placement of a failed shard)");
        check(distributed_job_transition_valid(DistributedJobStatus::Scheduled,
                                               DistributedJobStatus::Planning),
              "scheduled -> planning (stale-plan re-plan)");
        check(!distributed_job_transition_valid(DistributedJobStatus::Completed,
                                                DistributedJobStatus::Running),
              "completed -> running is refused");
        check(!distributed_job_transition_valid(DistributedJobStatus::Failed,
                                                DistributedJobStatus::Running),
              "failed -> running is refused");
        check(!distributed_job_transition_valid(DistributedJobStatus::Queued,
                                                DistributedJobStatus::Completed),
              "queued -> completed is refused (must pass through the flow)");

        // The Phase 11 mapping is the honest collapse.
        check(map_to_platform_job_status(DistributedJobStatus::Queued) ==
                  vortyx::platform::JobStatus::Queued,
              "queued maps to queued");
        check(map_to_platform_job_status(DistributedJobStatus::Planning) ==
                      vortyx::platform::JobStatus::Running &&
                  map_to_platform_job_status(DistributedJobStatus::Scheduled) ==
                      vortyx::platform::JobStatus::Running &&
                  map_to_platform_job_status(DistributedJobStatus::Running) ==
                      vortyx::platform::JobStatus::Running,
              "planning/scheduled/running map to running");
        check(map_to_platform_job_status(DistributedJobStatus::Completed) ==
                  vortyx::platform::JobStatus::Completed,
              "completed maps to completed");
        check(map_to_platform_job_status(DistributedJobStatus::Failed) ==
                  vortyx::platform::JobStatus::Failed,
              "failed maps to failed (partial failure included — no new state)");
        check(map_to_platform_job_status(DistributedJobStatus::Cancelled) ==
                  vortyx::platform::JobStatus::Cancelled,
              "cancelled maps to cancelled");
    }

    // =====================================================================
    // 3. Failure codes + retry policy
    // =====================================================================
    {
        check(std::string(to_string(FailureCode::DeviceLost)) == "device_lost",
              "code labels are stable snake_case");
        FailureCode parsed = FailureCode::None;
        check(failure_code_from_string("lease_expired", parsed) &&
                  parsed == FailureCode::LeaseExpired,
              "codes parse from their stable names");
        check(!failure_code_from_string("nonsense", parsed), "unknown names are refused");

        check(is_retryable(FailureCode::WorkerExecutionFailed) &&
                  is_retryable(FailureCode::DeviceLost) &&
                  is_retryable(FailureCode::LeaseExpired) &&
                  is_retryable(FailureCode::ShardTimeout),
              "execution/liveness failures are retryable");
        check(!is_retryable(FailureCode::Cancelled) &&
                  !is_retryable(FailureCode::InvalidAssignment) &&
                  !is_retryable(FailureCode::DuplicateResult),
              "cancellation/contract-bug/duplicate are NOT retryable");
        check(is_failure(FailureCode::DeviceLost) && !is_failure(FailureCode::None) &&
                  !is_failure(FailureCode::DuplicateResult),
              "duplicates are visible but not failures");

        RetryPolicy policy;
        policy.max_attempts = 3;      // 1 initial + 2 retries
        policy.backoff_base_ms = 10;

        check(retry_delay_ms(policy, 1) == 10, "backoff after attempt 1 = base");
        check(retry_delay_ms(policy, 2) == 20, "backoff doubles");
        check(retry_delay_ms(policy, 3) == 40, "backoff doubles again");
        check(retry_delay_ms(policy, 20) == kMaxBackoffMs, "backoff clamps at the cap");

        check(retry_permitted(policy, 0), "the first attempt is always permitted");
        check(retry_permitted(policy, 1) && retry_permitted(policy, 2),
              "retries remain while the ceiling is not reached");
        check(!retry_permitted(policy, 3), "the ceiling is FINAL — no infinite retry");
        policy.max_attempts = 1;
        check(!retry_permitted(policy, 1), "max_attempts=1 means exactly one attempt");
    }

    // =====================================================================
    // 4. ResultAggregator: deterministic order, duplicates, partial failure
    // =====================================================================
    {
        // All success, results arriving OUT OF ORDER: the aggregate is
        // still in logical order (shard 3 first, shard 0 last).
        ResultAggregator aggregator("job", 4, 100);
        auto make_result = [](std::uint32_t index, std::uint64_t begin, std::uint64_t end,
                              std::int32_t base) {
            ShardResult result;
            result.shard_id = "job-s" + std::to_string(index);
            result.parent_job_id = "job";
            result.shard_index = index;
            result.attempt = 1;
            result.device_id = "dev";
            result.backend = "cpu";
            result.completed = true;
            result.element_begin = begin;
            result.element_end = end;
            for (std::uint64_t i = begin; i < end; ++i) result.data.push_back(base);
            return result;
        };
        check(aggregator.accept(make_result(3, 75, 100, 3)) == AggregateOutcome::Accepted,
              "late shard accepted first");
        check(aggregator.accept(make_result(0, 0, 25, 0)) == AggregateOutcome::Accepted,
              "shard 0 accepted after shard 3");
        check(!aggregator.complete(), "missing shards keep the aggregate incomplete");
        check(aggregator.accept(make_result(1, 25, 50, 1)) == AggregateOutcome::Accepted,
              "shard 1 accepted");
        check(aggregator.accept(make_result(2, 50, 75, 2)) == AggregateOutcome::Accepted,
              "shard 2 accepted");
        check(aggregator.complete(), "all shards reported");

        const DistributedResult result = aggregator.assemble();
        check(result.completed, "full success -> completed");
        check(result.succeeded == 4 && result.failed == 0 && result.cancelled == 0,
              "counts are honest");
        check(result.data.size() == 100, "the aggregate reassembles the whole domain");
        bool ordered = true;
        for (std::uint64_t i = 0; i < 100; ++i) {
            const std::int32_t expected = static_cast<std::int32_t>(i / 25);
            ordered = ordered && result.data[static_cast<std::size_t>(i)] == expected;
        }
        check(ordered, "reassembly follows SHARD ORDER regardless of arrival order");

        // Duplicates: visible, counted, never overwriting the first verdict.
        // (The aggregate reassembles data only when EVERY shard succeeded,
        // so the duplicate rides on a fully successful job.)
        ResultAggregator duplicate_case("job", 2, 10);
        ShardResult first = make_result(0, 0, 5, 7);
        ShardResult second = make_result(0, 0, 5, 999);  // a conflicting re-report
        check(duplicate_case.accept(first) == AggregateOutcome::Accepted,
              "the first verdict is accepted");
        check(duplicate_case.accept(second) == AggregateOutcome::Duplicate,
              "the re-report is detected as a duplicate");
        check(duplicate_case.accept(make_result(1, 5, 10, 1)) == AggregateOutcome::Accepted,
              "the other shard reports normally");
        const DistributedResult duplicate_result = duplicate_case.assemble();
        check(duplicate_result.duplicates == 1, "duplicates are counted");
        check(duplicate_result.completed && duplicate_result.data.size() == 10 &&
                  duplicate_result.data[0] == 7,
              "the FIRST verdict's data survives (never overwritten)");

        // Unexpected shard.
        ShardResult outsider = make_result(9, 0, 0, 0);
        check(duplicate_case.accept(outsider) == AggregateOutcome::Unexpected,
              "a shard outside the plan is unexpected");

        // Partial failure: 8 shards, 7 succeed, 1 fails -> NOT completed.
        ResultAggregator partial("job", 8, 80);
        for (std::uint32_t i = 0; i < 8; ++i) {
            ShardResult one = make_result(i, i * 10, (i + 1) * 10, static_cast<std::int32_t>(i));
            if (i == 5) {
                one.completed = false;
                one.failure_code = FailureCode::DeviceLost;
                one.error = "device lost mid-execution";
                one.data.clear();
            }
            partial.accept(one);
        }
        const DistributedResult partial_result = partial.assemble();
        check(!partial_result.completed, "partial failure is NOT success (never disguised)");
        check(partial_result.succeeded == 7 && partial_result.failed == 1,
              "the counts say exactly what happened");
        check(partial_result.failures.size() == 1 &&
                  partial_result.failures[0].shard_index == 5 &&
                  partial_result.failures[0].failure_code == FailureCode::DeviceLost,
              "the failed shard is identified with its stable code");
        check(partial_result.data.empty(), "a failed aggregate carries no faked payload");

        // Cancellation counts.
        ResultAggregator cancelled("job", 2, 10);
        ShardResult done = make_result(0, 0, 5, 1);
        ShardResult stopped = make_result(1, 5, 10, 0);
        stopped.completed = false;
        stopped.failure_code = FailureCode::Cancelled;
        cancelled.accept(done);
        cancelled.accept(stopped);
        const DistributedResult cancelled_result = cancelled.assemble();
        check(!cancelled_result.completed && cancelled_result.cancelled == 1 &&
                  cancelled_result.succeeded == 1,
              "cancellations are tracked separately from failures");
    }

    if (failures == 0) {
        std::cout << "Distributed job tests passed.\n";
        return 0;
    }
    std::cerr << failures << " failure(s)\n";
    return 1;
}
