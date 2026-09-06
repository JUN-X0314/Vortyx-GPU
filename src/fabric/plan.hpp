#pragma once

// ComputePlan (Phase 16) — the fabric planner's output artifact.
//
// ═══════════════════════════════════════════════════════════════════════
// WHAT A PLAN IS: a metadata record of WHAT the fabric decided and WHY —
// the node assignments (which device runs which shard range), the scored
// decision per node (a structured explanation, deterministic to the last
// field), and the plan's identity (workload graph, version, cluster
// revision, planner fingerprint). WHAT A PLAN IS NOT: it carries no result
// payload, no secret, no telemetry, no binary artifact. An executor that
// knows nothing about planner internals can consume it — planner and
// executor are separated by exactly this type.
//
// PLAN IDENTITY AND DETERMINISM (the contract the tests pin):
//   - The PURE planner produces plan CONTENT (assignments + explanations).
//     It always sets plan_version = kUnassignedPlanVersion (0): versioning
//     is a LINEAGE concern (see replan.hpp), not a function of the
//     inputs — the same (graph, snapshot, config) always yields the same
//     content, byte for byte (pinned by serialize-equality tests).
//   - plan_version is stamped by whoever publishes the plan. Versions are
//     monotonic per workload graph (1, 2, 3, ...) and a version never
//     names two different contents (replan.hpp owns the rule).
//   - cluster_revision is the snapshot revision the plan was computed
//     from. plan_is_stale(plan, snapshot) is the pure stale rule: the
//     plan's revision no longer matches the current cluster — a stale
//     plan is RE-PLANNED, never force-executed (the Phase 12 policy,
//     carried up into the fabric).
//
// SERIALIZATION: serialize_compute_plan produces the strict platform JSON
// (Phase 11 writer, canonical field order — the same plan always
// serializes to the same bytes). Deserialization is intentionally ABSENT:
// the plan is produced and consumed in-process by the layers that own it;
// no wire path carries plans in Phase 16 (an API exposure serializes
// the SUMMARY fields it actually serves — see contract_fabric.hpp).
// ═══════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <vector>

#include "distributed/cluster.hpp" // ClusterSnapshot (plan_is_stale's input)
#include "distributed/shard.hpp"   // ElementRange (the one partition vocabulary)
#include "fabric/cost.hpp"
#include "fabric/graph.hpp"
#include "fabric/workload.hpp"
#include "platform/identity.hpp"

namespace vortyx::fabric {

using vortyx::platform::DeviceId;
using vortyx::distributed::ElementRange;

// The version the pure planner leaves (lineage stamps real versions).
inline constexpr std::uint32_t kUnassignedPlanVersion = 0;

// How many per-candidate rejections a node's explanation retains. The
// candidates are walked in REGISTRATION order, so the retained set is
// deterministic (the first N, plus honest counts of the rest — never a
// sample of an unordered map).
inline constexpr std::size_t kMaxRecordedRejections = 8;

// One stable rejection stage (why a candidate did not win). The codes are
// the plan explanation's vocabulary — lowercase snake_case, the project's
// error-code style. They MAP to the Phase 12 PlacementRejection codes at
// the policy boundary; the explanation keeps its own fine-grained set.
struct CandidateRejection {
    DeviceId device_id;
    const char* stage = "";  // stable code, see the planner's stage table

    friend bool operator==(const CandidateRejection& a, const CandidateRejection& b) {
        return a.device_id == b.device_id && std::string(a.stage) == std::string(b.stage);
    }
    friend bool operator!=(const CandidateRejection& a, const CandidateRejection& b) {
        return !(a == b);
    }
};

// The counters behind a bounded rejection list (deterministic — counted
// over the registration-order walk).
struct RejectionSummary {
    std::size_t recorded = 0;                       // entries retained below
    std::size_t total_rejected = 0;                 // all candidates that failed
    std::size_t total_capable = 0;                  // capability filter survivors

    friend bool operator==(const RejectionSummary& a, const RejectionSummary& b) {
        return a.recorded == b.recorded && a.total_rejected == b.total_rejected &&
               a.total_capable == b.total_capable;
    }
    friend bool operator!=(const RejectionSummary& a, const RejectionSummary& b) {
        return !(a == b);
    }
};

// The structured decision for one node (the "why this device" record the
// explainability contract demands — every field deterministic, in
// registration-order walk order, no unordered anything).
struct DeviceDecision {
    DeviceId device_id;          // the winner ("" when the node was refused)
    ScoreBreakdown score;        // the winner's named components
    std::vector<CandidateRejection> rejected;  // first kMaxRecordedRejections, in walk order
    RejectionSummary rejection_summary;

    friend bool operator==(const DeviceDecision& a, const DeviceDecision& b) {
        return a.device_id == b.device_id && a.score == b.score &&
               a.rejected == b.rejected && a.rejection_summary == b.rejection_summary;
    }
    friend bool operator!=(const DeviceDecision& a, const DeviceDecision& b) {
        return !(a == b);
    }
};

// One shard of one node, assigned.
struct PlanShardAssignment {
    std::uint32_t shard_index = 0;   // ascending within the node, always
    ElementRange range;              // the data-parallel slice (Phase 12 type)
    DeviceId device_id;              // where it will run

    friend bool operator==(const PlanShardAssignment& a, const PlanShardAssignment& b) {
        return a.shard_index == b.shard_index && a.range == b.range &&
               a.device_id == b.device_id;
    }
    friend bool operator!=(const PlanShardAssignment& a, const PlanShardAssignment& b) {
        return !(a == b);
    }
};

// One node's planned placement (assignment + the decision record).
struct PlanNodeAssignment {
    WorkloadNodeId node_id = kInvalidWorkloadNodeId;
    std::string workload_id;
    std::vector<PlanShardAssignment> shards;  // ascending shard_index
    DeviceDecision decision;

    friend bool operator==(const PlanNodeAssignment& a, const PlanNodeAssignment& b) {
        return a.node_id == b.node_id && a.workload_id == b.workload_id &&
               a.shards == b.shards && a.decision == b.decision;
    }
    friend bool operator!=(const PlanNodeAssignment& a, const PlanNodeAssignment& b) {
        return !(a == b);
    }
};

// Why a plan was refused (the plan-level failure record — machine-readable
// first, the message is the human echo).
struct PlanRejection {
    const char* code = "";       // "" = the plan was accepted
    std::string node_workload_id;  // the refusing node ("" when graph-level)
    std::string message;

    friend bool operator==(const PlanRejection& a, const PlanRejection& b) {
        return std::string(a.code) == std::string(b.code) &&
               a.node_workload_id == b.node_workload_id && a.message == b.message;
    }
    friend bool operator!=(const PlanRejection& a, const PlanRejection& b) {
        return !(a == b);
    }
};

// The plan (see the module header). A plain value: copyable, comparable,
// deterministically serializable.
struct ComputePlan {
    // Identity.
    std::string graph_id;       // the planned workload graph's id (caller-set)
    std::uint32_t plan_version = kUnassignedPlanVersion;  // stamped by lineage
    std::uint64_t cluster_revision = 0;  // the snapshot this plan is based on
    std::string planner_name = "adaptive_fabric";
    std::string planner_version;  // the fabric's version (VORTYX_VERSION_STRING)

    // The plan content, in the graph's deterministic planning order.
    std::vector<PlanNodeAssignment> nodes;

    // The refusal, when the plan was not accepted (nodes empty then).
    PlanRejection rejection;

    friend bool operator==(const ComputePlan& a, const ComputePlan& b) {
        return a.graph_id == b.graph_id && a.plan_version == b.plan_version &&
               a.cluster_revision == b.cluster_revision &&
               a.planner_name == b.planner_name && a.planner_version == b.planner_version &&
               a.nodes == b.nodes && a.rejection == b.rejection;
    }
    friend bool operator!=(const ComputePlan& a, const ComputePlan& b) { return !(a == b); }
};

// The stale rule (pure): a plan whose recorded cluster revision differs
// from the snapshot's current revision is STALE — re-plan, never force.
bool plan_is_stale(const ComputePlan& plan, const vortyx::distributed::ClusterSnapshot& snapshot);

// Deterministic strict-JSON serialization (canonical field order; the same
// plan always produces the same bytes — pinned by tests). The version
// field serializes as assigned (0 = the pure planner's unassigned content).
std::string serialize_compute_plan(const ComputePlan& plan);

}  // namespace vortyx::fabric
