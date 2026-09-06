// Phase 16 — the TensorGraph -> WorkloadGraph adapter
// (test_tensor_fabric.cpp).
//
// Pins the honest Tensor integration: int32 contiguous elementwise
// Add/Multiply convert into fabric workloads with the SAME executable
// vocabulary the runtime adapter uses (VectorAdd/VectorMultiply); the
// dependency structure is preserved 1:1; element counts come from the
// validated output inference; and every node WITHOUT an executable
// equivalent (fp32 nodes, matmul, reductions, ...) refuses the whole
// conversion with the node named — the fabric never plans work no device
// can claim, and never invents a mapping.

#include <iostream>
#include <string>

#include "fabric/planner.hpp"
#include "tensor/fabric_adapter.hpp"
#include "tensor/tensor.hpp"

using namespace vortyx::tensor;
using ST = TensorStatus;
using vortyx::platform::Status;
using vortyx::fabric::WorkloadNode;

namespace {

int failures = 0;

void check(bool ok, const std::string& message) {
    if (ok) {
        std::cout << "PASS: " << message << "\n";
    } else {
        std::cout << "FAIL: " << message << "\n";
        ++failures;
    }
}

vortyx::distributed::DeviceSnapshot device(const std::string& id) {
    vortyx::distributed::DeviceSnapshot d;
    d.device_id = id;
    d.owner_user_id = "user-1";
    d.state = vortyx::distributed::DeviceState::Ready;
    d.health = vortyx::distributed::DeviceHealth::Healthy;
    d.capabilities.metadata.software_version = "0.16.0";
    d.capabilities.metadata.backends = {"cpu"};
    d.capabilities.metadata.operations = {"vector_add", "vector_multiply", "vector_scale"};
    d.capabilities.capacity.compute_units = 4;
    d.capabilities.capacity.memory_bytes = 1 << 22;
    d.capabilities.capacity.concurrent_jobs = 4;
    d.capabilities.max_concurrent_shards = 4;
    return d;
}

}  // namespace

int main() {
    // ---- an int32 elementwise graph converts ------------------------------
    {
        TensorGraph graph;
        std::string error;
        GraphInputDesc x;
        x.name = "x";
        x.shape = TensorShape::make({2, 2});
        x.dtype = DataType::INT32;
        GraphInputDesc y;
        y.name = "y";
        y.shape = TensorShape::make({2, 2});
        y.dtype = DataType::INT32;
        check(graph.add_input(x, error) == ST::Ok, "adapter: x slot added");
        check(graph.add_input(y, error) == ST::Ok, "adapter: y slot added");

        NodeId add_node = kInvalidNodeId;
        GraphNodeInput from_x;
        from_x.source = GraphNodeInput::Source::GraphInput;
        from_x.index = 0;
        GraphNodeInput from_y;
        from_y.source = GraphNodeInput::Source::GraphInput;
        from_y.index = 1;
        check(graph.add_node(TensorOp::Add, TensorOpParams{}, {from_x, from_y},
                             add_node, error) == ST::Ok,
              "adapter: int32 Add node added");
        check(graph.set_outputs({add_node}, error) == ST::Ok, "adapter: output set");

        GraphValidation validation;
        check(validate_graph(graph, validation, error) == ST::Ok, "adapter: graph valid");

        vortyx::fabric::WorkloadGraph workload;
        check(tensor_graph_to_workload(graph, validation, "user-1", 1, workload, error) ==
                  Status::Ok,
              "adapter: int32 elementwise graph converts");
        check(workload.node_count() == 1, "adapter: one workload node per graph node");
        const WorkloadNode* node = workload.find(1);
        check(node != nullptr && node->descriptor.workload_id == "t1" &&
                  node->descriptor.operation == vortyx::compute::ComputeOp::VectorAdd &&
                  node->descriptor.element_count == 4 &&
                  node->descriptor.owner_user_id == "user-1",
              "adapter: the workload carries the honest executable vocabulary");
        vortyx::fabric::WorkloadGraphValidation wv;
        check(validate_workload_graph(workload, wv, error) == Status::Ok,
              "adapter: the converted graph is itself valid");
    }

    // ---- the dependency structure is preserved -----------------------------
    {
        TensorGraph graph;
        std::string error;
        GraphInputDesc x;
        x.name = "x";
        x.shape = TensorShape::make({4});
        x.dtype = DataType::INT32;
        check(graph.add_input(x, error) == ST::Ok, "chain: x added");

        NodeId n1 = kInvalidNodeId, n2 = kInvalidNodeId;
        GraphNodeInput from_x;
        from_x.source = GraphNodeInput::Source::GraphInput;
        from_x.index = 0;
        // n1 = Add(x, x); n2 = Multiply(n1, x) — a real data dependency.
        check(graph.add_node(TensorOp::Add, TensorOpParams{}, {from_x, from_x}, n1, error) ==
                  ST::Ok,
              "chain: n1 = Add(x, x)");
        GraphNodeInput from_n1;
        from_n1.source = GraphNodeInput::Source::NodeOutput;
        from_n1.index = static_cast<std::int32_t>(n1);
        check(graph.add_node(TensorOp::Multiply, TensorOpParams{},
                             {from_n1, from_x}, n2, error) == ST::Ok,
              "chain: n2 = Multiply(n1, x)");
        check(graph.set_outputs({n2}, error) == ST::Ok, "chain: output set");
        GraphValidation validation;
        check(validate_graph(graph, validation, error) == ST::Ok, "chain: graph valid");

        vortyx::fabric::WorkloadGraph workload;
        check(tensor_graph_to_workload(graph, validation, "user-1", 1, workload, error) ==
                  Status::Ok,
              "chain: converts");
        const WorkloadNode* second = workload.find(2);
        check(second != nullptr && second->dependencies.size() == 1 &&
                  second->dependencies[0] == 1,
              "chain: the data dependency (n1 -> n2) is preserved");
        check(second != nullptr &&
                  second->descriptor.operation == vortyx::compute::ComputeOp::VectorMultiply,
              "chain: Multiply maps to the executable multiply");

        // The converted chain PLANS deterministically through the fabric.
        vortyx::distributed::ClusterSnapshot snapshot;
        snapshot.revision = 3;
        snapshot.devices = {device("dev-a")};
        vortyx::fabric::FabricPlanner planner(vortyx::fabric::FabricPlannerConfig{});
        vortyx::fabric::ComputePlan plan;
        check(planner.plan_graph(workload, "tensor-chain", snapshot, plan, error) == Status::Ok,
              "chain: the converted graph plans");
        check(plan.nodes.size() == 2 && plan.nodes[0].workload_id == "t1" &&
                  plan.nodes[1].workload_id == "t2",
              "chain: dependencies order the plan (t1 before t2)");
    }

    // ---- fp32 nodes refuse (no executable equivalent) ----------------------
    {
        TensorGraph graph;
        std::string error;
        GraphInputDesc x;
        x.name = "x";
        x.shape = TensorShape::make({2, 2});
        x.dtype = DataType::FP32;
        check(graph.add_input(x, error) == ST::Ok, "fp32: x added");
        NodeId relu_node = kInvalidNodeId;
        GraphNodeInput from_x;
        from_x.source = GraphNodeInput::Source::GraphInput;
        from_x.index = 0;
        check(graph.add_node(TensorOp::Relu, TensorOpParams{}, {from_x}, relu_node, error) ==
                  ST::Ok,
              "fp32: relu node added");
        check(graph.set_outputs({relu_node}, error) == ST::Ok, "fp32: output set");
        GraphValidation validation;
        check(validate_graph(graph, validation, error) == ST::Ok, "fp32: graph valid");

        vortyx::fabric::WorkloadGraph workload;
        check(tensor_graph_to_workload(graph, validation, "user-1", 1, workload, error) ==
                  Status::InvalidInput,
              "fp32: the conversion refuses (fp32 has no executable equivalent)");
        check(error.find("relu") != std::string::npos &&
                  error.find("no executable equivalent") != std::string::npos,
              "fp32: the refusal names the op honestly");
    }

    // ---- matmul refuses -----------------------------------------------------
    {
        TensorGraph graph;
        std::string error;
        GraphInputDesc a;
        a.name = "a";
        a.shape = TensorShape::make({2, 3});
        a.dtype = DataType::FP32;
        GraphInputDesc b;
        b.name = "b";
        b.shape = TensorShape::make({3, 2});
        b.dtype = DataType::FP32;
        check(graph.add_input(a, error) == ST::Ok && graph.add_input(b, error) == ST::Ok,
              "matmul: inputs added");
        NodeId mm = kInvalidNodeId;
        GraphNodeInput from_a;
        from_a.source = GraphNodeInput::Source::GraphInput;
        from_a.index = 0;
        GraphNodeInput from_b;
        from_b.source = GraphNodeInput::Source::GraphInput;
        from_b.index = 1;
        check(graph.add_node(TensorOp::MatMul, TensorOpParams{},
                             {from_a, from_b},
                             mm, error) == ST::Ok,
              "matmul: node added");
        check(graph.set_outputs({mm}, error) == ST::Ok, "matmul: output set");
        GraphValidation validation;
        check(validate_graph(graph, validation, error) == ST::Ok, "matmul: graph valid");

        vortyx::fabric::WorkloadGraph workload;
        check(tensor_graph_to_workload(graph, validation, "user-1", 1, workload, error) ==
                  Status::InvalidInput,
              "matmul: refused (no device vocabulary claims matmul)");
    }

    if (failures == 0) {
        std::cout << "ALL TENSOR FABRIC ADAPTER CHECKS PASSED\n";
        return 0;
    }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
