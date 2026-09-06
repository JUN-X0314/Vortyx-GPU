#pragma once

// WorkloadGraph (Phase 16) — a deterministic dependency graph of workloads.
//
// RELATIONSHIP TO TensorGraph (Phase 13): the SAME structural design, on
// purpose and after reading it — node ids are assigned by insertion order
// (1-based, 0 = invalid), the execution order is the smallest-ready-id-
// first Kahn topological order, two-phase binding is allowed so cycles are
// POSSIBLE and are then caught by validation (which names the nodes), and
// explicit size caps bound any hostile submission. One structural design
// across the two graphs means one mental model and one set of pinned
// properties. The tensor layer's fabric_adapter converts TensorGraph
// nodes into WorkloadNodes (see tensor/fabric_adapter.hpp) instead of the
// fabric growing a second graph type.
//
// WHAT A GRAPH IS FOR: the fabric plans a GRAPH (not just one workload) —
// dependency order constrains planning order (a node is planned only
// after its producers), so a plan's assignments respect data dependencies
// structurally. The graph itself EXECUTES nothing: it is metadata the
// planner consumes.
//
// VALIDATION (validate, pure): every node's descriptor valid, every
// dependency resolvable and acyclic, duplicate workload ids refused,
// duplicate dependencies refused, node/dependency caps enforced. All
// refusals use the platform Status vocabulary (the fabric, like the
// distributed layer, speaks the control-plane outcomes — no second enum).

#include <cstdint>
#include <string>
#include <vector>

#include "fabric/workload.hpp"
#include "platform/status.hpp"

namespace vortyx::fabric {

// Explicit graph size caps (the TensorGraph pattern — resource-exhaustion
// defense for any future remote-submission path; the same generous-for-
// real-graphs, small-enough-to-refuse-abuse scale).
inline constexpr std::uint32_t kMaxWorkloadGraphNodes = 256;
inline constexpr std::uint32_t kMaxWorkloadDependencies = 16;

using WorkloadNodeId = std::uint32_t;
inline constexpr WorkloadNodeId kInvalidWorkloadNodeId = 0;

// One node: a workload descriptor plus its producers (the nodes that must
// be planned — and by execution-time dependency, run — before it).
struct WorkloadNode {
    WorkloadNodeId node_id = kInvalidWorkloadNodeId;  // assigned by the graph
    WorkloadDescriptor descriptor;
    std::vector<WorkloadNodeId> dependencies;  // node ids; duplicates refused
};

class WorkloadGraph {
public:
    WorkloadGraph() = default;

    // Adds a node with dependencies bound immediately. The node id is
    // returned (insertion order, 1-based). Unknown dependency ids are
    // refused at bind time (a dependency on a node that does not exist yet
    // is declared with add_node + bind below, or simply added after its
    // producers — the graph is built producer-first in practice).
    // Errors: InvalidInput (bad descriptor / unknown dependency /
    // duplicate dependency / duplicate workload_id / caps exceeded).
    Status add_node(const WorkloadDescriptor& descriptor,
                    const std::vector<WorkloadNodeId>& dependencies, WorkloadNodeId& out_id,
                    std::string& error);

    // Binds one more dependency to an existing node. Re-binding the same
    // dependency is a NO-OP (idempotent — deterministic); an unknown node
    // or dependency id is refused.
    Status bind_dependency(WorkloadNodeId node_id, WorkloadNodeId dependency_id,
                           std::string& error);

    // -- inspection ---------------------------------------------------------

    const std::vector<WorkloadNode>& nodes() const { return nodes_; }
    std::uint32_t node_count() const { return static_cast<std::uint32_t>(nodes_.size()); }
    const WorkloadNode* find(WorkloadNodeId node_id) const;  // nullptr when unknown

private:
    std::vector<WorkloadNode> nodes_;  // insertion order; node_id == index + 1
};

// The validation result: the deterministic topological planning order.
struct WorkloadGraphValidation {
    std::vector<WorkloadNodeId> planning_order;  // node ids, topo order
};

// Full validation (pure). Returns Ok and fills 'validation', or the
// precise failing status with 'error' naming the offending node:
//   invalid descriptor              -> InvalidInput (names the node)
//   dependency on unknown node      -> InvalidInput
//   cycle                           -> InvalidInput (names the nodes)
//   node/dependency caps            -> InvalidInput (the size limit is part
//                                      of the request contract)
Status validate_workload_graph(const WorkloadGraph& graph, WorkloadGraphValidation& validation,
                               std::string& error);

}  // namespace vortyx::fabric
