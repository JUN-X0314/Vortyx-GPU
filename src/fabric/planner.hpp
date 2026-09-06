#pragma once

// FabricPlanner (Phase 16) — the deterministic planning core.
//
// ═══════════════════════════════════════════════════════════════════════
// THE CONTRACT: planning is a PURE function of (WorkloadGraph,
// ClusterSnapshot, FabricPlannerConfig). No registry, no clock, no I/O,
// no global mutable state, no hash-order dependence. The same inputs
// always produce the same plan CONTENT — byte-identical, pinned by tests.
//
// THE PIPELINE (per planning call):
//   1. validate_workload_graph          (structure, cycles, caps)
//   2. planning order                   (smallest-ready-id Kahn — the
//                                        TensorGraph rule, one design)
//   3. per node, in planning order:
//        a. candidates                  = snapshot.candidates_for(owner)
//                                         (ownership + state + health
//                                         filtered — the Phase 12 rule,
//                                         reused, never re-implemented)
//        b. hard filters                = capability claim (device_supports,
//                                         Phase 12) → backend claim →
//                                         exclusion constraints → per-shard
//                                         resource fit (shard_memory_bytes,
//                                         Phase 12's one memory rule)
//        c. scoring                     = score_candidate (cost.hpp, int64)
//        d. selection                   = best score, tie → smaller device
//                                         id; plan-local capacity is
//                                         decremented so two shards of one
//                                         plan cannot overcommit a device
//   4. all-or-nothing: any unplannable node refuses the WHOLE plan with
//      the node's structured reason (a plan that cannot execute is not
//      published as if it could).
//
// WHAT THE PLANNER DOES NOT DO (the boundary that keeps Phase 12 in
// charge): it does not touch the registry, does not take leases, does not
// reserve resources, does not dispatch work. It reads a snapshot and
// returns a plan; the EXECUTION path (the orchestrator, through the policy
// bridge) reserves and runs against the LIVE registry with its own
// stale-plan check. A planner that mutated the cluster would be a second
// scheduler — the thing Phase 16 was told not to build.
// ═══════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>

#include "fabric/cost.hpp"
#include "fabric/graph.hpp"
#include "fabric/plan.hpp"
#include "fabric/workload.hpp"
#include "platform/status.hpp"

namespace vortyx::fabric {

// The planner's stable rejection codes (the plan explanation's vocabulary;
// the policy bridge maps them onto the Phase 12 PlacementRejection codes
// at the execution boundary).
namespace rejection_stage {
inline constexpr const char* kInvalidRequest = "invalid_request";
inline constexpr const char* kClusterEmpty = "cluster_empty";
inline constexpr const char* kUnsupportedCapability = "unsupported_capability";
inline constexpr const char* kBackendUnavailable = "backend_unavailable";
inline constexpr const char* kExcludedConstraint = "excluded_constraint";
inline constexpr const char* kInsufficientResource = "insufficient_resource";
inline constexpr const char* kDeviceUnhealthy = "device_unhealthy";
}  // namespace rejection_stage

// The cross-node plan-local claim record: the bytes earlier nodes/shards
// of ONE planning call already assigned to a device. An implementation
// detail of the planning core (plan-local accounting), exposed in the
// header only because place_node's signature carries it.
struct PlanClaim {
    DeviceId device_id;
    std::int64_t bytes = 0;
};

class FabricPlanner {
public:
    explicit FabricPlanner(FabricPlannerConfig config);

    // The config fingerprint part of plan identity (the version string the
    // plan records — planner_name comes from the plan struct itself).
    const FabricPlannerConfig& config() const { return config_; }

    // Plans the whole graph (pure). Returns Ok with 'plan' filled (version
    // kUnassignedPlanVersion — lineage stamps real versions), or
    // InvalidInput with 'plan' ALSO filled: a refused plan carries the
    // structured rejection (code, node, message) and no nodes. Errors:
    //   graph validation failure        -> InvalidInput (the graph's reason)
    //   node unplannable                -> InvalidInput (the node's reason)
    Status plan_graph(const WorkloadGraph& graph, const std::string& graph_id,
                      const vortyx::distributed::ClusterSnapshot& snapshot,
                      ComputePlan& plan, std::string& error) const;

    // Plans ONE workload (the single-node degenerate graph, exposed for
    // the placement bridge and direct tests). Same purity, same rules.
    // On success the plan carries exactly one node assignment.
    Status plan_single(const WorkloadDescriptor& descriptor, const std::string& workload_id,
                       const vortyx::distributed::ClusterSnapshot& snapshot,
                       ComputePlan& plan, std::string& error) const;

private:
    // The shared per-node decision core: walks the candidates in
    // registration order, applies the hard filters, scores survivors,
    // picks the winner (or fills the structured refusal).
    // 'plan_claimed' carries the capacity EARLIER nodes/shards of this
    // plan already claimed per device (in/out — the planner never
    // double-books a device within one plan).
    bool place_node(const WorkloadDescriptor& descriptor,
                    const std::vector<vortyx::distributed::DeviceSnapshot>& candidates,
                    std::uint32_t shard_count, std::vector<PlanClaim>& plan_claimed,
                    std::vector<PlanShardAssignment>& out_shards,
                    DeviceDecision& out_decision, const char*& out_rejection_code,
                    std::string& error) const;

    FabricPlannerConfig config_;
};

}  // namespace vortyx::fabric
