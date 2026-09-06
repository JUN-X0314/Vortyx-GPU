// Fabric cost model implementation (Phase 16) — see cost.hpp.

#include "fabric/cost.hpp"

#include <algorithm>
#include <limits>

namespace vortyx::fabric {

using vortyx::distributed::DeviceSnapshot;

Status FabricPlannerConfig::validate(std::string& error) const {
    if (base_score < 0) {
        error = "base_score must be >= 0";
        return Status::InvalidInput;
    }
    for (const std::int64_t weight : {slack_penalty_weight, queue_penalty_weight,
                                      locality_bonus_weight, backend_bonus_weight}) {
        if (weight < 0 || weight > kMaxWeight) {
            error = "planner weights must be in [0, " + std::to_string(kMaxWeight) + "]";
            return Status::InvalidInput;
        }
    }
    return Status::Ok;
}

bool checked_scale(std::int64_t value, std::int64_t weight, std::int64_t& out,
                   std::string& error) {
    if (value < 0 || weight < 0) {
        error = "checked_scale: negative input";
        return false;
    }
    if (value != 0 && weight > std::numeric_limits<std::int64_t>::max() / value) {
        error = "score arithmetic would overflow (refused, never wrapped)";
        return false;
    }
    out = value * weight;
    return true;
}

bool score_candidate(const ScoreInputs& inputs, const FabricPlannerConfig& config,
                     ScoreBreakdown& out, std::string& error) {
    if (inputs.candidate == nullptr) {
        error = "score_candidate: null candidate";
        return false;
    }
    if (config.validate(error) != Status::Ok) return false;

    const DeviceSnapshot& device = *inputs.candidate;

    out.base = config.base_score;
    out.locality_bonus = inputs.locality_match ? config.locality_bonus_weight : 0;
    out.backend_bonus = inputs.backend_match ? config.backend_bonus_weight : 0;

    // Slack: available memory minus the requirement (>= 0 — the resource
    // filter already refused candidates that cannot hold the shard).
    const std::int64_t available = device.available().memory_bytes;
    std::int64_t slack = available - inputs.needed_memory_bytes;
    if (slack < 0) {
        // Not the scorer's business to admit what does not fit — the
        // planner filters first; a negative slack here is a caller bug and
        // is refused, not clamped into a falsely attractive score.
        error = "score_candidate: candidate cannot hold the requirement (filter bug)";
        return false;
    }
    if (!checked_scale(slack, config.slack_penalty_weight, out.slack_penalty, error)) {
        return false;
    }
    out.slack_penalty = -out.slack_penalty;  // a penalty

    if (!checked_scale(device.running_shards, config.queue_penalty_weight, out.queue_penalty,
                       error)) {
        return false;
    }
    out.queue_penalty = -out.queue_penalty;  // a penalty

    // Total with checked additions (every term bounded by validation, but
    // the sums are still checked — the honest way to guarantee it).
    const std::int64_t sub_total = out.base + out.slack_penalty + out.queue_penalty;
    if (sub_total > std::numeric_limits<std::int64_t>::max() - out.locality_bonus) {
        error = "score arithmetic would overflow (refused, never wrapped)";
        return false;
    }
    const std::int64_t with_locality = sub_total + out.locality_bonus;
    if (with_locality > std::numeric_limits<std::int64_t>::max() - out.backend_bonus) {
        error = "score arithmetic would overflow (refused, never wrapped)";
        return false;
    }
    out.total = with_locality + out.backend_bonus;
    return true;
}

bool candidate_ranks_before(const ScoreBreakdown& a, const std::string& device_a,
                            const ScoreBreakdown& b, const std::string& device_b) {
    if (a.total != b.total) return a.total > b.total;  // higher score first
    return device_a < device_b;                        // stable id tie-break
}

}  // namespace vortyx::fabric
