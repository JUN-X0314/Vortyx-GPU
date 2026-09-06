// Phase 16 — Adaptive Compute Fabric: plan lineage and the safe replanning
// rule (test_fabric_replan.cpp).
//
// Pins: the version contract (monotonic, same content keeps its version,
// bounded ring), the checkpoint semantics (succeeded shards preserved
// verbatim — same device, same range), the honest refusal when the
// remaining work cannot be placed, and the stale-pair refusal (a replan
// without a cluster change is a caller bug, not a plan event).

#include <iostream>
#include <string>
#include <vector>

#include "fabric/planner.hpp"
#include "fabric/replan.hpp"

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

vortyx::distributed::DeviceSnapshot device(const std::string& id,
                                           std::int64_t memory_bytes = 1 << 22) {
    vortyx::distributed::DeviceSnapshot d;
    d.device_id = id;
    d.owner_user_id = "user-1";
    d.state = vortyx::distributed::DeviceState::Ready;
    d.health = vortyx::distributed::DeviceHealth::Healthy;
    d.capabilities.metadata.software_version = "0.16.0";
    d.capabilities.metadata.backends = {"cpu"};
    d.capabilities.metadata.operations = {"vector_add", "vector_multiply", "vector_scale"};
    d.capabilities.capacity.compute_units = 4;
    d.capabilities.capacity.memory_bytes = memory_bytes;
    d.capabilities.capacity.concurrent_jobs = 4;
    d.capabilities.max_concurrent_shards = 4;
    return d;
}

vortyx::distributed::ClusterSnapshot snapshot(std::vector<vortyx::distributed::DeviceSnapshot> ds,
                                              std::uint64_t revision) {
    vortyx::distributed::ClusterSnapshot s;
    s.revision = revision;
    s.devices = std::move(ds);
    return s;
}

}  // namespace

int main() {

    // ---- lineage versions --------------------------------------------------
    {
        PlanLineage lineage;
        check(lineage.current_version() == 0 && lineage.current_plan() == nullptr,
              "lineage: empty lineage has no version");

        ComputePlan v1_content;
        v1_content.graph_id = "g1";
        v1_content.cluster_revision = 5;
        v1_content.nodes.push_back(PlanNodeAssignment{});
        v1_content.nodes[0].workload_id = "w1";
        check(lineage.record(v1_content, lineage_trigger::kInitial) == 1,
              "lineage: the first record is version 1");
        check(lineage.current_version() == 1 &&
                  lineage.current_plan()->plan_version == 1,
              "lineage: the recorded plan carries its version");

        // The SAME content keeps the current version (no fake bump).
        ComputePlan same = v1_content;
        check(lineage.record(same, lineage_trigger::kInitial) == 1,
              "lineage: identical content keeps the current version");

        // Different content -> the next version, monotonic.
        ComputePlan v2_content = v1_content;
        v2_content.cluster_revision = 6;
        v2_content.nodes[0].decision.device_id = "dev-2";
        check(lineage.record(v2_content, lineage_trigger::kReplanRemaining) == 2,
              "lineage: changed content bumps to version 2");

        // The history is bounded (a ring): the current version survives.
        for (std::uint32_t i = 3; i <= 20; ++i) {
            ComputePlan content = v1_content;
            content.cluster_revision = 5 + i;
            content.nodes[0].decision.device_id = "dev-" + std::to_string(i);
            lineage.record(content, lineage_trigger::kReplanRemaining);
        }
        check(lineage.current_version() == 20,
              "lineage: versions stay monotonic across many records");
        check(lineage.entries().size() == PlanLineage::kMaxEntries,
              "lineage: the ring is bounded (oldest entries fall off)");
        check(lineage.entries().front().plan_version == 20 - PlanLineage::kMaxEntries + 1,
              "lineage: the ring retains the most recent history");
    }

    // ---- the safe replan: checkpoints preserved, remaining re-planned ------
    {
        const auto before = snapshot({device("dev-a"), device("dev-b")}, 10);
        FabricPlanner planner(FabricPlannerConfig{});

        // A two-node graph; plan it.
        WorkloadGraph graph;
        WorkloadNodeId n1 = 0, n2 = 0;
        std::string error;
        graph.add_node(descriptor("w1", 100000), {}, n1, error);
        graph.add_node(descriptor("w2", 100000), {}, n2, error);
        ComputePlan v1;
        const Status planned = planner.plan_graph(graph, "g1", before, v1, error);
        if (planned != Status::Ok || v1.nodes.size() != 2) {
            std::cout << "FAIL: replan: the initial plan exists  [" << error << "]\n";
            ++failures;
            std::cout << failures << " CHECK(S) FAILED\n";
            return 1;  // cannot index an unproduced plan — stop here honestly
        }
        std::cout << "PASS: replan: the initial plan exists\n";
        // The realistic flow: the pure content is PUBLISHED as version 1
        // (lineage stamping — the planner itself leaves 0).
        PlanLineage lineage;
        lineage.record(v1, lineage_trigger::kInitial);
        v1 = *lineage.current_plan();
        check(v1.plan_version == 1, "replan: the initial publication is version 1");
        check(v1.nodes.size() == 2 && v1.nodes[0].shards.size() == 1 &&
                  v1.nodes[1].shards.size() == 1,
              "replan: both nodes placed");
        const DeviceId w1_device = v1.nodes[0].shards[0].device_id;
        const ElementRange w1_range = v1.nodes[0].shards[0].range;
        // Tie-break: identical devices -> the smaller id first.
        check(w1_device == "dev-a", "replan: sanity — the first node won dev-a");

        // Execution feedback: w1's shard SUCCEEDED on dev-a (a checkpoint);
        // w2's shard FAILED with a device failure.
        std::vector<ShardExecutionFeedback> outcomes;
        ShardExecutionFeedback succeeded;
        succeeded.shard_id = "w1-s0";
        succeeded.workload_id = "w1";
        succeeded.shard_index = 0;
        succeeded.range = w1_range;
        succeeded.device_id = w1_device;
        succeeded.outcome = ShardOutcome::Succeeded;
        outcomes.push_back(succeeded);
        ShardExecutionFeedback failed;
        failed.shard_id = "w2-s0";
        failed.workload_id = "w2";
        failed.shard_index = 0;
        failed.range = v1.nodes[1].shards[0].range;
        failed.device_id = "dev-b";
        failed.outcome = ShardOutcome::Failed;
        failed.failure_code = "device_lost";
        outcomes.push_back(failed);

        // The cluster MOVED: dev-b is gone (a device left / failed).
        const auto after = snapshot({device("dev-a"), device("dev-c")}, 11);
        FabricReplanner replanner(planner);
        ComputePlan v2;
        check(replanner.replan(v1, outcomes, graph, "g1", after, v2, error) == Status::Ok,
              "replan: the replan succeeded against the fresh snapshot");
        check(v2.plan_version == 2, "replan: the version is exactly previous + 1");
        check(v2.cluster_revision == 11, "replan: the replan records the NEW revision");

        // THE CHECKPOINT: w1 keeps its recorded device and range verbatim.
        bool checkpoint_ok = false;
        for (const PlanNodeAssignment& node : v2.nodes) {
            if (node.workload_id != "w1") continue;
            checkpoint_ok = node.shards.size() == 1 &&
                            node.shards[0].device_id == w1_device &&
                            node.shards[0].range == w1_range;
        }
        check(checkpoint_ok,
              "replan: the succeeded shard is preserved verbatim (never re-placed)");

        // w2 moved to a device that EXISTS in the fresh snapshot.
        bool replan_ok = false;
        for (const PlanNodeAssignment& node : v2.nodes) {
            if (node.workload_id != "w2") continue;
            replan_ok = node.shards.size() == 1 &&
                        (node.shards[0].device_id == "dev-a" ||
                         node.shards[0].device_id == "dev-c") &&
                        node.shards[0].device_id != "dev-b";
        }
        check(replan_ok, "replan: the unfinished work was re-planned on live devices");
    }

    // ---- the honest refusal: nothing can take the remaining work -----------
    {
        const auto before = snapshot({device("dev-a"), device("dev-b")}, 10);
        FabricPlanner planner(FabricPlannerConfig{});
        WorkloadGraph graph;
        WorkloadNodeId n1 = 0;
        std::string error;
        graph.add_node(descriptor("w1", 100000), {}, n1, error);
        ComputePlan v1;
        if (planner.plan_graph(graph, "g1", before, v1, error) != Status::Ok ||
            v1.nodes.empty()) {
            std::cout << "FAIL: refusal setup: initial plan missing  [" << error << "]\n";
            ++failures;
            std::cout << failures << " CHECK(S) FAILED\n";
            return 1;
 }

        // w1 succeeded on dev-a; the fresh cluster has NO devices at all.
        std::vector<ShardExecutionFeedback> outcomes;
        ShardExecutionFeedback succeeded;
        succeeded.shard_id = "w1-s0";
        succeeded.workload_id = "w1";
        succeeded.shard_index = 0;
        succeeded.range = v1.nodes[0].shards[0].range;
        succeeded.device_id = "dev-a";
        succeeded.outcome = ShardOutcome::Succeeded;
        outcomes.push_back(succeeded);

        const auto after = snapshot({}, 12);
        FabricReplanner replanner(planner);
        ComputePlan refused;
        check(replanner.replan(v1, outcomes, graph, "g1", after, refused, error) ==
                  Status::InvalidInput,
              "refusal: no placement for the remaining work is a structured failure");
        check(std::string(refused.rejection.code) == rejection_stage::kClusterEmpty,
              "refusal: the stage code is stable");
    }

    // ---- the stale-pair refusal: a replan needs a CHANGED cluster ----------
    {
        const auto same = snapshot({device("dev-a")}, 10);
        FabricPlanner planner(FabricPlannerConfig{});
        WorkloadGraph graph;
        WorkloadNodeId n1 = 0;
        std::string error;
        graph.add_node(descriptor("w1"), {}, n1, error);
        ComputePlan v1;
        if (planner.plan_graph(graph, "g1", same, v1, error) != Status::Ok ||
            v1.nodes.empty()) {
            std::cout << "FAIL: stale pair setup: initial plan missing  [" << error << "]\n";
            ++failures;
            std::cout << failures << " CHECK(S) FAILED\n";
            return 1;
 }

        FabricReplanner replanner(planner);
        ComputePlan out;
        check(replanner.replan(v1, {}, graph, "g1", same, out, error) == Status::InvalidInput,
              "stale pair: a replan without a revision change is refused");
    }

    if (failures == 0) {
        std::cout << "ALL FABRIC REPLAN CHECKS PASSED\n";
        return 0;
    }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
