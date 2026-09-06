#pragma once

// FabricPolicy (Phase 16) — the bridge that carries fabric plans INTO the
// unchanged Phase 12 execution path.
//
// ═══════════════════════════════════════════════════════════════════════
// THE MECHANISM (and why it is the honest one): Phase 12's orchestrator
// already has exactly one placement seam — ISchedulingPolicy. Its
// plan(request, snapshot) is called at every placement point (initial
// placement and per-shard retry re-placement). FabricPolicy IMPLEMENTS
// that interface and answers with the fabric planner's decision:
//
//   - The ORCHESTRATOR keeps everything it owns: the stale-plan check
//     against the live registry revision, the atomic leases, the retry
//     waves, the checkpoint semantics that never re-run succeeded shards,
//     cancellation, terminal transitions. NOTHING about Phase 12
//     execution changes.
//   - The FABRIC owns the placement decision: every plan(request, ...)
//     call goes through FabricPlanner (the deterministic core), and every
//     published plan is recorded in the per-job lineage (version 1, 2, 3,
//     ... — each re-placement event with different content is a new
//     version).
//   - The map from the orchestrator's PlacementRequest to the fabric's
//     WorkloadDescriptor is derive-only (operation, size, backend,
//     owner, exclusions) — no new submission contract exists.
//
// THREADING: plan() is called under the orchestrator's placement
// serialization (one submit at a time per record; the registry has its
// own lock). The policy's per-job state is mutated only inside plan();
// the LOOKUPS (lineage_for, last_plan_for, counters) are safe for
// concurrent readers only if the caller synchronizes — the service reads
// them after its dispatcher's submit() returns (happens-before via the
// dispatch handoff). Documented, not hidden behind a lock that would
// pretend more than it guarantees.
// ═══════════════════════════════════════════════════════════════════════

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "distributed/policy.hpp"  // ISchedulingPolicy / PlacementRequest / PlacementPlan
#include "fabric/cost.hpp"
#include "fabric/planner.hpp"
#include "fabric/replan.hpp"
#include "platform/status.hpp"

namespace vortyx::fabric {

// The policy's configuration name (the value the orchestrator-side
// diagnostics report when this policy is active).
inline constexpr const char* kFabricPolicyName = "adaptive_fabric";

// The fabric planner's real counters (measured, never estimated): every
// increment is one real event the policy observed.
struct FabricPolicyCounters {
    std::uint64_t plan_invocations = 0;     // plan() calls
    std::uint64_t plans_published = 0;      // accepted placements
    std::uint64_t plan_rejections = 0;      // refused placements
    std::uint64_t replans = 0;              // versions beyond 1 recorded
    std::uint64_t stale_refusals = 0;       // plan() refused a stale snapshot
};

class FabricPolicy final : public vortyx::distributed::ISchedulingPolicy {
public:
    explicit FabricPolicy(FabricPlannerConfig config);

    const char* name() const override { return kFabricPolicyName; }

    // The ISchedulingPolicy seam: plans the request through the fabric
    // planner and returns the Phase 12 placement the orchestrator will
    // execute (leases, stale checks and all — unchanged). A refused plan
    // maps onto the Phase 12 PlacementRejection codes so the orchestrator's
    // reporting stays in ITS vocabulary.
    vortyx::distributed::PlacementPlan plan(
        const vortyx::distributed::PlacementRequest& request,
        const vortyx::distributed::ClusterSnapshot& snapshot) override;

    // The per-job lineage (nullptr when this policy never planned for the
    // job — the honest "not planned here" answer).
    const PlanLineage* lineage_for(const vortyx::platform::JobId& job_id) const;
    // The latest published plan for a job (nullptr when none).
    const ComputePlan* last_plan_for(const vortyx::platform::JobId& job_id) const;

    // The real counters (a snapshot — the policy keeps counting).
    FabricPolicyCounters counters() const { return counters_; }

    // Forgets one job's lineage (the terminal cleanup — bounded state,
    // the same discipline the orchestrator applies to its request map).
    void forget(const vortyx::platform::JobId& job_id);

private:
    FabricPlanner planner_;
    std::unordered_map<vortyx::platform::JobId, PlanLineage> lineages_;
    FabricPolicyCounters counters_;
};

// Builds the orchestrator-ready policy (a convenience for the service and
// the tests — the same construction, one place).
std::unique_ptr<FabricPolicy> make_fabric_policy(FabricPlannerConfig config);

}  // namespace vortyx::fabric
