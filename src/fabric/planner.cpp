// FabricPlanner implementation (Phase 16) — see planner.hpp.

#include "fabric/planner.hpp"

#include <algorithm>
#include <limits>
#include <vector>

#include "core/version.hpp"          // VORTYX_VERSION_STRING (plan fingerprint)
#include "distributed/device.hpp"    // device_supports (the capability rule)
#include "distributed/resource.hpp"  // shard_memory_bytes (the one memory rule)
#include "distributed/shard.hpp"     // partition_element_count (the one partition rule)

namespace vortyx::fabric {

using vortyx::distributed::ClusterSnapshot;
using vortyx::distributed::DeviceSnapshot;
using vortyx::distributed::ElementRange;
using vortyx::platform::Status;

namespace {

// True when 'device_id' is excluded by the descriptor's constraints.
bool is_excluded(const WorkloadDescriptor& descriptor, const DeviceId& device_id) {
    return std::find(descriptor.excluded_devices.begin(), descriptor.excluded_devices.end(),
                     device_id) != descriptor.excluded_devices.end();
}

}  // namespace

FabricPlanner::FabricPlanner(FabricPlannerConfig config) : config_(config) {
    // No throw, no silent fix: an invalid config is refused at planning
    // time (plan_graph checks and refuses with the structured rejection).
}

bool FabricPlanner::place_node(const WorkloadDescriptor& descriptor,
                               const std::vector<DeviceSnapshot>& candidates,
                               std::uint32_t shard_count,
                               std::vector<PlanClaim>& plan_claimed,
                               std::vector<PlanShardAssignment>& out_shards,
                               DeviceDecision& out_decision, const char*& out_rejection_code,
                               std::string& error) const {
    out_shards.clear();
    out_decision = DeviceDecision{};
    out_rejection_code = nullptr;

    // The shard partition is the Phase 12 rule (exact cover, no empty
    // shards, deterministic ranges) — the fabric does not grow a second
    // one. The effective shard count is min(requested, element_count).
    std::vector<ElementRange> ranges;
    if (vortyx::distributed::partition_element_count(descriptor.element_count, shard_count,
                                                     ranges, error) != Status::Ok) {
        out_rejection_code = rejection_stage::kInvalidRequest;
        error = "workload " + descriptor.workload_id + ": " + error;
        return false;
    }
    const std::size_t effective_shards = ranges.size();

    // Per-shard memory requirement — the Phase 12 rule (one definition of
    // "how much memory does a shard of this operation need").
    std::vector<std::int64_t> shard_bytes(effective_shards, 0);
    for (std::size_t i = 0; i < effective_shards; ++i) {
        if (!vortyx::distributed::shard_memory_bytes(ranges[i].size(), descriptor.operation,
                                                     shard_bytes[i], error)) {
            out_rejection_code = rejection_stage::kInvalidRequest;
            error = "workload " + descriptor.workload_id + ": " + error;
            return false;
        }
    }

    // Capability census (one deterministic walk, registration order): how
    // many candidates claim the operation, how many are hard-rejected.
    std::size_t total_capable = 0;
    std::size_t total_excluded_capable = 0;
    for (const DeviceSnapshot& device : candidates) {
        if (!vortyx::distributed::device_supports(device.capabilities, descriptor.operation,
                                                  descriptor.requested_backend)) {
            continue;
        }
        ++total_capable;
        if (is_excluded(descriptor, device.device_id)) ++total_excluded_capable;
    }

    // ---- per-shard placement (deterministic) ------------------------------
    // For each shard in ascending order: walk capable, non-excluded
    // candidates in registration order; require resource fit against the
    // plan-local availability (snapshot minus what EARLIER shards and
    // nodes of this plan already claimed on the device); score; keep the
    // best by the documented ordering rule (score desc, device id asc).
    for (std::size_t shard = 0; shard < effective_shards; ++shard) {
        const DeviceSnapshot* best_device = nullptr;
        ScoreBreakdown best_breakdown;

        for (const DeviceSnapshot& device : candidates) {
            if (!vortyx::distributed::device_supports(device.capabilities, descriptor.operation,
                                                      descriptor.requested_backend)) {
                continue;
            }
            if (is_excluded(descriptor, device.device_id)) continue;

            std::int64_t available = device.available().memory_bytes;
            for (const PlanClaim& claim : plan_claimed) {
                if (claim.device_id == device.device_id) available -= claim.bytes;
            }
            if (available < shard_bytes[shard]) continue;  // resource fit failed

            ScoreInputs inputs;
            inputs.candidate = &device;
            inputs.needed_memory_bytes = shard_bytes[shard];
            inputs.locality_match = !descriptor.preferred_device.empty() &&
                                    descriptor.preferred_device == device.device_id;
            const std::string preferred = device.capabilities.preferred_backend();
            inputs.backend_match = !descriptor.requested_backend.empty() &&
                                   preferred == descriptor.requested_backend;

            ScoreBreakdown breakdown;
            if (!score_candidate(inputs, config_, breakdown, error)) {
                // An overflow refusal is honest: this candidate cannot be
                // ranked, so it cannot win. Skip it — never wrap the score.
                continue;
            }
            if (best_device == nullptr ||
                candidate_ranks_before(breakdown, device.device_id, best_breakdown,
                                       best_device->device_id)) {
                best_device = &device;
                best_breakdown = breakdown;
            }
        }

        if (best_device == nullptr) {
            // No candidate holds this shard. The stage code is derived
            // deterministically from what the walk saw:
            if (candidates.empty()) {
                out_rejection_code = rejection_stage::kClusterEmpty;
            } else if (total_capable == 0) {
                out_rejection_code = rejection_stage::kUnsupportedCapability;
            } else if (total_excluded_capable == total_capable) {
                out_rejection_code = rejection_stage::kExcludedConstraint;
            } else {
                out_rejection_code = rejection_stage::kInsufficientResource;
            }
            error = "workload " + descriptor.workload_id + ": shard " +
                    std::to_string(shard) + " cannot be placed (" +
                    (out_rejection_code != nullptr ? out_rejection_code : "unknown") + ")";
            return false;
        }

        PlanShardAssignment assignment;
        assignment.shard_index = static_cast<std::uint32_t>(shard);
        assignment.range = ranges[shard];
        assignment.device_id = best_device->device_id;
        out_shards.push_back(assignment);
        plan_claimed.push_back({best_device->device_id, shard_bytes[shard]});

        // The node's decision records SHARD 0's winner and score (the
        // device that runs the first — and deterministically largest —
        // share). Later shards' choices are visible in the assignments.
        if (shard == 0) {
            out_decision.device_id = best_device->device_id;
            out_decision.score = best_breakdown;
        }
    }

    // ---- the rejection record (deterministic) -----------------------------
    // Every candidate that is not a winner gets its hard-failure stage:
    // capability first, then exclusion, then "capable but never fit/won"
    // (insufficient_resource). The walk is registration order; the list is
    // bounded (kMaxRecordedRejections) and the summary counts are honest.
    for (const DeviceSnapshot& device : candidates) {
        if (device.device_id == out_decision.device_id) continue;  // the winner
        bool won_any = false;
        for (const PlanClaim& claim : plan_claimed) {
            if (claim.device_id == device.device_id) {
                won_any = true;
                break;
            }
        }
        if (won_any) continue;  // multi-shard co-winner — not rejected
        const char* stage;
        if (!vortyx::distributed::device_supports(device.capabilities, descriptor.operation,
                                                  descriptor.requested_backend)) {
            stage = rejection_stage::kUnsupportedCapability;
        } else if (is_excluded(descriptor, device.device_id)) {
            stage = rejection_stage::kExcludedConstraint;
        } else {
            stage = rejection_stage::kInsufficientResource;
        }
        if (out_decision.rejected.size() < kMaxRecordedRejections) {
            out_decision.rejected.push_back({device.device_id, stage});
        }
        ++out_decision.rejection_summary.total_rejected;
    }
    out_decision.rejection_summary.recorded = out_decision.rejected.size();
    out_decision.rejection_summary.total_capable = total_capable;
    return true;
}

Status FabricPlanner::plan_graph(const WorkloadGraph& graph, const std::string& graph_id,
                                 const ClusterSnapshot& snapshot, ComputePlan& plan,
                                 std::string& error) const {
    plan = ComputePlan{};
    plan.graph_id = graph_id;
    plan.cluster_revision = snapshot.revision;
    plan.planner_version = VORTYX_VERSION_STRING;

    if (config_.validate(error) != Status::Ok) {
        plan.rejection = {rejection_stage::kInvalidRequest, "", error};
        return Status::InvalidInput;
    }

    WorkloadGraphValidation validation;
    if (validate_workload_graph(graph, validation, error) != Status::Ok) {
        plan.rejection = {rejection_stage::kInvalidRequest, "", error};
        return Status::InvalidInput;
    }

    // The planning order: Kahn's algorithm over the dependency structure,
    // with the ready set ordered by (priority DESC, node id ASC) — the
    // descriptor priority's one real effect: higher-priority workloads
    // claim resources first among structurally ready nodes. Fully
    // deterministic; no clock, no iteration order.
    const std::vector<WorkloadNode>& nodes = graph.nodes();
    std::vector<std::size_t> pending(nodes.size() + 1, 0);
    std::vector<WorkloadNodeId> ready;
    for (const WorkloadNode& node : nodes) {
        pending[node.node_id] = node.dependencies.size();
        if (node.dependencies.empty()) ready.push_back(node.node_id);
    }

    std::vector<WorkloadNodeId> order;
    order.reserve(nodes.size());
    while (!ready.empty()) {
        // Pick the ready node with the highest priority; ties by smallest
        // node id (stable identifiers as tie-breakers).
        std::size_t best_index = 0;
        for (std::size_t i = 1; i < ready.size(); ++i) {
            const WorkloadNode& a = *graph.find(ready[i]);
            const WorkloadNode& b = *graph.find(ready[best_index]);
            if (a.descriptor.priority > b.descriptor.priority ||
                (a.descriptor.priority == b.descriptor.priority &&
                 a.node_id < b.node_id)) {
                best_index = i;
            }
        }
        const WorkloadNodeId node_id = ready[best_index];
        ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(best_index));
        order.push_back(node_id);

        for (const WorkloadNode& node : nodes) {
            if (std::find(node.dependencies.begin(), node.dependencies.end(), node_id) ==
                node.dependencies.end()) {
                continue;
            }
            if (--pending[node.node_id] == 0) {
                ready.push_back(node.node_id);
            }
        }
    }
    // (order.size() == nodes.size() is guaranteed by validate_workload_graph
    // having just passed — the cycle case was refused above.)

    std::vector<PlanClaim> plan_claimed;

    for (const WorkloadNodeId node_id : order) {
        const WorkloadNode* node = graph.find(node_id);
        const WorkloadDescriptor& descriptor = node->descriptor;

        // Ownership-scoped candidates — the Phase 12 rule reused verbatim:
        // a planner never even SEES another user's devices.
        const std::vector<DeviceSnapshot> candidates =
            snapshot.candidates_for(descriptor.owner_user_id);

        PlanNodeAssignment assignment;
        assignment.node_id = node_id;
        assignment.workload_id = descriptor.workload_id;

        const char* rejection_code = nullptr;
        if (!place_node(descriptor, candidates, descriptor.preferred_shard_count,
                        plan_claimed, assignment.shards, assignment.decision,
                        rejection_code, error)) {
            plan.rejection = {rejection_code != nullptr ? rejection_code
                                                        : rejection_stage::kInvalidRequest,
                              descriptor.workload_id, error};
            return Status::InvalidInput;
        }
        plan.nodes.push_back(std::move(assignment));
    }
    return Status::Ok;
}

Status FabricPlanner::plan_single(const WorkloadDescriptor& descriptor,
                                  const std::string& workload_id,
                                  const ClusterSnapshot& snapshot, ComputePlan& plan,
                                  std::string& error) const {
    WorkloadGraph graph;
    WorkloadDescriptor single = descriptor;
    single.workload_id = workload_id;
    WorkloadNodeId id = kInvalidWorkloadNodeId;
    Status status = graph.add_node(single, {}, id, error);
    if (status != Status::Ok) return status;
    return plan_graph(graph, workload_id, snapshot, plan, error);
}

}  // namespace vortyx::fabric
