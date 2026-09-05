#pragma once

// GraphExecutor (Phase 13) — runs a planned graph end to end.
//
//   bindings (validated against the declared inputs) -> slot allocation
//   (through the Phase 4 resource system) -> steps in plan order -> outputs
//
// GUARANTEES (implemented + tested, not aspirational):
//   - Bindings are checked EXACTLY: count, name order, shape and dtype must
//     match the graph's declared inputs (InvalidShape / DtypeMismatch with
//     the slot named otherwise).
//   - Placement: every binding must be Host or on the SAME device; a
//     cross-device binding set is TransferUnsupported (no transfer exists).
//   - Execution follows the plan's step order (deterministic); each step
//     dispatches through the TensorExecutor's capability dispatch.
//   - Memory follows the plan's slots: reused slots are written fully by
//     their producing kernel before any read (the planner guarantees the
//     liveness); a planned run is bit-identical to an unplanned run (pinned
//     by tests).
//   - Outputs are returned as live tensors sharing their (pinned) slot
//     storage — no copies, no dangling references (shared ownership).
//   - The per-step trace records real data only: node id, op, status, and a
//     steady_clock elapsed measurement for steps that ran (real timing,
//     never fabricated; tests assert structure, never values).
//
// Threading: externally serialized (like the TensorExecutor it uses). All
// state is per-call; no global mutable state.

#include <string>
#include <vector>

#include "tensor/executor.hpp"
#include "tensor/graph.hpp"
#include "tensor/plan.hpp"
#include "tensor/status.hpp"
#include "tensor/tensor_value.hpp"

namespace vortyx::tensor {

// One step's observable record (real data only — see the module header).
struct GraphStepTrace {
    NodeId node_id = kInvalidNodeId;
    TensorOp op = TensorOp::Add;
    std::string backend;             // backend that executed the step ("" if it never ran)
    TensorStatus status = TensorStatus::Ok;
    std::string error;               // failure reason when status != Ok
    std::int64_t elapsed_ns = 0;     // real steady_clock measurement; 0 when not measured
    bool timing_measured = false;    // explicit: elapsed_ns is a real measurement
};

struct GraphExecutionResult {
    TensorStatus status = TensorStatus::Ok;   // Ok only when EVERY step succeeded
    std::string error;                        // aggregate failure reason (first failure)

    std::vector<Tensor> outputs;              // one per graph output, in output order
    std::vector<GraphStepTrace> trace;        // one per step, in execution order

    // Honest resource accounting for THIS run (from the Phase 4 manager).
    std::size_t buffers_allocated = 0;        // slot storages created
    std::int64_t bytes_allocated = 0;         // their total size
};

class GraphExecutor {
public:
    // 'executor' must outlive the call (all allocation flows through it).
    explicit GraphExecutor(TensorExecutor& executor);

    // Executes 'graph' (already planned into 'plan' — the caller plans once
    // and may execute many times; re-planning per call is also fine).
    // 'inputs' must have exactly graph.inputs().size() entries, in slot
    // order, matching the declared shapes/dtypes exactly.
    GraphExecutionResult execute(const TensorGraph& graph, const GraphExecutionPlan& plan,
                                 const std::vector<Tensor>& inputs);

    // Convenience: validate + plan + execute in one call (the planner runs
    // with the capabilities of the executor's FIRST backend — the dispatch
    // target — which is the honest target selection for a local run).
    GraphExecutionResult execute(const TensorGraph& graph,
                                 const std::vector<Tensor>& inputs);

private:
    TensorExecutor* executor_;
};

}  // namespace vortyx::tensor
