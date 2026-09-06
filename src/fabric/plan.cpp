// ComputePlan implementation (Phase 16) — see plan.hpp.

#include "fabric/plan.hpp"

#include "platform/json.hpp"

namespace vortyx::fabric {

bool plan_is_stale(const ComputePlan& plan,
                   const vortyx::distributed::ClusterSnapshot& snapshot) {
    // The pure stale rule: the cluster moved under the plan. Nothing else
    // qualifies — a plan is not "older" by wall clock (no clock input
    // exists here), only by revision.
    return plan.cluster_revision != snapshot.revision;
}

std::string serialize_compute_plan(const ComputePlan& plan) {
    using vortyx::platform::JsonValue;

    JsonValue out = JsonValue::make_object();

    // Canonical field order (the schema — stable across releases).
    out.add("graph_id", JsonValue::make_string(plan.graph_id));
    out.add("plan_version", JsonValue::make_number(plan.plan_version));
    out.add("cluster_revision", JsonValue::make_number(
              static_cast<double>(plan.cluster_revision)));
    out.add("planner_name", JsonValue::make_string(plan.planner_name));
    out.add("planner_version", JsonValue::make_string(plan.planner_version));
    out.add("rejection_code", JsonValue::make_string(plan.rejection.code));
    out.add("rejection_node", JsonValue::make_string(plan.rejection.node_workload_id));
    out.add("rejection_message", JsonValue::make_string(plan.rejection.message));

    JsonValue nodes = JsonValue::make_array();
    for (const PlanNodeAssignment& node : plan.nodes) {
        JsonValue node_value = JsonValue::make_object();
        node_value.add("node_id", JsonValue::make_number(node.node_id));
        node_value.add("workload_id", JsonValue::make_string(node.workload_id));

        JsonValue shards = JsonValue::make_array();
        for (const PlanShardAssignment& shard : node.shards) {
            JsonValue shard_value = JsonValue::make_object();
            shard_value.add("shard_index", JsonValue::make_number(shard.shard_index));
            shard_value.add("begin", JsonValue::make_number(
                                 static_cast<double>(shard.range.begin)));
            shard_value.add("end", JsonValue::make_number(
                                static_cast<double>(shard.range.end)));
            shard_value.add("device_id", JsonValue::make_string(shard.device_id));
            shards.push(std::move(shard_value));
        }
        node_value.add("shards", std::move(shards));

        JsonValue decision = JsonValue::make_object();
        decision.add("device_id", JsonValue::make_string(node.decision.device_id));
        JsonValue score = JsonValue::make_object();
        const ScoreBreakdown& breakdown = node.decision.score;
        score.add("base", JsonValue::make_number(static_cast<double>(breakdown.base)));
        score.add("slack_penalty",
                  JsonValue::make_number(static_cast<double>(breakdown.slack_penalty)));
        score.add("queue_penalty",
                  JsonValue::make_number(static_cast<double>(breakdown.queue_penalty)));
        score.add("locality_bonus",
                  JsonValue::make_number(static_cast<double>(breakdown.locality_bonus)));
        score.add("backend_bonus",
                  JsonValue::make_number(static_cast<double>(breakdown.backend_bonus)));
        score.add("total", JsonValue::make_number(static_cast<double>(breakdown.total)));
        decision.add("score", std::move(score));

        JsonValue rejected = JsonValue::make_array();
        for (const CandidateRejection& rejection : node.decision.rejected) {
            JsonValue entry = JsonValue::make_object();
            entry.add("device_id", JsonValue::make_string(rejection.device_id));
            entry.add("stage", JsonValue::make_string(rejection.stage));
            rejected.push(std::move(entry));
        }
        decision.add("rejected", std::move(rejected));
        decision.add("rejected_recorded", JsonValue::make_number(
                          static_cast<double>(node.decision.rejection_summary.recorded)));
        decision.add("rejected_total", JsonValue::make_number(
                          static_cast<double>(node.decision.rejection_summary.total_rejected)));
        decision.add("capable_total", JsonValue::make_number(
                          static_cast<double>(node.decision.rejection_summary.total_capable)));
        node_value.add("decision", std::move(decision));

        nodes.push(std::move(node_value));
    }
    out.add("nodes", std::move(nodes));

    return out.serialize();
}

}  // namespace vortyx::fabric
