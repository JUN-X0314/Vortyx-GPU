// Tensor graph tests (Phase 13) — construction determinism, validation
// (cycles, unbound inputs, shape/dtype inference), deterministic execution
// plans, memory-planner reuse safety, and a real end-to-end graph
// execution (MatMul -> Add(bias) -> ReLU) verified numerically.
//
// Convention: plain main() + check(), like every other test in this project.

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "tensor/tensor.hpp"

using namespace vortyx::tensor;
using ST = TensorStatus;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_status(ST actual, ST expected, const std::string& message) {
    check(actual == expected,
          message + " (expected " + tensor_status_code(expected) + ", got " +
              tensor_status_code(actual) + ")");
}

std::vector<float> floats(const Tensor& tensor) {
    std::vector<float> out(static_cast<std::size_t>(tensor.elements()), 0.0f);
    std::string error;
    if (tensor.read_host(out.data(), out.size() * sizeof(float), error) != ST::Ok) {
        std::cerr << "FAIL: read failed: " << error << "\n";
        ++failures;
        return {};
    }
    return out;
}

bool same_floats(const std::vector<float>& got, const std::vector<float>& want,
                 float tolerance = 1e-5f) {
    if (got.size() != want.size()) return false;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (std::fabs(got[i] - want[i]) > tolerance) return false;
    }
    return true;
}

}  // namespace

int main() {
    // The manager MUST live in a shared_ptr (the Phase 4 contract: Buffer
    // handles observe it weakly).
    auto manager = std::make_shared<vortyx::resource::ResourceManager>();
    vortyx::resource::CpuBufferProvider cpu_provider;
    check(manager->register_provider(&cpu_provider), "cpu provider registers");

    TensorExecutor::Deps deps;
    deps.resources = manager.get();
    std::unique_ptr<TensorExecutor> executor;
    std::string error;
    check(TensorExecutor::create(deps, executor, error) == ST::Ok, "executor created");

    // =====================================================================
    // 1. Construction: deterministic ids, caps, duplicate refusals
    // =====================================================================
    {
        TensorGraph graph;
        GraphInputDesc x;
        x.name = "x";
        x.shape = TensorShape::make({2, 2});
        x.dtype = DataType::FP32;
        check_status(graph.add_input(x, error), ST::Ok, "input slot added");
        check_status(graph.add_input(x, error), ST::InvalidInput, "duplicate name refused");

        GraphInputDesc unnamed;
        unnamed.name = "";
        check_status(graph.add_input(unnamed, error), ST::InvalidInput, "unnamed slot refused");

        NodeId n1 = kInvalidNodeId;
        NodeId n2 = kInvalidNodeId;
        check_status(graph.add_node(TensorOp::Relu, TensorOpParams{}, n1, error), ST::Ok,
                     "node 1");
        check_status(graph.add_node(TensorOp::Relu, TensorOpParams{}, n2, error), ST::Ok,
                     "node 2");
        check(n1 == 1 && n2 == 2, "node ids are the deterministic insertion sequence");

        check_status(graph.set_outputs({n2}, error), ST::Ok, "outputs set");
        check_status(graph.set_outputs({n2, n2}, error), ST::InvalidState,
                     "duplicate output refused");
        check_status(graph.set_outputs({}, error), ST::InvalidInput, "empty outputs refused");
        check_status(graph.set_outputs({99}, error), ST::InvalidState, "unknown output refused");

        // Node cap.
        TensorGraph huge;
        NodeId last = kInvalidNodeId;
        ST status = ST::Ok;
        for (std::uint32_t i = 0; i <= kMaxGraphNodes; ++i) {
            status = huge.add_node(TensorOp::Relu, TensorOpParams{}, last, error);
        }
        check_status(status, ST::ResourceLimitExceeded, "node cap enforced");
    }

    // =====================================================================
    // 2. Validation: unbound inputs, unknown references, inference errors
    // =====================================================================
    {
        TensorGraph graph;
        GraphInputDesc x;
        x.name = "x";
        x.shape = TensorShape::make({2, 2});
        x.dtype = DataType::FP32;
        check(graph.add_input(x, error) == ST::Ok, "x added");

        NodeId relu_node = kInvalidNodeId;
        check(graph.add_node(TensorOp::Relu, TensorOpParams{}, relu_node, error) == ST::Ok,
              "relu node");
        GraphValidation validation;
        check_status(validate_graph(graph, validation, error), ST::InvalidState,
                     "unbound input refused");
        check(error.find("not bound") != std::string::npos, "the error names the unbound slot");

        // Bind to a nonexistent node.
        GraphNodeInput bad;
        bad.source = GraphNodeInput::Source::NodeOutput;
        bad.index = 42;
        check(graph.bind_input(relu_node, 0, bad, error) == ST::Ok, "binding stored");
        check_status(validate_graph(graph, validation, error), ST::InvalidState,
                     "reference to unknown node refused");

        // Bind correctly, then violate the op's dtype rules through a second node.
        GraphNodeInput from_x;
        from_x.source = GraphNodeInput::Source::GraphInput;
        from_x.index = 0;
        check(graph.bind_input(relu_node, 0, from_x, error) == ST::Ok, "bound to x");
        check_status(validate_graph(graph, validation, error), ST::Ok, "valid graph accepted");
        check(validation.node_outputs[relu_node - 1].shape == TensorShape::make({2, 2}),
              "inference propagated the shape");

        // Add an int32 input feeding the float relu -> UnsupportedDtype at THAT node.
        GraphInputDesc xi;
        xi.name = "xi";
        xi.shape = TensorShape::make({2});
        xi.dtype = DataType::INT32;
        check(graph.add_input(xi, error) == ST::Ok, "int slot added");
        NodeId relu2 = kInvalidNodeId;
        check(graph.add_node(TensorOp::Relu, TensorOpParams{}, relu2, error) == ST::Ok,
              "second relu");
        GraphNodeInput from_xi;
        from_xi.source = GraphNodeInput::Source::GraphInput;
        from_xi.index = 1;
        check(graph.bind_input(relu2, 0, from_xi, error) == ST::Ok, "bound to xi");
        check_status(validate_graph(graph, validation, error), ST::UnsupportedDtype,
                     "dtype inference violation caught at the node");

        // Shape inconsistency: MatMul K mismatch across edges.
        TensorGraph mm;
        GraphInputDesc a;
        a.name = "a";
        a.shape = TensorShape::make({2, 3});
        a.dtype = DataType::FP32;
        GraphInputDesc b;
        b.name = "b";
        b.shape = TensorShape::make({4, 2});
        b.dtype = DataType::FP32;
        check(mm.add_input(a, error) == ST::Ok && mm.add_input(b, error) == ST::Ok, "mm inputs");
        NodeId mm_node = kInvalidNodeId;
        check(mm.add_node(TensorOp::MatMul, TensorOpParams{}, mm_node, error) == ST::Ok,
              "mm node");
        GraphNodeInput ia;
        ia.source = GraphNodeInput::Source::GraphInput;
        ia.index = 0;
        GraphNodeInput ib;
        ib.source = GraphNodeInput::Source::GraphInput;
        ib.index = 1;
        check(mm.bind_input(mm_node, 0, ia, error) == ST::Ok, "bind a");
        check(mm.bind_input(mm_node, 1, ib, error) == ST::Ok, "bind b");
        GraphValidation mm_validation;
        check_status(validate_graph(mm, mm_validation, error), ST::InvalidShape,
                     "K mismatch across graph edges caught");
    }

    // =====================================================================
    // 3. Cycle detection (two-phase binding CAN create one)
    // =====================================================================
    {
        TensorGraph graph;
        GraphInputDesc x;
        x.name = "x";
        x.shape = TensorShape::make({2, 2});
        x.dtype = DataType::FP32;
        check(graph.add_input(x, error) == ST::Ok, "x added");

        NodeId n1 = kInvalidNodeId;
        NodeId n2 = kInvalidNodeId;
        check(graph.add_node(TensorOp::Add, TensorOpParams{}, n1, error) == ST::Ok, "node 1");
        check(graph.add_node(TensorOp::Add, TensorOpParams{}, n2, error) == ST::Ok, "node 2");

        GraphNodeInput from_x;
        from_x.source = GraphNodeInput::Source::GraphInput;
        from_x.index = 0;
        // n1 = x + n2's output; n2 = x + n1's output -> a cycle.
        GraphNodeInput from_n2;
        from_n2.source = GraphNodeInput::Source::NodeOutput;
        from_n2.index = static_cast<std::int32_t>(n2);
        GraphNodeInput from_n1;
        from_n1.source = GraphNodeInput::Source::NodeOutput;
        from_n1.index = static_cast<std::int32_t>(n1);
        check(graph.bind_input(n1, 0, from_x, error) == ST::Ok, "n1 slot0");
        check(graph.bind_input(n1, 1, from_n2, error) == ST::Ok, "n1 slot1 -> n2");
        check(graph.bind_input(n2, 0, from_x, error) == ST::Ok, "n2 slot0");
        check(graph.bind_input(n2, 1, from_n1, error) == ST::Ok, "n2 slot1 -> n1");
        check(graph.set_outputs({n1}, error) == ST::Ok, "outputs");

        GraphValidation validation;
        check_status(validate_graph(graph, validation, error), ST::InvalidState,
                     "cycle detected");
        check(error.find("cycle") != std::string::npos, "the error names the cycle");
    }

    // =====================================================================
    // 4. Deterministic planning: same graph -> identical plan
    // =====================================================================
    {
        // Small DAG: out = ReLU(Add(x, MatMul(x, x))) on 2x2 floats.
        TensorGraph graph;
        GraphInputDesc x;
        x.name = "x";
        x.shape = TensorShape::make({2, 2});
        x.dtype = DataType::FP32;
        check(graph.add_input(x, error) == ST::Ok, "x added");

        NodeId mm = kInvalidNodeId;
        NodeId add = kInvalidNodeId;
        NodeId relu = kInvalidNodeId;
        check(graph.add_node(TensorOp::MatMul, TensorOpParams{}, mm, error) == ST::Ok, "mm");
        check(graph.add_node(TensorOp::Add, TensorOpParams{}, add, error) == ST::Ok, "add");
        check(graph.add_node(TensorOp::Relu, TensorOpParams{}, relu, error) == ST::Ok, "relu");

        GraphNodeInput fx;
        fx.source = GraphNodeInput::Source::GraphInput;
        fx.index = 0;
        GraphNodeInput fmm;
        fmm.source = GraphNodeInput::Source::NodeOutput;
        fmm.index = static_cast<std::int32_t>(mm);
        GraphNodeInput fadd;
        fadd.source = GraphNodeInput::Source::NodeOutput;
        fadd.index = static_cast<std::int32_t>(add);
        check(graph.bind_input(mm, 0, fx, error) == ST::Ok, "mm slot0");
        check(graph.bind_input(mm, 1, fx, error) == ST::Ok, "mm slot1");
        check(graph.bind_input(add, 0, fx, error) == ST::Ok, "add slot0");
        check(graph.bind_input(add, 1, fmm, error) == ST::Ok, "add slot1");
        check(graph.bind_input(relu, 0, fadd, error) == ST::Ok, "relu slot0");
        check(graph.set_outputs({relu}, error) == ST::Ok, "outputs");

        GraphValidation validation;
        check(validate_graph(graph, validation, error) == ST::Ok, "graph valid");

        GraphExecutionPlan plan_a;
        GraphExecutionPlan plan_b;
        MemoryPlannerConfig config;
        const TensorCapabilities& caps = executor->backends().front()->capabilities();
        check(make_execution_plan(graph, validation, caps, config, plan_a, error) == ST::Ok,
              "plan A built");
        check(make_execution_plan(graph, validation, caps, config, plan_b, error) == ST::Ok,
              "plan B built");

        check(plan_a.steps.size() == plan_b.steps.size() && plan_a.steps.size() == 3,
              "three steps in topological order");
        bool identical = true;
        for (std::size_t i = 0; i < plan_a.steps.size(); ++i) {
            identical = identical && plan_a.steps[i].node_id == plan_b.steps[i].node_id &&
                        plan_a.steps[i].output_slot == plan_b.steps[i].output_slot &&
                        plan_a.steps[i].output_shape == plan_b.steps[i].output_shape;
            for (std::size_t s = 0; s < plan_a.steps[i].inputs.size(); ++s) {
                identical = identical &&
                            plan_a.steps[i].inputs[s].source == plan_b.steps[i].inputs[s].source &&
                            plan_a.steps[i].inputs[s].index == plan_b.steps[i].inputs[s].index;
            }
        }
        identical = identical && plan_a.slots.size() == plan_b.slots.size() &&
                    plan_a.naive_bytes == plan_b.naive_bytes &&
                    plan_a.planned_bytes == plan_b.planned_bytes;
        check(identical, "two plans of the same graph are structurally identical");

        // Topological order: matmul before add before relu.
        check(plan_a.steps[0].node_id == mm && plan_a.steps[1].node_id == add &&
                  plan_a.steps[2].node_id == relu,
              "deterministic smallest-id-first topological order");

        // Memory plan: the matmul output (2x2) is consumed by add; the add
        // output (2x2) by relu; relu is the pinned output. The matmul slot
        // is reusable after add... nothing else reuses it here (sizes equal
        // but relu happens after add's last use -> relu could reuse the
        // matmul slot). Planned bytes must be <= naive bytes, and the
        // OUTPUT slot must be pinned.
        check(plan_a.planned_bytes <= plan_a.naive_bytes, "reuse never increases bytes");
        const PlanSlot& out_slot = plan_a.slots[static_cast<std::size_t>(
            plan_a.steps[2].output_slot)];
        check(out_slot.pinned, "the graph output slot is pinned");
    }

    // =====================================================================
    // 4b. Planner regression: slot reuse requires SHAPE equality, not just
    //     equal bytes. (Phase 13 audit: a byte-equal but differently-shaped
    //     redefinition was planned into the same slot — one allocation, one
    //     shape — so the first definition's kernel then refused the slot
    //     tensor at execution: a VALID graph that could never run.)
    // =====================================================================
    {
        // node1 = Reshape(x[2,3] -> [1,6])   (24B fp32)
        // node2 = Add(a[2,3], b[2,3])        (24B fp32)  -> bytes match,
        // shapes do not. The add step must NOT reuse the reshape slot.
        TensorGraph graph;
        GraphInputDesc x;
        x.name = "x";
        x.shape = TensorShape::make({2, 3});
        x.dtype = DataType::FP32;
        check(graph.add_input(x, error) == ST::Ok, "4b: x");
        GraphInputDesc a;
        a.name = "a";
        a.shape = TensorShape::make({2, 3});
        a.dtype = DataType::FP32;
        check(graph.add_input(a, error) == ST::Ok, "4b: a");
        GraphInputDesc b;
        b.name = "b";
        b.shape = TensorShape::make({2, 3});
        b.dtype = DataType::FP32;
        check(graph.add_input(b, error) == ST::Ok, "4b: b");

        NodeId reshape = kInvalidNodeId;
        NodeId add = kInvalidNodeId;
        TensorOpParams reshape_params;
        reshape_params.reshape_target = TensorShape::make({1, 6});
        check(graph.add_node(TensorOp::Reshape, reshape_params,
                             {GraphNodeInput{GraphNodeInput::Source::GraphInput, 0}}, reshape,
                             error) == ST::Ok,
              "4b: reshape node");
        check(graph.add_node(TensorOp::Add, TensorOpParams{},
                             {GraphNodeInput{GraphNodeInput::Source::GraphInput, 1},
                              GraphNodeInput{GraphNodeInput::Source::GraphInput, 2}},
                             add, error) == ST::Ok,
              "4b: add node");
        check(graph.set_outputs({add}, error) == ST::Ok, "4b: outputs");

        GraphValidation validation;
        check(validate_graph(graph, validation, error) == ST::Ok, "4b: graph valid");

        GraphExecutionPlan plan;
        MemoryPlannerConfig config;
        const TensorCapabilities& caps = executor->backends().front()->capabilities();
        check(make_execution_plan(graph, validation, caps, config, plan, error) == ST::Ok,
              "4b: plan built");
        check(plan.steps[0].output_slot != plan.steps[1].output_slot,
              "4b: shape-differing steps do NOT share a slot");
        check(plan.slots.size() == 2 && plan.planned_bytes == plan.naive_bytes,
              "4b: honest accounting (no reuse across shapes)");

        // And the planned run must actually EXECUTE with correct values.
        const float xd[6] = {1, 2, 3, 4, 5, 6};
        const float ad[6] = {10, 20, 30, 40, 50, 60};
        Tensor tx;
        Tensor ta;
        Tensor tb;
        check(Tensor::from_host(*manager, TensorShape::make({2, 3}), DataType::FP32, xd,
                                sizeof(xd), tx, error) == ST::Ok, "4b: host x");
        check(Tensor::from_host(*manager, TensorShape::make({2, 3}), DataType::FP32, ad,
                                sizeof(ad), ta, error) == ST::Ok, "4b: host a");
        check(Tensor::from_host(*manager, TensorShape::make({2, 3}), DataType::FP32, ad,
                                sizeof(ad), tb, error) == ST::Ok, "4b: host b");
        GraphExecutor graph_executor(*executor);
        GraphExecutionResult result = graph_executor.execute(graph, plan, {tx, ta, tb});
        check_status(result.status, ST::Ok, "4b: the valid graph EXECUTES");
        check(result.outputs.size() == 1 && result.outputs[0].shape() == TensorShape::make({2, 3}),
              "4b: output shape is the add output");
        const float expected[6] = {20, 40, 60, 80, 100, 120};
        check(same_floats(floats(result.outputs[0]), std::vector<float>(expected, expected + 6)),
              "4b: add output values are exact");
    }

    // =====================================================================
    // 5. Memory planner reuse: fewer allocations, bit-identical results
    // =====================================================================
    {
        // A chain where two 2x2 intermediates die before later 2x2 outputs:
        // t1 = ReLU(x); t2 = ReLU(t1); out = Add(t2, t1)  -> t1/t2 same size,
        // t2 can reuse t1's slot (t1's last use IS t2's step? no: out reads
        // t1 too). Construct a case with real reuse: t1 = ReLU(x);
        // t2 = Tanh(t1); t3 = Sigmoid(t2); out = Mul(t3, ones)
        // -> t1 dead after t2; t2 dead after t3; t3 pinned.
        TensorGraph graph;
        GraphInputDesc x;
        x.name = "x";
        x.shape = TensorShape::make({2, 2});
        x.dtype = DataType::FP32;
        check(graph.add_input(x, error) == ST::Ok, "x");

        NodeId t1 = kInvalidNodeId;
        NodeId t2 = kInvalidNodeId;
        NodeId t3 = kInvalidNodeId;
        check(graph.add_node(TensorOp::Relu, TensorOpParams{}, t1, error) == ST::Ok, "t1");
        check(graph.add_node(TensorOp::Tanh, TensorOpParams{}, t2, error) == ST::Ok, "t2");
        check(graph.add_node(TensorOp::Sigmoid, TensorOpParams{}, t3, error) == ST::Ok, "t3");

        GraphNodeInput fx;
        fx.source = GraphNodeInput::Source::GraphInput;
        fx.index = 0;
        GraphNodeInput f1;
        f1.source = GraphNodeInput::Source::NodeOutput;
        f1.index = static_cast<std::int32_t>(t1);
        GraphNodeInput f2;
        f2.source = GraphNodeInput::Source::NodeOutput;
        f2.index = static_cast<std::int32_t>(t2);
        check(graph.bind_input(t1, 0, fx, error) == ST::Ok, "t1<-x");
        check(graph.bind_input(t2, 0, f1, error) == ST::Ok, "t2<-t1");
        check(graph.bind_input(t3, 0, f2, error) == ST::Ok, "t3<-t2");
        check(graph.set_outputs({t3}, error) == ST::Ok, "outputs");

        GraphValidation validation;
        check(validate_graph(graph, validation, error) == ST::Ok, "valid");

        GraphExecutionPlan with_reuse;
        MemoryPlannerConfig reuse_on;
        reuse_on.enable_reuse = true;
        const TensorCapabilities& caps = executor->backends().front()->capabilities();
        check(make_execution_plan(graph, validation, caps, reuse_on, with_reuse, error) ==
                  ST::Ok, "reuse plan built");

        GraphExecutionPlan without_reuse;
        MemoryPlannerConfig reuse_off;
        reuse_off.enable_reuse = false;
        check(make_execution_plan(graph, validation, caps, reuse_off, without_reuse, error) ==
                  ST::Ok, "no-reuse plan built");

        // All outputs are 2x2 fp32 = 16 bytes; naive = 48. Reuse rules
        // (correctness first): t2 READS t1's slot in its own step, so it can
        // NOT write into it (no same-step aliasing) -> t2 gets a fresh slot;
        // t3 reads t2 (step 2) and t1's slot's last read was step 1 < 2, so
        // t3 reuses t1's slot. Result: TWO slots (32 bytes), not three.
        check(with_reuse.slots.size() == 2 && with_reuse.planned_bytes == 32,
              "liveness-safe reuse: two slots for a three-step chain");
        check(without_reuse.slots.size() == 3 && without_reuse.planned_bytes == 48,
              "no-reuse baseline allocates every output");
        check(with_reuse.naive_bytes == 48, "naive accounting honest");
        check(with_reuse.steps[0].output_slot == with_reuse.steps[2].output_slot &&
                  with_reuse.steps[1].output_slot != with_reuse.steps[0].output_slot,
              "t3 reuses t1's slot; t2 could not (same-step read/write alias)");

        // Execute BOTH plans on positive inputs; results must be identical.
        const float data[4] = {1.0f, -2.0f, 3.0f, -4.0f};
        Tensor tx;
        check(Tensor::from_host(*manager, TensorShape::make({2, 2}), DataType::FP32, data,
                                sizeof(data), tx, error) == ST::Ok, "input x");

        GraphExecutor graph_executor(*executor);
        GraphExecutionResult reuse_run = graph_executor.execute(graph, with_reuse, {tx});
        check_status(reuse_run.status, ST::Ok, "reuse run executes");
        GraphExecutionResult fresh_run = graph_executor.execute(graph, without_reuse, {tx});
        check_status(fresh_run.status, ST::Ok, "fresh run executes");

        check(reuse_run.outputs.size() == 1 && fresh_run.outputs.size() == 1,
              "one output each");
        check(same_floats(floats(reuse_run.outputs[0]), floats(fresh_run.outputs[0])),
              "reused-slot execution is bit-identical to the fresh-slot run");
        // x = {1,-2,3,-4}: relu -> {1,0,3,0}; tanh -> {tanh(1),0,tanh(3),0};
        // sigmoid -> {sig(tanh(1)),sig(0),sig(tanh(3)),sig(0)} (sigmoid(0)=0.5).
        const float v0 = 1.0f / (1.0f + std::exp(-std::tanh(1.0f)));
        const float v2 = 1.0f / (1.0f + std::exp(-std::tanh(3.0f)));
        check(same_floats(floats(reuse_run.outputs[0]), {v0, 0.5f, v2, 0.5f}),
              "chain values correct (sigmoid(0) = 0.5)");

        // The reuse run really allocated exactly the planned slot count.
        check(reuse_run.buffers_allocated == 2 && reuse_run.bytes_allocated == 32,
              "reuse run allocated two slot buffers (32 bytes)");
        check(fresh_run.buffers_allocated == 3, "fresh run allocated every slot");
    }

    // =====================================================================
    // 6. End-to-end graph execution with real numbers:
    //    out = ReLU(GEMM(A, B, C)) on hand-verified data
    // =====================================================================
    {
        TensorGraph graph;
        GraphInputDesc a_desc;
        a_desc.name = "A";
        a_desc.shape = TensorShape::make({1, 2});
        a_desc.dtype = DataType::FP32;
        GraphInputDesc b_desc;
        b_desc.name = "B";
        b_desc.shape = TensorShape::make({2, 1});
        b_desc.dtype = DataType::FP32;
        GraphInputDesc c_desc;
        c_desc.name = "C";
        c_desc.shape = TensorShape::make({1, 1});
        c_desc.dtype = DataType::FP32;
        check(graph.add_input(a_desc, error) == ST::Ok, "A");
        check(graph.add_input(b_desc, error) == ST::Ok, "B");
        check(graph.add_input(c_desc, error) == ST::Ok, "C");

        NodeId gemm = kInvalidNodeId;
        NodeId relu = kInvalidNodeId;
        TensorOpParams gemm_params;
        gemm_params.gemm_alpha = 2.0f;
        gemm_params.gemm_beta = 0.0f;
        check(graph.add_node(TensorOp::GEMM, gemm_params, gemm, error) == ST::Ok, "gemm node");
        check(graph.add_node(TensorOp::Relu, TensorOpParams{}, relu, error) == ST::Ok,
              "relu node");

        GraphNodeInput fa;
        fa.source = GraphNodeInput::Source::GraphInput;
        fa.index = 0;
        GraphNodeInput fb;
        fb.source = GraphNodeInput::Source::GraphInput;
        fb.index = 1;
        GraphNodeInput fc;
        fc.source = GraphNodeInput::Source::GraphInput;
        fc.index = 2;
        GraphNodeInput fgemm;
        fgemm.source = GraphNodeInput::Source::NodeOutput;
        fgemm.index = static_cast<std::int32_t>(gemm);
        check(graph.bind_input(gemm, 0, fa, error) == ST::Ok, "gemm<-A");
        check(graph.bind_input(gemm, 1, fb, error) == ST::Ok, "gemm<-B");
        check(graph.bind_input(gemm, 2, fc, error) == ST::Ok, "gemm<-C");
        check(graph.bind_input(relu, 0, fgemm, error) == ST::Ok, "relu<-gemm");
        check(graph.set_outputs({relu}, error) == ST::Ok, "outputs");

        // A=[[1,2]] B=[[3],[4]] C=[[0]] -> 2*(1*3+2*4) = 22 -> relu = 22.
        const float a_data[2] = {1, 2};
        const float b_data[2] = {3, 4};
        const float c_data[1] = {0};
        Tensor ta;
        Tensor tb;
        Tensor tc;
        check(Tensor::from_host(*manager, TensorShape::make({1, 2}), DataType::FP32, a_data,
                                sizeof(a_data), ta, error) == ST::Ok, "A data");
        check(Tensor::from_host(*manager, TensorShape::make({2, 1}), DataType::FP32, b_data,
                                sizeof(b_data), tb, error) == ST::Ok, "B data");
        check(Tensor::from_host(*manager, TensorShape::make({1, 1}), DataType::FP32, c_data,
                                sizeof(c_data), tc, error) == ST::Ok, "C data");

        GraphExecutor graph_executor(*executor);
        GraphExecutionResult result = graph_executor.execute(graph, {ta, tb, tc});
        check_status(result.status, ST::Ok, "end-to-end execution");
        check(result.outputs.size() == 1, "one output");
        check(same_floats(floats(result.outputs[0]), {22.0f}), "end-to-end value 2*(AB)=22");
        check(result.trace.size() == 2, "trace has one entry per step");
        check(result.trace[0].node_id == gemm && result.trace[1].node_id == relu,
              "trace order matches execution");
        check(result.trace[0].timing_measured && result.trace[0].elapsed_ns >= 0,
              "trace carries a real (non-fabricated) measurement");
        check(result.buffers_allocated == 2, "gemm + relu slots allocated");

        // Negative result clipped by ReLU: run the single-op path on -22.
        Tensor neg;
        const float neg_data[1] = {-22.0f};
        check(Tensor::from_host(*manager, TensorShape::make({1, 1}), DataType::FP32, neg_data,
                                sizeof(neg_data), neg, error) == ST::Ok, "neg tensor");
        Tensor clipped;
        check_status(executor->execute_op(TensorOp::Relu, TensorOpParams{}, {neg}, clipped,
                                          error),
                     ST::Ok, "relu on -22");
        check(same_floats(floats(clipped), {0.0f}), "relu clips the negative gemm result");

        // Binding validation: wrong shape refused with the slot named.
        const float wide_data[4] = {1, 2, 3, 4};  // 2x2 = 16 bytes of real data
        Tensor wrong_a;
        check(Tensor::from_host(*manager, TensorShape::make({2, 2}), DataType::FP32, wide_data,
                                sizeof(wide_data), wrong_a, error) == ST::Ok,
              "wrong-shaped A");
        GraphExecutionResult refused = graph_executor.execute(graph, {wrong_a, tb, tc});
        check_status(refused.status, ST::InvalidShape, "shape contract enforced");
        check(refused.error.find("A") != std::string::npos, "the error names the slot");

        // Wrong dtype refused.
        const std::int32_t int_data[2] = {1, 2};
        Tensor int_a;
        check(Tensor::from_host(*manager, TensorShape::make({1, 2}), DataType::INT32, int_data,
                                sizeof(int_data), int_a, error) == ST::Ok, "int A");
        GraphExecutionResult dtype_refused = graph_executor.execute(graph, {int_a, tb, tc});
        check_status(dtype_refused.status, ST::DtypeMismatch, "dtype contract enforced");
    }

    if (failures == 0) {
        std::cout << "Tensor graph tests passed.\n";
        return 0;
    }
    std::cerr << failures << " tensor graph test(s) failed.\n";
    return 1;
}
