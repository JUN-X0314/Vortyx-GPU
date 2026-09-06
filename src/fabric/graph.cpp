// WorkloadGraph implementation (Phase 16) — see graph.hpp.

#include "fabric/graph.hpp"

#include <algorithm>
#include <set>

namespace vortyx::fabric {

Status WorkloadGraph::add_node(const WorkloadDescriptor& descriptor,
                               const std::vector<WorkloadNodeId>& dependencies,
                               WorkloadNodeId& out_id, std::string& error) {
    if (nodes_.size() >= kMaxWorkloadGraphNodes) {
        error = "workload graph node cap (" + std::to_string(kMaxWorkloadGraphNodes) +
                ") exceeded";
        return Status::InvalidInput;
    }
    if (validate_workload_descriptor(descriptor, error) != Status::Ok) {
        return Status::InvalidInput;
    }
    // Duplicate workload ids are refused: a graph node IS its workload
    // identity, and two nodes claiming one identity would make plan
    // lineage ambiguous.
    for (const WorkloadNode& node : nodes_) {
        if (node.descriptor.workload_id == descriptor.workload_id) {
            error = "duplicate workload_id '" + descriptor.workload_id + "' in graph";
            return Status::InvalidInput;
        }
    }
    WorkloadNode node;
    node.descriptor = descriptor;
    for (const WorkloadNodeId dependency : dependencies) {
        if (dependency == kInvalidWorkloadNodeId || dependency > nodes_.size()) {
            error = "node '" + descriptor.workload_id +
                    "' declares dependency on unknown node id " + std::to_string(dependency);
            return Status::InvalidInput;
        }
        if (std::find(node.dependencies.begin(), node.dependencies.end(), dependency) !=
            node.dependencies.end()) {
            continue;  // idempotent duplicate binding (documented)
        }
        if (node.dependencies.size() >= kMaxWorkloadDependencies) {
            error = "node '" + descriptor.workload_id + "' exceeds the dependency cap (" +
                    std::to_string(kMaxWorkloadDependencies) + ")";
            return Status::InvalidInput;
        }
        node.dependencies.push_back(dependency);
    }
    out_id = static_cast<WorkloadNodeId>(nodes_.size() + 1);
    node.node_id = out_id;
    nodes_.push_back(std::move(node));
    return Status::Ok;
}

Status WorkloadGraph::bind_dependency(WorkloadNodeId node_id, WorkloadNodeId dependency_id,
                                      std::string& error) {
    WorkloadNode* node = nullptr;
    if (node_id == kInvalidWorkloadNodeId || node_id > nodes_.size()) {
        error = "bind_dependency: unknown node id " + std::to_string(node_id);
        return Status::InvalidInput;
    }
    node = &nodes_[node_id - 1];
    if (dependency_id == kInvalidWorkloadNodeId || dependency_id > nodes_.size()) {
        error = "bind_dependency: unknown dependency id " + std::to_string(dependency_id);
        return Status::InvalidInput;
    }
    if (std::find(node->dependencies.begin(), node->dependencies.end(), dependency_id) !=
        node->dependencies.end()) {
        return Status::Ok;  // already bound (idempotent)
    }
    if (node->dependencies.size() >= kMaxWorkloadDependencies) {
        error = "node '" + node->descriptor.workload_id + "' exceeds the dependency cap (" +
                std::to_string(kMaxWorkloadDependencies) + ")";
        return Status::InvalidInput;
    }
    node->dependencies.push_back(dependency_id);
    return Status::Ok;
}

const WorkloadNode* WorkloadGraph::find(WorkloadNodeId node_id) const {
    if (node_id == kInvalidWorkloadNodeId || node_id > nodes_.size()) return nullptr;
    return &nodes_[node_id - 1];
}

Status validate_workload_graph(const WorkloadGraph& graph, WorkloadGraphValidation& validation,
                               std::string& error) {
    validation.planning_order.clear();
    if (graph.node_count() == 0) {
        error = "workload graph has no nodes";
        return Status::InvalidInput;
    }

    // Per-node descriptor validation (the graph's structural rules —
    // duplicate ids and unknown dependencies — are refused at build time;
    // this pass re-checks the descriptors so validation is self-contained).
    for (const WorkloadNode& node : graph.nodes()) {
        if (validate_workload_descriptor(node.descriptor, error) != Status::Ok) {
            error = "node " + std::to_string(node.node_id) + " (" +
                    node.descriptor.workload_id + "): " + error;
            return Status::InvalidInput;
        }
    }

    // Kahn's algorithm, smallest-ready-node-id first — the SAME ordering
    // rule TensorGraph pins (one structural design across the two graphs).
    std::vector<std::size_t> pending_dependencies(graph.node_count() + 1, 0);
    std::set<WorkloadNodeId> ready;
    for (const WorkloadNode& node : graph.nodes()) {
        pending_dependencies[node.node_id] = node.dependencies.size();
        if (node.dependencies.empty()) ready.insert(node.node_id);
    }

    while (!ready.empty()) {
        const WorkloadNodeId node_id = *ready.begin();  // smallest id first
        ready.erase(ready.begin());
        validation.planning_order.push_back(node_id);
        // Release dependents.
        for (const WorkloadNode& node : graph.nodes()) {
            if (std::find(node.dependencies.begin(), node.dependencies.end(), node_id) ==
                node.dependencies.end()) {
                continue;
            }
            if (--pending_dependencies[node.node_id] == 0) {
                ready.insert(node.node_id);
            }
        }
    }

    if (validation.planning_order.size() != graph.node_count()) {
        // Name the nodes still stuck in the cycle — a cycle must be a loud,
        // precise error, never a silent partial plan.
        std::string stuck;
        for (const WorkloadNode& node : graph.nodes()) {
            if (pending_dependencies[node.node_id] != 0) {
                if (!stuck.empty()) stuck += ", ";
                stuck += node.descriptor.workload_id;
            }
        }
        error = "workload graph contains a cycle involving: " + stuck;
        return Status::InvalidInput;
    }
    return Status::Ok;
}

}  // namespace vortyx::fabric
