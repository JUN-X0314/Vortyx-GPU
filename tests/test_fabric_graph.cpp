// Phase 16 — Adaptive Compute Fabric: WorkloadGraph structure and
// validation (test_fabric_graph.cpp).
//
// Pins: deterministic node ids, duplicate-id refusal, dependency binding
// rules and caps, cycle detection (named nodes), and the deterministic
// topological planning order (smallest-ready-id Kahn — the TensorGraph
// rule, one design across the two graphs).

#include <iostream>
#include <string>
#include <vector>

#include "fabric/graph.hpp"

using namespace vortyx;
using namespace vortyx::fabric;

namespace {

int failures = 0;

void check(bool ok, const std::string& name) {
    if (ok) {
        std::cout << "PASS: " << name << "\n";
    } else {
        std::cout << "FAIL: " << name << "\n";
        ++failures;
    }
}

WorkloadDescriptor descriptor(const std::string& id) {
    WorkloadDescriptor d;
    d.workload_id = id;
    d.owner_user_id = "user-1";
    d.element_count = 100;
    return d;
}

}  // namespace

int main() {
    // ---- construction -----------------------------------------------------
    {
        WorkloadGraph graph;
        WorkloadNodeId a = 0, b = 0, c = 0;
        std::string error;
        check(graph.add_node(descriptor("a"), {}, a, error) == Status::Ok && a == 1,
              "graph: first node gets id 1 (insertion order)");
        check(graph.add_node(descriptor("b"), {a}, b, error) == Status::Ok && b == 2,
              "graph: second node gets id 2");
        check(graph.add_node(descriptor("c"), {a, b}, c, error) == Status::Ok,
              "graph: multi-dependency node accepted");

        WorkloadNodeId dup = 0;
        check(graph.add_node(descriptor("a"), {}, dup, error) == Status::InvalidInput,
              "graph: duplicate workload_id refused");

        WorkloadNodeId unknown = 0;
        check(graph.add_node(descriptor("d"), {99}, unknown, error) == Status::InvalidInput,
              "graph: unknown dependency refused at bind time");
        check(graph.node_count() == 3, "graph: refusals leave the graph untouched");
    }

    // ---- validation + planning order -------------------------------------
    {
        WorkloadGraph graph;
        WorkloadNodeId a = 0, b = 0, c = 0, d = 0;
        std::string error;
        graph.add_node(descriptor("a"), {}, a, error);
        graph.add_node(descriptor("b"), {}, b, error);
        graph.add_node(descriptor("c"), {a, b}, c, error);
        graph.add_node(descriptor("d"), {c}, d, error);

        WorkloadGraphValidation validation;
        check(validate_workload_graph(graph, validation, error) == Status::Ok,
              "validation: acyclic graph accepted");
        // Both roots ready -> smallest id first; c only after a AND b.
        check(validation.planning_order.size() == 4 &&
                  validation.planning_order[0] == a && validation.planning_order[1] == b &&
                  validation.planning_order[2] == c && validation.planning_order[3] == d,
              "validation: smallest-ready-id-first topological order");
    }

    // ---- cycle detection ---------------------------------------------------
    {
        WorkloadGraph graph;
        WorkloadNodeId a = 0, b = 0, c = 0;
        std::string error;
        graph.add_node(descriptor("a"), {}, a, error);
        graph.add_node(descriptor("b"), {a}, b, error);
        graph.add_node(descriptor("c"), {b}, c, error);
        // Close the cycle: a depends on c (two-phase binding CAN create one).
        check(graph.bind_dependency(a, c, error) == Status::Ok,
              "cycle: late binding accepted (the graph allows cycles to exist)");
        WorkloadGraphValidation validation;
        check(validate_workload_graph(graph, validation, error) == Status::InvalidInput &&
                  error.find("cycle") != std::string::npos &&
                  error.find("a") != std::string::npos,
              "cycle: detected and named, never silently planned");
    }

    // ---- self-dependency + caps ------------------------------------------
    {
        WorkloadGraph graph;
        WorkloadNodeId a = 0;
        std::string error;
        graph.add_node(descriptor("a"), {}, a, error);
        check(graph.bind_dependency(a, a, error) == Status::Ok,
              "self-dep: binding accepted (validation must catch it)");
        WorkloadGraphValidation validation;
        check(validate_workload_graph(graph, validation, error) == Status::InvalidInput,
              "self-dep: a self-dependency is a cycle");

        // The node cap.
        WorkloadGraph big;
        WorkloadNodeId last = 0;
        bool cap_hit = false;
        for (std::uint32_t i = 0; i < kMaxWorkloadGraphNodes + 1; ++i) {
            WorkloadNodeId id = 0;
            const Status status = big.add_node(descriptor("n" + std::to_string(i)), {}, id, error);
            if (status != Status::Ok) {
                cap_hit = true;
                break;
            }
            last = id;
        }
        check(cap_hit && last == kMaxWorkloadGraphNodes,
              "caps: node cap enforced at exactly kMaxWorkloadGraphNodes");

        // The dependency cap.
        WorkloadGraph star;
        std::vector<WorkloadNodeId> spokes;
        WorkloadNodeId hub = 0;
        star.add_node(descriptor("hub"), {}, hub, error);
        for (std::uint32_t i = 0; i < kMaxWorkloadDependencies + 2; ++i) {
            WorkloadNodeId id = 0;
            star.add_node(descriptor("s" + std::to_string(i)), {}, id, error);
            spokes.push_back(id);
        }
        bool dep_cap = false;
        for (const WorkloadNodeId spoke : spokes) {
            const Status status = star.bind_dependency(hub, spoke, error);
            if (status != Status::Ok) {
                dep_cap = true;
                break;
            }
        }
        check(dep_cap, "caps: dependency cap enforced");
    }

    // ---- empty graph -------------------------------------------------------
    {
        WorkloadGraph graph;
        WorkloadGraphValidation validation;
        std::string error;
        check(validate_workload_graph(graph, validation, error) == Status::InvalidInput,
              "validation: empty graph refused");
    }

    if (failures == 0) {
        std::cout << "ALL FABRIC GRAPH CHECKS PASSED\n";
        return 0;
    }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
