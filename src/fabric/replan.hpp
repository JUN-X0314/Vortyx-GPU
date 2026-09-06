#pragma once

// Execution feedback, plan lineage and the replanning rule (Phase 16).
//
// ═══════════════════════════════════════════════════════════════════════
// FEEDBACK (what the fabric may honestly know): the fabric consumes the
// outcomes the execution path really reports — shard success/failure/
// cancellation with the stable Phase 12 failure codes, and a measured
// duration ONLY when a caller actually measured one (the field is
// optional and never synthesized). Device utilization, throughput,
// temperature and every other measurement the system does not perform do
// not exist here — a fabric that invented telemetry would be lying at the
// architecture level.
//
// LINEAGE (the plan history): per workload graph, the versions the fabric
// published for it — version 1 is the initial plan; every REPLAN bumps
// the version by exactly one. A lineage entry is metadata only (version,
// revision, trigger, the plan content). The history is bounded (a ring —
// the OLDEST entries fall off; the current version is always retained):
// an unbounded history would be a memory leak wearing a feature hat.
//
// REPLANNING (the safe boundary, precisely drawn):
//   - SUCCEEDED shards are CHECKPOINTS: their assignments are copied
//     verbatim into the new plan version and are never re-placed, never
//     re-executed by anything this layer does.
//   - Only UNFINISHED work (failed-with-retries-remaining, cancelled-
//     before-dispatch, or never-dispatched) is re-planned against a FRESH
//     snapshot.
//   - The new version is previous + 1. The same version never names two
//     different assignment sets (the lineage stamps a version only when
//     the content differs from the current one).
//   - Terminal jobs are never reopened: replanning consumes OUTCOMES, it
//     does not mutate any job or shard state — the terminal-state
//     immutability rules of Phases 11/12/15 are untouched by design.
//
// STALE DETECTION (the trigger side): a plan computed from snapshot
// revision R is stale against any snapshot whose revision differs
// (plan_is_stale, plan.hpp — pure). The replanner REFUSES to re-plan from
// a stale pair: the fresh snapshot is the input, the old plan is history.
// ═══════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "fabric/cost.hpp"
#include "fabric/graph.hpp"
#include "fabric/plan.hpp"
#include "fabric/planner.hpp"
#include "fabric/workload.hpp"
#include "distributed/shard.hpp"  // ElementRange (the checkpoint's work slice)
#include "platform/status.hpp"

namespace vortyx::fabric {

// ---------------------------------------------------------------------------
// Feedback (the honest outcome record)
// ---------------------------------------------------------------------------

enum class ShardOutcome {
    Succeeded,
    Failed,
    Cancelled,
};

const char* to_string(ShardOutcome outcome);

struct ShardExecutionFeedback {
    std::string shard_id;        // the Phase 12 deterministic shard id
    std::string workload_id;     // the node this shard belonged to
    std::uint32_t shard_index = 0;
    ElementRange range;          // the work slice it executed (the checkpoint)
    DeviceId device_id;          // where it ran (or was refused)
    ShardOutcome outcome = ShardOutcome::Failed;
    std::string failure_code;    // the Phase 12 stable code when Failed
    // Measured wall-clock duration (ms) — set ONLY when the caller really
    // measured the execution. Empty = not measured (never synthesized).
    bool measured_duration_ms_present = false;
    std::int64_t measured_duration_ms = 0;
};

// ---------------------------------------------------------------------------
// Lineage (the bounded plan history)
// ---------------------------------------------------------------------------

// Why a plan version exists (stable codes; the explanation vocabulary).
namespace lineage_trigger {
inline constexpr const char* kInitial = "initial";
inline constexpr const char* kReplanRemaining = "replan_remaining";
inline constexpr const char* kStaleCluster = "replan_stale_cluster";
}  // namespace lineage_trigger

struct PlanLineageEntry {
    std::uint32_t plan_version = 0;
    std::uint64_t cluster_revision = 0;
    const char* trigger = "";        // stable code, see lineage_trigger
    ComputePlan plan;                // the content of that version

    friend bool operator==(const PlanLineageEntry& a, const PlanLineageEntry& b) {
        return a.plan_version == b.plan_version && a.cluster_revision == b.cluster_revision &&
               std::string(a.trigger) == std::string(b.trigger) && a.plan == b.plan;
    }
    friend bool operator!=(const PlanLineageEntry& a, const PlanLineageEntry& b) {
        return !(a == b);
    }
};

// The per-graph lineage ring. Not thread-safe BY DESIGN: one lineage belongs
// to one planning context (the policy bridge serializes access with the
// orchestrator's own placement serialization); a shared mutable lineage
// across threads would need a lock and a claim about atomicity nothing
// below needs.
class PlanLineage {
public:
    // The lineage depth (oldest falls off; the current version is always
    // retained — a ring, not a leak).
    static constexpr std::size_t kMaxEntries = 8;

    // Records a plan content as the NEXT version. Rule: if the content
    // (with the version field zeroed) equals the CURRENT version's
    // content, the current version is returned unchanged — the same
    // version never names two different contents, and identical content
    // does not manufacture a fake new version.
    std::uint32_t record(const ComputePlan& content, const char* trigger);

    // The current (highest) version, or 0 when nothing was recorded.
    std::uint32_t current_version() const;
    const ComputePlan* current_plan() const;  // nullptr when empty
    const std::deque<PlanLineageEntry>& entries() const { return entries_; }

private:
    std::deque<PlanLineageEntry> entries_;  // ascending version (bounded ring)
};

// ---------------------------------------------------------------------------
// The replanner (the safe boundary)
// ---------------------------------------------------------------------------

class FabricReplanner {
public:
    explicit FabricReplanner(const FabricPlanner& planner) : planner_(planner) {}

    // Re-plans the UNFINISHED work of a graph against a FRESH snapshot.
    //
    //   previous       — the plan being superseded (any version; the
    //                    replanner reads it, never mutates it)
    //   outcomes       — the execution feedback so far (which shards
    //                    SUCCEEDED where — the checkpoints to preserve)
    //   fresh_snapshot — the CURRENT cluster (the replanner refuses a
    //                    stale pair: fresh.revision must differ from
    //                    previous.cluster_revision — replanning is FOR a
    //                    changed cluster, not a no-op)
    //   graph          — the workload graph (unchanged structure; the same
    //                    nodes, so succeeded shards map by workload_id +
    //                    shard_index)
    //
    // Returns Ok with 'out' filled: version = previous.version + 1, the
    // succeeded shards' assignments COPIED VERBATIM (same device, same
    // range, marked by presence), the unfinished nodes re-planned by the
    // planner core against the fresh snapshot. Errors: InvalidInput (the
    // planner refused the remaining work — 'out' carries the structured
    // rejection and the PRESERVED succeeded assignments stay visible in
    // 'out').
    vortyx::platform::Status replan(const ComputePlan& previous,
                                    const std::vector<ShardExecutionFeedback>& outcomes,
                                    const WorkloadGraph& graph, const std::string& graph_id,
                                    const vortyx::distributed::ClusterSnapshot& fresh_snapshot,
                                    ComputePlan& out, std::string& error) const;

private:
    const FabricPlanner& planner_;
};

}  // namespace vortyx::fabric
