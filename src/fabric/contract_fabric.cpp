// Fabric contract serialization implementation (Phase 16) — see contract_fabric.hpp.

#include "fabric/contract_fabric.hpp"

#include <algorithm>

#include "platform/json.hpp"

namespace vortyx::fabric {

PlanSummary summarize_plan(const ComputePlan& plan) {
    PlanSummary summary;
    summary.plan_version = plan.plan_version;
    summary.planner_name = plan.planner_name;
    summary.planner_version = plan.planner_version;
    summary.cluster_revision = plan.cluster_revision;

    // The reason summary: every clause is a recorded fact of the plan —
    // the top-level outcome, the per-node decisions with their named score
    // components, and the bounded rejection counts. Deterministic order
    // (the plan's own node order).
    std::string reason;
    if (plan.rejection.code != nullptr && *plan.rejection.code != '\0') {
        reason = "refused: ";
        reason += plan.rejection.code;
        if (!plan.rejection.node_workload_id.empty()) {
            reason += " (workload ";
            reason += plan.rejection.node_workload_id;
            reason += ")";
        }
        reason += ": ";
        reason += plan.rejection.message;
    } else {
        reason = "accepted";
        if (!plan.nodes.empty()) {
            reason += ": planned ";
            reason += std::to_string(plan.nodes.size());
            reason += plan.nodes.size() == 1 ? " workload" : " workloads";
        }
        for (const PlanNodeAssignment& node : plan.nodes) {
            reason += "; ";
            reason += node.workload_id;
            reason += " -> ";
            reason += node.decision.device_id;
            if (node.decision.rejection_summary.total_rejected > 0) {
                reason += " (";
                reason += std::to_string(node.decision.rejection_summary.total_rejected);
                reason += " candidate(s) rejected)";
            }
        }
    }
    summary.reason_summary = reason;

    // The devices, deduplicated, in the plan's node/shard order.
    for (const PlanNodeAssignment& node : plan.nodes) {
        for (const PlanShardAssignment& shard : node.shards) {
            const bool present =
                std::find(summary.devices.begin(), summary.devices.end(),
                          shard.device_id) != summary.devices.end();
            if (!present) summary.devices.push_back(shard.device_id);
        }
    }
    return summary;
}

std::string serialize_plan_summary(const PlanSummary& summary) {
    using vortyx::platform::JsonValue;

    JsonValue out = JsonValue::make_object();
    out.add("plan_version", JsonValue::make_number(summary.plan_version));
    out.add("planner", JsonValue::make_string(summary.planner_name));
    out.add("planner_version", JsonValue::make_string(summary.planner_version));
    out.add("cluster_revision",
            JsonValue::make_number(static_cast<double>(summary.cluster_revision)));
    JsonValue devices = JsonValue::make_array();
    for (const std::string& device : summary.devices) {
        devices.push(JsonValue::make_string(device));
    }
    out.add("devices", std::move(devices));
    out.add("reason", JsonValue::make_string(summary.reason_summary));
    return out.serialize();
}

}  // namespace vortyx::fabric
