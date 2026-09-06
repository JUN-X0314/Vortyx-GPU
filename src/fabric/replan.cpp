// Feedback, lineage and replanning implementation (Phase 16) — see replan.hpp.

#include "fabric/replan.hpp"

#include <algorithm>

namespace vortyx::fabric {

const char* to_string(ShardOutcome outcome) {
    switch (outcome) {
        case ShardOutcome::Succeeded: return "succeeded";
        case ShardOutcome::Failed: return "failed";
        case ShardOutcome::Cancelled: return "cancelled";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// PlanLineage
// ---------------------------------------------------------------------------

std::uint32_t PlanLineage::record(const ComputePlan& content, const char* trigger) {
    // Same-content rule: identical content keeps the current version (a
    // version never names two different assignment sets, and an identical
    // plan never manufactures a fake new version).
    if (!entries_.empty()) {
        ComputePlan candidate = content;
        candidate.plan_version = entries_.back().plan.plan_version;
        if (candidate == entries_.back().plan) {
            return entries_.back().plan_version;
        }
    }

    const std::uint32_t next_version =
        entries_.empty() ? 1u : entries_.back().plan_version + 1u;
    PlanLineageEntry entry;
    entry.plan_version = next_version;
    entry.cluster_revision = content.cluster_revision;
    entry.trigger = trigger;
    entry.plan = content;
    entry.plan.plan_version = next_version;
    entries_.push_back(std::move(entry));

    // The bounded ring: the current version is always retained.
    while (entries_.size() > kMaxEntries) {
        entries_.pop_front();
    }
    return next_version;
}

std::uint32_t PlanLineage::current_version() const {
    return entries_.empty() ? 0u : entries_.back().plan_version;
}

const ComputePlan* PlanLineage::current_plan() const {
    return entries_.empty() ? nullptr : &entries_.back().plan;
}

// ---------------------------------------------------------------------------
// FabricReplanner
// ---------------------------------------------------------------------------

vortyx::platform::Status FabricReplanner::replan(
    const ComputePlan& previous, const std::vector<ShardExecutionFeedback>& outcomes,
    const WorkloadGraph& graph, const std::string& graph_id,
    const vortyx::distributed::ClusterSnapshot& fresh_snapshot, ComputePlan& out,
    std::string& error) const {
    out = ComputePlan{};

    // The replan is FOR a changed cluster: a same-revision replan is a
    // caller bug (nothing changed — there is nothing to re-decide) and is
    // refused, not silently treated as a fresh plan.
    if (previous.cluster_revision == fresh_snapshot.revision) {
        error = "replan refused: the snapshot revision has not changed (" +
                std::to_string(fresh_snapshot.revision) + ")";
        return vortyx::platform::Status::InvalidInput;
    }

    // Index the succeeded checkpoints by workload id.
    std::vector<const ShardExecutionFeedback*> succeeded;
    for (const ShardExecutionFeedback& feedback : outcomes) {
        if (feedback.outcome == ShardOutcome::Succeeded) succeeded.push_back(&feedback);
    }

    // The fresh plan content for the WHOLE graph first (the planner core
    // is the only placement authority — the replanner does not place by
    // hand). Then the succeeded shards' assignments OVERWRITE the fresh
    // content for their nodes: the checkpoint wins, the fresh planning of
    // that node's remaining shards is discarded in favor of the recorded
    // truth. A node whose shards ALL succeeded keeps exactly its recorded
    // assignments — nothing re-runs.
    ComputePlan fresh;
    vortyx::platform::Status status =
        planner_.plan_graph(graph, graph_id, fresh_snapshot, fresh, error);
    if (status != vortyx::platform::Status::Ok) {
        // The remaining work is not placeable right now (a device died and
        // nothing can take its place). The structured rejection travels;
        // the caller decides with the Phase 12 retry rules.
        out = fresh;
        return status;
    }

    // Per node: succeeded shards preserved verbatim; unfinished shards
    // re-planned (the fresh plan's shards for this node minus the
    // succeeded ones — the fresh placement of a succeeded shard_index is
    // REPLACED by the checkpoint's recorded assignment).
    out.graph_id = fresh.graph_id;
    out.cluster_revision = fresh.cluster_revision;
    out.planner_name = fresh.planner_name;
    out.planner_version = fresh.planner_version;
    // Monotonic versioning: the replan is the NEXT version. The previous
    // version's identity is untouched (history, not mutable state). An
    // unassigned (pure) previous content replans to version 1.
    out.plan_version = previous.plan_version + 1u;
    out.nodes = std::move(fresh.nodes);

    for (PlanNodeAssignment& node : out.nodes) {
        for (const ShardExecutionFeedback* feedback : succeeded) {
            if (feedback->workload_id != node.workload_id) continue;
            // Find the fresh assignment for this shard index and REPLACE
            // it with the recorded checkpoint (same index, the RECORDED
            // device and range — never re-derived).
            bool replaced = false;
            for (PlanShardAssignment& shard : node.shards) {
                if (shard.shard_index == feedback->shard_index) {
                    shard.device_id = feedback->device_id;
                    shard.range = feedback->range;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                // The fresh plan did not produce this shard index (e.g. a
                // coalesced shard count). Preserve the checkpoint anyway —
                // it is the recorded truth (index, range, device).
                PlanShardAssignment preserved;
                preserved.shard_index = feedback->shard_index;
                preserved.range = feedback->range;
                preserved.device_id = feedback->device_id;
                node.shards.push_back(preserved);
            }
        }
        // Deterministic shard order after the merge.
        std::sort(node.shards.begin(), node.shards.end(),
                  [](const PlanShardAssignment& a, const PlanShardAssignment& b) {
                      return a.shard_index < b.shard_index;
                  });
    }

    return vortyx::platform::Status::Ok;
}

}  // namespace vortyx::fabric
