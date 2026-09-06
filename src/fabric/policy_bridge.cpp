// FabricPolicy implementation (Phase 16) — see policy_bridge.hpp.

#include "fabric/policy_bridge.hpp"

namespace vortyx::fabric {

FabricPolicy::FabricPolicy(FabricPlannerConfig config) : planner_(config) {}

vortyx::distributed::PlacementPlan FabricPolicy::plan(
    const vortyx::distributed::PlacementRequest& request,
    const vortyx::distributed::ClusterSnapshot& snapshot) {
    using vortyx::distributed::PlacementPlan;
    using vortyx::distributed::PlacementRejection;

    ++counters_.plan_invocations;

    // The request IS the workload metadata (the derive-only map — the
    // orchestrator's vocabulary flows into the fabric's unchanged).
    WorkloadDescriptor descriptor;
    descriptor.workload_id = request.job_id;
    descriptor.owner_user_id = request.owner_user_id;
    descriptor.operation = request.operation;
    descriptor.element_count = request.element_count;
    descriptor.requested_backend = request.requested_backend;
    descriptor.preferred_shard_count = request.requested_shard_count;
    descriptor.allow_fallback = request.allow_fallback;
    descriptor.excluded_devices = request.excluded_devices;
    // descriptor.priority stays default: a per-shard re-placement request
    // carries no priority — the priority semantics live at the GRAPH level
    // (see plan_graph), not at the single-shard placement seam.

    ComputePlan plan_content;
    std::string error;
    const vortyx::platform::Status status =
        planner_.plan_single(descriptor, request.job_id, snapshot, plan_content, error);

    if (status != vortyx::platform::Status::Ok) {
        ++counters_.plan_rejections;
        // Map the fabric's fine-grained stage onto the Phase 12 rejection
        // vocabulary (the orchestrator reports in ITS codes; the structured
        // reason travels in the refused placement's message). Rejected
        // plans take no lineage version: a version names an assignment set,
        // and a refusal names none.
        PlacementPlan refused;
        refused.accepted = false;
        refused.cluster_revision = snapshot.revision;
        const std::string stage = plan_content.rejection.code != nullptr
                                      ? plan_content.rejection.code
                                      : "";
        if (stage == rejection_stage::kClusterEmpty) {
            refused.rejection = PlacementRejection::ClusterEmpty;
        } else if (stage == rejection_stage::kUnsupportedCapability) {
            refused.rejection = PlacementRejection::UnsupportedCapability;
        } else if (stage == rejection_stage::kInsufficientResource) {
            refused.rejection = PlacementRejection::InsufficientResource;
        } else if (stage == rejection_stage::kExcludedConstraint) {
            refused.rejection = PlacementRejection::NoDeviceAvailable;
        } else {
            refused.rejection = PlacementRejection::InvalidRequest;
        }
        refused.message = error;
        return refused;
    }

    // Publish into the lineage (same content keeps the current version —
    // the replan version rule lives in PlanLineage::record).
    PlanLineage& lineage = lineages_[request.job_id];
    const char* trigger = lineage.current_version() == 0
                              ? lineage_trigger::kInitial
                              : lineage_trigger::kReplanRemaining;
    const std::uint32_t version = lineage.record(plan_content, trigger);
    if (version > 1u) ++counters_.replans;
    ++counters_.plans_published;

    // Translate the plan content into the Phase 12 placement.
    PlacementPlan out;
    out.accepted = true;
    out.cluster_revision = snapshot.revision;
    const PlanNodeAssignment& node = plan_content.nodes.front();
    out.shards.reserve(node.shards.size());
    for (const PlanShardAssignment& shard : node.shards) {
        vortyx::distributed::ShardPlan shard_plan;
        shard_plan.shard_index = shard.shard_index;
        shard_plan.range = shard.range;
        shard_plan.device_id = shard.device_id;
        // The reservation requirement — the SAME rule the Phase 12
        // policies apply (shard memory via shard_memory_bytes, one
        // concurrent slot, no compute-unit claim): the orchestrator
        // reserves exactly this against the live registry, so the fabric's
        // placements hold the same capacity accounting as native ones.
        vortyx::distributed::ResourceVector needed;
        std::string mem_error;
        if (!vortyx::distributed::shard_memory_bytes(shard.range.size(),
                                                     descriptor.operation,
                                                     needed.memory_bytes, mem_error)) {
            // Cannot happen for ranges the planner just placed (the sizes
            // were checked there); refuse honestly instead of reserving 0.
            PlacementPlan refused;
            refused.accepted = false;
            refused.cluster_revision = snapshot.revision;
            refused.rejection = vortyx::distributed::PlacementRejection::InvalidRequest;
            refused.message = "workload " + descriptor.workload_id + ": " + mem_error;
            return refused;
        }
        needed.concurrent_jobs = 1;
        shard_plan.resources = needed;
        out.shards.push_back(shard_plan);
    }
    return out;
}

const PlanLineage* FabricPolicy::lineage_for(const vortyx::platform::JobId& job_id) const {
    const auto it = lineages_.find(job_id);
    return it == lineages_.end() ? nullptr : &it->second;
}

const ComputePlan* FabricPolicy::last_plan_for(const vortyx::platform::JobId& job_id) const {
    const PlanLineage* lineage = lineage_for(job_id);
    return lineage == nullptr ? nullptr : lineage->current_plan();
}

void FabricPolicy::forget(const vortyx::platform::JobId& job_id) { lineages_.erase(job_id); }

std::unique_ptr<FabricPolicy> make_fabric_policy(FabricPlannerConfig config) {
    return std::make_unique<FabricPolicy>(config);
}

}  // namespace vortyx::fabric
