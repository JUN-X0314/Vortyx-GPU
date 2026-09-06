// Phase 16 — Adaptive Compute Fabric: workload descriptor, cost model and
// feedback honesty (test_fabric_core.cpp).
//
// Pins: descriptor validation, the deterministic derivation from a
// JobEnvelope, config validation, the score's named components and their
// determinism, the checked-arithmetic refusals, the candidate ordering
// rule and the feedback vocabulary's measured-vs-estimated honesty.

#include <iostream>
#include <string>

#include "fabric/cost.hpp"
#include "fabric/replan.hpp"
#include "fabric/workload.hpp"

using namespace vortyx;
using namespace vortyx::fabric;

namespace {

int failures = 0;

void check(bool ok, const char* name, const std::string& detail = "") {
    if (ok) {
        std::cout << "PASS: " << name << "\n";
    } else {
        std::cout << "FAIL: " << name;
        if (!detail.empty()) std::cout << "  [" << detail << "]";
        std::cout << "\n";
        ++failures;
    }
}

WorkloadDescriptor valid_descriptor() {
    WorkloadDescriptor descriptor;
    descriptor.workload_id = "wl-1";
    descriptor.owner_user_id = "user-1";
    descriptor.operation = vortyx::compute::ComputeOp::VectorAdd;
    descriptor.element_count = 1000;
    return descriptor;
}

vortyx::distributed::DeviceSnapshot snapshot(const std::string& id, std::int64_t memory,
                                             std::int64_t running) {
    vortyx::distributed::DeviceSnapshot device;
    device.device_id = id;
    device.owner_user_id = "user-1";
    device.state = vortyx::distributed::DeviceState::Ready;
    device.health = vortyx::distributed::DeviceHealth::Healthy;
    device.capabilities.capacity.memory_bytes = memory;
    device.capabilities.capacity.compute_units = 4;
    device.capabilities.capacity.concurrent_jobs = 2;
    device.capabilities.max_concurrent_shards = 2;
    device.running_shards = running;
    // The operation claim (the metadata's canonical label).
    device.capabilities.metadata.software_version = "0.16.0";
    device.capabilities.metadata.backends = {"cpu"};
    device.capabilities.metadata.operations = {"vector_add", "vector_multiply", "vector_scale"};
    return device;
}

}  // namespace

int main() {
    // ---- descriptor validation -------------------------------------------
    {
        std::string error;
        check(validate_workload_descriptor(valid_descriptor(), error) == Status::Ok,
              "descriptor: valid input accepted");

        WorkloadDescriptor zero = valid_descriptor();
        zero.element_count = 0;
        check(validate_workload_descriptor(zero, error) == Status::InvalidInput &&
                  error.find("element_count") != std::string::npos,
              "descriptor: zero elements refused");

        WorkloadDescriptor shards = valid_descriptor();
        shards.preferred_shard_count = kMaxPreferredShardCount + 1;
        check(validate_workload_descriptor(shards, error) == Status::InvalidInput,
              "descriptor: shard-count cap enforced");

        WorkloadDescriptor backend = valid_descriptor();
        backend.requested_backend = "cuda";  // NOT a canonical backend here
        check(validate_workload_descriptor(backend, error) == Status::InvalidInput,
              "descriptor: unknown backend name refused");

        WorkloadDescriptor owner = valid_descriptor();
        owner.owner_user_id = "";
        check(validate_workload_descriptor(owner, error) == Status::InvalidInput,
              "descriptor: empty owner refused");

        WorkloadDescriptor excluded = valid_descriptor();
        excluded.excluded_devices = {"bad id!"};
        check(validate_workload_descriptor(excluded, error) == Status::InvalidInput,
              "descriptor: invalid excluded device id refused");
    }

    // ---- deterministic derivation from the envelope ----------------------
    {
        JobEnvelope envelope;
        envelope.job_id = "job-42";
        envelope.operation = vortyx::compute::ComputeOp::VectorMultiply;
        envelope.element_count = 2048;
        envelope.requested_backend = "cpu";
        envelope.priority = 7;

        const WorkloadDescriptor derived =
            derive_workload_descriptor(envelope, "user-9", 3, false);
        std::string error;
        check(validate_workload_descriptor(derived, error) == Status::Ok,
              "derivation: derived descriptor is valid");
        check(derived.workload_id == "job-42" && derived.owner_user_id == "user-9" &&
                  derived.operation == vortyx::compute::ComputeOp::VectorMultiply &&
                  derived.element_count == 2048 && derived.requested_backend == "cpu" &&
                  derived.priority == 7 && derived.preferred_shard_count == 3 &&
                  !derived.allow_fallback && derived.preferred_device.empty() &&
                  derived.excluded_devices.empty(),
              "derivation: every envelope field flows through verbatim");

        // The same envelope derives the same descriptor (determinism).
        const WorkloadDescriptor again =
            derive_workload_descriptor(envelope, "user-9", 3, false);
        check(again.workload_id == derived.workload_id &&
                  again.element_count == derived.element_count &&
                  again.priority == derived.priority,
              "derivation: deterministic");
    }

    // ---- planner config validation ---------------------------------------
    {
        std::string error;
        FabricPlannerConfig config;
        check(config.validate(error) == Status::Ok, "config: defaults valid");

        FabricPlannerConfig negative;
        negative.slack_penalty_weight = -1;
        check(negative.validate(error) == Status::InvalidInput,
              "config: negative weight refused");

        FabricPlannerConfig huge;
        huge.queue_penalty_weight = kMaxWeight + 1;
        check(huge.validate(error) == Status::InvalidInput, "config: weight cap enforced");

        FabricPlannerConfig bad_base;
        bad_base.base_score = -5;
        check(bad_base.validate(error) == Status::InvalidInput,
              "config: negative base score refused");
    }

    // ---- scoring determinism + named components --------------------------
    {
        const vortyx::distributed::DeviceSnapshot device = snapshot("dev-a", 100000, 2);
        FabricPlannerConfig config;

        ScoreInputs inputs;
        inputs.candidate = &device;
        inputs.needed_memory_bytes = 40000;
        inputs.locality_match = true;
        inputs.backend_match = false;

        ScoreBreakdown first;
        std::string error;
        check(score_candidate(inputs, config, first, error) == true,
              "scoring: candidate scored");
        check(first.base == config.base_score &&
                  first.slack_penalty == -(60000 * config.slack_penalty_weight) &&
                  first.queue_penalty == -(2 * config.queue_penalty_weight) &&
                  first.locality_bonus == config.locality_bonus_weight &&
                  first.backend_bonus == 0,
              "scoring: every named component has its documented value");

        ScoreBreakdown again;
        check(score_candidate(inputs, config, again, error) == true && again == first,
              "scoring: same inputs, same breakdown (deterministic)");

        // Tighter fit ranks before looser fit (all else equal).
        vortyx::distributed::DeviceSnapshot loose = snapshot("dev-b", 100000, 2);
        ScoreInputs loose_inputs = inputs;
        loose_inputs.candidate = &loose;
        loose_inputs.needed_memory_bytes = 20000;  // more slack
        ScoreBreakdown loose_score;
        check(score_candidate(loose_inputs, config, loose_score, error) == true,
              "scoring: looser candidate scored");
        check(candidate_ranks_before(first, "dev-a", loose_score, "dev-b"),
              "ordering: tighter fit wins (the smallest-slack rule)");

        // Exact tie -> the smaller device id (stable identifiers, never
        // iteration order).
        check(candidate_ranks_before(first, "dev-a", first, "dev-b") &&
                  !candidate_ranks_before(first, "dev-b", first, "dev-a"),
              "ordering: tie broken by the smaller device id");

        // The score refuses rather than wraps.
        ScoreInputs overflow_inputs;
        vortyx::distributed::DeviceSnapshot big = snapshot("dev-big", 40000, 0);
        overflow_inputs.candidate = &big;
        overflow_inputs.needed_memory_bytes = 0;
        FabricPlannerConfig bad_config;
        bad_config.slack_penalty_weight = kMaxWeight;
        // slack = 40000; weighted under kMaxWeight^2 — under the int64 line.
        ScoreBreakdown scaled;
        check(score_candidate(overflow_inputs, bad_config, scaled, error) == true,
              "scoring: bounded weights stay exact");

        // A candidate that cannot hold the requirement is refused (a
        // scorer that clamps would fabricate an attractive score).
        ScoreInputs unfit;
        unfit.candidate = &device;  // 100000 bytes available
        unfit.needed_memory_bytes = 200000;
        ScoreBreakdown refused;
        check(score_candidate(unfit, config, refused, error) == false,
              "scoring: unfit candidate refused, not clamped");

        // Overflow refusal: absurd weight against huge slack.
        vortyx::distributed::DeviceSnapshot enormous = snapshot("dev-c", 4000000000000LL, 0);
        ScoreInputs overflow2;
        overflow2.candidate = &enormous;
        overflow2.needed_memory_bytes = 0;
        ScoreBreakdown refused2;
        check(score_candidate(overflow2, bad_config, refused2, error) == false,
              "scoring: score overflow refused, never wrapped");
    }

    // ---- feedback honesty -------------------------------------------------
    {
        ShardExecutionFeedback feedback;
        feedback.shard_id = "job-1-s0";
        feedback.workload_id = "job-1";
        feedback.shard_index = 0;
        feedback.device_id = "dev-a";
        feedback.outcome = ShardOutcome::Succeeded;
        check(std::string(to_string(feedback.outcome)) == "succeeded",
              "feedback: succeeded outcome labeled");
        check(!feedback.measured_duration_ms_present,
              "feedback: measured duration absent unless actually measured");
        ShardExecutionFeedback failed;
        failed.outcome = ShardOutcome::Failed;
        failed.failure_code = "device_lost";
        check(std::string(to_string(failed.outcome)) == "failed" &&
                  failed.measured_duration_ms_present == false,
              "feedback: failed outcome labeled, no synthesized duration");
    }

    if (failures == 0) {
        std::cout << "ALL FABRIC CORE CHECKS PASSED\n";
        return 0;
    }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
