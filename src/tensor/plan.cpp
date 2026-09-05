// Graph execution planning (Phase 13) — implementation.

#include "tensor/plan.hpp"

#include <algorithm>

#include "tensor/storage.hpp"  // tensor_byte_size (the checked byte math)

namespace vortyx::tensor {

namespace {

// The plan's input translation of one node input (pure).
bool translate_node_input(const TensorGraph& graph,
                          const std::vector<std::int32_t>& step_of_node,
                          const GraphNodeInput& input, PlanStepInput& out, std::string& error) {
    switch (input.source) {
        case GraphNodeInput::Source::Unbound:
            error = "unbound node input reached planning (validation gap)";
            return false;
        case GraphNodeInput::Source::GraphInput:
            if (input.index < 0 || static_cast<std::size_t>(input.index) >= graph.inputs().size()) {
                error = "graph input slot " + std::to_string(input.index) + " does not exist";
                return false;
            }
            out.source = PlanStepInput::Source::GraphInput;
            out.index = input.index;
            return true;
        case GraphNodeInput::Source::NodeOutput: {
            if (input.index <= 0 ||
                static_cast<std::size_t>(input.index) >= step_of_node.size() ||
                step_of_node[static_cast<std::size_t>(input.index)] < 0) {
                error = "node input references node " + std::to_string(input.index) +
                        " which has no plan step (unknown node)";
                return false;
            }
            out.source = PlanStepInput::Source::Slot;
            out.index = step_of_node[static_cast<std::size_t>(input.index)];
            return true;
        }
    }
    error = "unknown node input source";
    return false;
}

}  // namespace

TensorStatus make_execution_plan(const TensorGraph& graph, const GraphValidation& validation,
                                 const TensorCapabilities& capabilities,
                                 const MemoryPlannerConfig& config, GraphExecutionPlan& plan,
                                 std::string& error) {
    // --- stage 1: graph validation (already done by the caller? re-run the
    // cheap invariant checks this function depends on — the order). The
    // caller passes the validation result; the topological order is
    // recomputed here from the same pure function (deterministic).
    std::vector<NodeId> order;
    const TensorStatus order_status = graph_topological_order(graph, order, error);
    if (order_status != TensorStatus::Ok) return order_status;
    if (order.size() != graph.node_count() || validation.node_outputs.size() != graph.node_count()) {
        error = "planning requires a validated graph (node/output count mismatch)";
        return TensorStatus::InvalidInput;
    }

    // --- stage 2: capability check per node (against the INFERRED dtypes) --
    for (const NodeId node_id : order) {
        const GraphNode& node = *graph.find(node_id);
        if (!capabilities.supports_op(node.op)) {
            error = std::string("node ") + std::to_string(node_id) + " requires op '" +
                    to_string(node.op) + "' which the target does not support";
            return TensorStatus::UnsupportedOperation;
        }
        const TensorOpOutputDesc& desc = validation.node_outputs[node_id - 1];
        if (!capabilities.supports_dtype(desc.dtype)) {
            error = std::string("node ") + std::to_string(node_id) + " produces dtype '" +
                    to_string(desc.dtype) + "' which the target does not support";
            return TensorStatus::UnsupportedDtype;
        }
        if (desc.shape.rank() > capabilities.max_rank) {
            error = "node " + std::to_string(node_id) + " output rank " +
                    std::to_string(desc.shape.rank()) + " exceeds the target's max rank " +
                    std::to_string(capabilities.max_rank);
            return TensorStatus::ResourceLimitExceeded;
        }
        std::int64_t out_elements = 0;
        if (!desc.shape.total_elements(out_elements)) {
            error = "node " + std::to_string(node_id) + " output element count overflows";
            return TensorStatus::ResourceLimitExceeded;
        }
        std::int64_t out_bytes = 0;
        if (!tensor_byte_size(out_elements, desc.dtype, out_bytes) ||
            out_bytes > capabilities.max_bytes) {
            error = "node " + std::to_string(node_id) + " output byte size exceeds the "
                    "target's per-tensor limit";
            return TensorStatus::ResourceLimitExceeded;
        }
    }

    // --- stage 3: steps in topological order --------------------------------
    // step_of_node[node_id] = the step index that writes that node's output.
    std::vector<std::int32_t> step_of_node(graph.node_count() + 1, -1);
    plan.steps.clear();
    plan.steps.reserve(order.size());

    for (std::size_t s = 0; s < order.size(); ++s) {
        const NodeId node_id = order[s];
        const GraphNode& node = *graph.find(node_id);
        const TensorOpOutputDesc& desc = validation.node_outputs[node_id - 1];

        PlanStep step;
        step.node_id = node_id;
        step.op = node.op;
        step.params = node.params;
        step.output_shape = desc.shape;
        step.output_dtype = desc.dtype;

        for (std::size_t slot = 0; slot < node.inputs.size(); ++slot) {
            PlanStepInput translated;
            if (!translate_node_input(graph, step_of_node, node.inputs[slot], translated,
                                      error)) {
                error = "node " + std::to_string(node_id) + ": " + error;
                return TensorStatus::InvalidState;
            }
            step.inputs.push_back(translated);
        }
        step.backend = "";  // resolved by the executor's dispatch (recorded post-run)
        step_of_node[node_id] = static_cast<std::int32_t>(s);
        plan.steps.push_back(std::move(step));
    }

    // --- stage 4: memory planning (liveness, exact-size first-fit reuse) ----
    plan.slots.clear();
    plan.naive_bytes = 0;
    plan.planned_bytes = 0;

    // last_use[step_index] = the latest step that READS the output of that
    // step (-1 = never read again after definition). Graph outputs are
    // PINNED: their buffers back the returned tensors and are never reused.
    std::vector<std::int32_t> last_use(order.size(), -1);
    std::vector<bool> pinned(order.size(), false);
    for (const NodeId output_node : graph.outputs()) {
        pinned[static_cast<std::size_t>(step_of_node[output_node])] = true;
    }
    for (std::size_t s = 0; s < plan.steps.size(); ++s) {
        for (const PlanStepInput& input : plan.steps[s].inputs) {
            if (input.source == PlanStepInput::Source::Slot) {
                last_use[static_cast<std::size_t>(input.index)] = static_cast<std::int32_t>(s);
            }
        }
    }

    // Mutable planner-side slot state. def_step is the MOST RECENT
    // definition (a reuse re-defines the slot, so the liveness owner moves
    // with it); a slot may back a new definition at step s exactly when
    // last_use[def_step] < s (strictly-after rule — no same-step aliasing).
    struct SlotState {
        TensorShape shape;     // ALL definitions of this slot share it (reuse
                               // requires exact shape equality — see below)
        std::int64_t byte_size = 0;
        DataType dtype = DataType::FP32;
        std::size_t def_step = 0;
        NodeId definer = kInvalidNodeId;
        bool pinned = false;  // graph output — its buffer outlives the plan
    };
    std::vector<SlotState> slots;  // index == slot_id (creation order)

    for (std::size_t s = 0; s < plan.steps.size(); ++s) {
        PlanStep& step = plan.steps[s];
        std::int64_t out_elements = 0;
        if (!step.output_shape.total_elements(out_elements)) {
            error = "node " + std::to_string(step.node_id) + " output element count overflows";
            return TensorStatus::ResourceLimitExceeded;
        }
        std::int64_t out_bytes = 0;
        if (!tensor_byte_size(out_elements, step.output_dtype, out_bytes)) {
            error = "node " + std::to_string(step.node_id) + " output byte size overflows";
            return TensorStatus::ResourceLimitExceeded;
        }
        plan.naive_bytes += out_bytes;

        // Reuse safety rule (the planner's correctness core):
        //   - the slot is not pinned (graph outputs outlive the plan),
        //   - its MOST RECENT definition's last READ is strictly before this
        //     step (no same-step read/write aliasing),
        //   - shape, byte size and dtype match EXACTLY (no partial overlap,
        //     no reinterpretation). Shape equality is REQUIRED, not just byte
        //     equality: the executor allocates each slot ONCE (with the
        //     slot's shape) and every producing kernel verifies the provided
        //     slot tensor against its inferred output shape — a byte-equal
        //     but differently-shaped redefinition would produce a plan that
        //     can never execute (regression: the Phase 13 audit repro —
        //     Reshape[2,3]->[1,6] followed by Add[2,3]+[2,3] planned into one
        //     slot then failed at execution with InvalidShape).
        std::int32_t assigned = -1;
        if (config.enable_reuse) {
            for (std::size_t i = 0; i < slots.size() && assigned < 0; ++i) {
                SlotState& candidate = slots[i];
                if (candidate.pinned) continue;
                if (last_use[candidate.def_step] >= static_cast<std::int32_t>(s)) continue;
                if (candidate.byte_size != out_bytes || candidate.dtype != step.output_dtype ||
                    !(candidate.shape == step.output_shape)) {
                    continue;
                }
                assigned = static_cast<std::int32_t>(i);
                candidate.def_step = s;            // the liveness owner moves
                candidate.definer = step.node_id;  // to the new definition
                if (pinned[s]) {
                    // A graph output now lives in this slot: it stays alive
                    // beyond the plan from here on.
                    candidate.pinned = true;
                }
            }
        }
        if (assigned < 0) {
            SlotState fresh;
            fresh.shape = step.output_shape;
            fresh.byte_size = out_bytes;
            fresh.dtype = step.output_dtype;
            fresh.def_step = s;
            fresh.definer = step.node_id;
            fresh.pinned = pinned[s];
            assigned = static_cast<std::int32_t>(slots.size());
            slots.push_back(fresh);
        }
        step.output_slot = assigned;
    }

    // Materialize the plan's slot table (creation order = slot id order).
    // With shape-equality reuse, every definition of a slot shares one shape.
    for (const SlotState& state : slots) {
        PlanSlot slot;
        slot.slot_id = static_cast<std::int32_t>(plan.slots.size());
        slot.byte_size = state.byte_size;
        slot.dtype = state.dtype;
        slot.shape = state.shape;
        slot.defined_by = state.definer;
        slot.pinned = state.pinned;
        plan.slots.push_back(std::move(slot));
    }

    for (const PlanSlot& slot : plan.slots) {
        plan.planned_bytes += slot.byte_size;
    }
    return TensorStatus::Ok;
}

}  // namespace vortyx::tensor
