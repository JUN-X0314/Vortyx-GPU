// Phase 16 — Adaptive Compute Fabric: the deterministic planner and plan
// serialization (test_fabric_planner.cpp).
//
// Pins the planning contract: the same (graph, snapshot, config) produces
// the BYTE-IDENTICAL plan; capability mismatch and resource insufficiency
// are structured refusals (never guessed into support); the ownership
// filter applies; the stale rule is pure; priority orders independent
// planning; locality and backend preferences shift the decision the
// documented way; excluded constraints hold; and the structured
// explanation is complete and deterministic.

#include <iostream>
#include <string>
#include <vector>

#include "fabric/planner.hpp"

using namespace vortyx;
using namespace vortyx::fabric;

namespace {

int failures = 0;

void check(bool ok, const std::string& name, const std::string& detail = "") {
    if (ok) {
        std::cout << "PASS: " << name << "\n";
    } else {
        std::cout << "FAIL: " << name;
        if (!detail.empty()) std::cout << "  [" << detail << "]";
        std::cout << "\n";
        ++failures;
    }
}

WorkloadDescriptor descriptor(const std::string& id, std::uint64_t elements = 1000) {
    WorkloadDescriptor d;
    d.workload_id = id;
    d.owner_user_id = "user-1";
    d.element_count = elements;
    return d;
}

// A snapshot with one device (the honest simulator pattern: claimed
// capabilities only, self-reported capacity).
vortyx::distributed::DeviceSnapshot device(const std::string& id,
                                           const std::vector<std::string>& operations,
                                           std::int64_t memory_bytes = 1 << 20,
                                           std::int64_t concurrent = 4) {
    vortyx::distributed::DeviceSnapshot d;
    d.device_id = id;
    d.owner_user_id = "user-1";
    d.state = vortyx::distributed::DeviceState::Ready;
    d.health = vortyx::distributed::DeviceHealth::Healthy;
    d.capabilities.metadata.software_version = "0.16.0";
    d.capabilities.metadata.backends = {"cpu"};
    d.capabilities.metadata.operations = operations;
    d.capabilities.capacity.compute_units = 4;
    d.capabilities.capacity.memory_bytes = memory_bytes;
    d.capabilities.capacity.concurrent_jobs = concurrent;
    d.capabilities.max_concurrent_shards = concurrent;
    return d;
}

vortyx::distributed::ClusterSnapshot snapshot(std::vector<vortyx::distributed::DeviceSnapshot> ds,
                                              std::uint64_t revision = 7) {
    vortyx::distributed::ClusterSnapshot s;
    s.revision = revision;
    s.devices = std::move(ds);
    return s;
}

FabricPlannerConfig config() { return FabricPlannerConfig{}; }

}  // namespace

int main() {
    // ---- determinism: the same inputs, the same plan, byte for byte ------
    {
        WorkloadGraph graph;
        WorkloadNodeId a = 0, b = 0;
        std::string error;
        graph.add_node(descriptor("w1"), {}, a, error);
        graph.add_node(descriptor("w2"), {a}, b, error);
        const auto snap = snapshot({device("dev-1", {"vector_add", "vector_multiply"}),
                                    device("dev-2", {"vector_add", "vector_multiply"})});

        FabricPlanner planner(config());
        ComputePlan first, second;
        check(planner.plan_graph(graph, "g1", snap, first, error) == Status::Ok,
              "determinism: first plan produced");
        check(planner.plan_graph(graph, "g1", snap, second, error) == Status::Ok,
              "determinism: second plan produced");
        check(first == second, "determinism: value-equal plans");
        check(serialize_compute_plan(first) == serialize_compute_plan(second),
              "determinism: byte-identical serialization");
        check(first.plan_version == kUnassignedPlanVersion,
              "determinism: the pure planner leaves the version unassigned");
        check(first.cluster_revision == 7, "determinism: plan records its snapshot revision");
        check(first.planner_name == "adaptive_fabric", "determinism: planner name recorded");
        check(first.nodes.size() == 2 && first.nodes[0].workload_id == "w1" &&
                  first.nodes[1].workload_id == "w2",
              "determinism: plan follows the topological order");
    }

    // ---- capability mismatch: a structured refusal, not a guess ----------
    {
        WorkloadGraph graph;
        WorkloadNodeId a = 0;
        std::string error;
        graph.add_node(descriptor("w1"), {}, a, error);
        // The device claims ONLY vector_scale — a vector_add workload finds
        // no claimant (unknown capability is never guessed into support).
        const auto snap = snapshot({device("dev-1", {"vector_scale"})});

        FabricPlanner planner(config());
        ComputePlan plan;
        check(planner.plan_graph(graph, "g1", snap, plan, error) == Status::InvalidInput,
              "capability: unplannable workload refused");
        check(plan.nodes.empty() &&
                  std::string(plan.rejection.code) == rejection_stage::kUnsupportedCapability &&
                  plan.rejection.node_workload_id == "w1",
              "capability: structured refusal names the stage and the node");
        check(!plan.rejection.message.empty(), "capability: refusal carries a human echo");
    }

    // ---- resource insufficiency ------------------------------------------
    {
        WorkloadGraph graph;
        WorkloadNodeId a = 0;
        std::string error;
        // 1M int32 elements = 12 MB across buffers; the device holds 1 MiB.
        graph.add_node(descriptor("w1", 1000000), {}, a, error);
        const auto snap = snapshot({device("dev-1", {"vector_add"}, 1 << 20)});

        FabricPlanner planner(config());
        ComputePlan plan;
        check(planner.plan_graph(graph, "g1", snap, plan, error) == Status::InvalidInput,
              "resource: insufficient device memory refused");
        check(std::string(plan.rejection.code) == rejection_stage::kInsufficientResource,
              "resource: the refusal stage is precise");
    }

    // ---- ownership scope ---------------------------------------------------
    {
        WorkloadGraph graph;
        WorkloadNodeId a = 0;
        std::string error;
        WorkloadDescriptor foreign = descriptor("w1");
        foreign.owner_user_id = "user-2";
        graph.add_node(foreign, {}, a, error);
        // The only device belongs to user-1: a planner that saw it would
        // break the Phase 12 visibility rule.
        const auto snap = snapshot({device("dev-1", {"vector_add"})});

        FabricPlanner planner(config());
        ComputePlan plan;
        check(planner.plan_graph(graph, "g1", snap, plan, error) == Status::InvalidInput &&
              std::string(plan.rejection.code) == rejection_stage::kClusterEmpty,
              "ownership: another user's devices are invisible to planning");
    }

    // ---- locality + backend preferences -----------------------------------
    {
        // Two identical devices; the descriptor prefers dev-2 (locality).
        WorkloadGraph graph;
        WorkloadNodeId a = 0;
        std::string error;
        WorkloadDescriptor local = descriptor("w1");
        local.preferred_device = "dev-2";
        graph.add_node(local, {}, a, error);
        const auto snap = snapshot({device("dev-1", {"vector_add"}),
                                    device("dev-2", {"vector_add"})});

        FabricPlanner planner(config());
        ComputePlan plan;
        check(planner.plan_graph(graph, "g1", snap, plan, error) == Status::Ok,
              "locality: plan produced");
        check(plan.nodes[0].decision.device_id == "dev-2",
              "locality: the hinted device wins deterministically");
        check(plan.nodes[0].decision.score.locality_bonus > 0,
              "locality: the bonus is visible in the explanation");

        // Backend preference: same two devices, only dev-2's PREFERRED
        // backend matches the request (both claim cpu here; the preference
        // bonus goes to the aligned one — via a requested backend the
        // device's preference matches).
        WorkloadGraph bg;
        WorkloadNodeId b = 0;
        WorkloadDescriptor with_backend = descriptor("w1");
        with_backend.requested_backend = "cpu";
        bg.add_node(with_backend, {}, b, error);
        auto dev1 = device("dev-1", {"vector_add"});
        auto dev2 = device("dev-2", {"vector_add"});
        dev2.capabilities.metadata.backends = {"vulkan", "cpu"};  // prefers vulkan
        dev1.capabilities.metadata.backends = {"cpu"};            // prefers cpu
        // Both CLAIM cpu (the operations list is claimed capability; the
        // backends list is the device's own preference order).
        const auto bsnap = snapshot({dev1, dev2});
        FabricPlanner bplanner(config());
        ComputePlan bplan;
        check(bplanner.plan_graph(bg, "g1", bsnap, bplan, error) == Status::Ok,
              "backend: plan produced");
        check(bplan.nodes[0].decision.device_id == "dev-1" &&
                  bplan.nodes[0].decision.score.backend_bonus > 0,
              "backend: the aligned device wins, bonus recorded");
    }

    // ---- priority orders independent planning ------------------------------
    {
        // ONE device with capacity for ONE shard's memory. Two independent
        // workloads: low priority first in id order, high priority second.
        // The high-priority node must claim the space (planning preference),
        // and the low-priority node must then refuse honestly.
        auto dev = device("dev-1", {"vector_add"}, 20000, 4);
        const auto snap = snapshot({dev});

        WorkloadGraph graph;
        WorkloadNodeId low = 0, high = 0;
        std::string error;
        WorkloadDescriptor low_d = descriptor("w-low", 1000);  // 12 KB
        low_d.priority = 1;
        WorkloadDescriptor high_d = descriptor("w-high", 1000);  // 12 KB
        high_d.priority = 9;
        graph.add_node(low_d, {}, low, error);
        graph.add_node(high_d, {}, high, error);

        FabricPlanner planner(config());
        ComputePlan plan;
        check(planner.plan_graph(graph, "g1", snap, plan, error) == Status::InvalidInput,
              "priority: the loser refuses honestly (all-or-nothing)");
        // The high-priority node was planned first and holds the space.
        check(error.find("w-low") != std::string::npos,
              "priority: the low-priority node is the one refused");
    }

    // ---- excluded constraints ---------------------------------------------
    {
        WorkloadGraph graph;
        WorkloadNodeId a = 0;
        std::string error;
        WorkloadDescriptor constrained = descriptor("w1");
        constrained.excluded_devices = {"dev-1"};
        graph.add_node(constrained, {}, a, error);
        const auto snap = snapshot({device("dev-1", {"vector_add"})});

        FabricPlanner planner(config());
        ComputePlan plan;
        check(planner.plan_graph(graph, "g1", snap, plan, error) == Status::InvalidInput &&
              std::string(plan.rejection.code) == rejection_stage::kExcludedConstraint,
              "constraints: the excluded device is never a candidate");
    }

    // ---- explanation completeness + rejection bookkeeping ------------------
    {
        WorkloadGraph graph;
        WorkloadNodeId a = 0;
        std::string error;
        graph.add_node(descriptor("w1"), {}, a, error);
        auto capable = device("dev-1", {"vector_add"});
        auto incapable = device("dev-2", {"vector_scale"});
        const auto snap = snapshot({capable, incapable});

        FabricPlanner planner(config());
        ComputePlan plan;
        check(planner.plan_graph(graph, "g1", snap, plan, error) == Status::Ok,
              "explanation: plan produced over a mixed cluster");
        const DeviceDecision& decision = plan.nodes[0].decision;
        check(decision.device_id == "dev-1", "explanation: the capable device won");
        check(decision.rejection_summary.total_capable == 1 &&
                  decision.rejection_summary.total_rejected == 1 &&
                  decision.rejected.size() == 1 &&
                  std::string(decision.rejected[0].stage) ==
                      rejection_stage::kUnsupportedCapability &&
                  decision.rejected[0].device_id == "dev-2",
              "explanation: rejections recorded with stable stage codes");
    }

    // ---- the stale rule (pure) ---------------------------------------------
    {
        const auto snap = snapshot({}, 7);
        WorkloadGraph graph;
        WorkloadNodeId a = 0;
        std::string error;
        graph.add_node(descriptor("w1"), {}, a, error);
        FabricPlanner planner(config());
        ComputePlan plan;
        planner.plan_graph(graph, "g1", snap, plan, error);

        auto moved = snapshot({}, 8);
        check(plan_is_stale(plan, moved), "stale: a moved cluster makes the plan stale");
        check(!plan_is_stale(plan, snap), "stale: the same revision is not stale");
    }

    // ---- malformed graphs are refused --------------------------------------
    {
        WorkloadGraph graph;
        WorkloadNodeId a = 0, b = 0;
        std::string error;
        graph.add_node(descriptor("w1"), {}, a, error);
        graph.add_node(descriptor("w2"), {a}, b, error);
        graph.bind_dependency(a, b, error);  // cycle

        FabricPlanner planner(config());
        ComputePlan plan;
        check(planner.plan_graph(graph, "g1", snapshot({}), plan, error) ==
                  Status::InvalidInput,
              "malformed: a cyclic graph is refused with a structured rejection");
        check(std::string(plan.rejection.code) == rejection_stage::kInvalidRequest,
              "malformed: the graph-level stage code is stable");
    }

    if (failures == 0) {
        std::cout << "ALL FABRIC PLANNER CHECKS PASSED\n";
        return 0;
    }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
