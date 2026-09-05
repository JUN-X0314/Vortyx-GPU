#pragma once

// Graph execution planning (Phase 13) — validation -> capability check ->
// ordering -> memory planning -> a deterministic plan.
//
// A graph is NEVER executed directly: the planner turns it into a
// GraphExecutionPlan first, and the GraphExecutor runs the plan. The plan is
// a plain value (copyable, serializable) — the provider-neutral artifact a
// future production scheduler (Phase 14/15) or the Adaptive Compute Fabric
// can consume without seeing the graph type.
//
// PLANNING STAGES (all deterministic, each pinned by tests):
//   1. validate_graph            (the shared rule set + cycle detection)
//   2. capability check          (every node's op AND inferred output dtype
//                                 must be covered by the target capabilities;
//                                 the failure names the node — planning a
//                                 graph a device cannot run is refused, never
//                                 guessed into "probably fine")
//   3. topological order         (smallest-ready-node-id-first Kahn — the
//                                 same graph always yields the same order)
//   4. memory planning           (liveness-based buffer slot reuse)
//
// MEMORY PLANNER (correctness first, by construction):
//   - Each node output gets a SLOT; slots are REUSED (first-fit by ascending
//     slot id) when the byte sizes match EXACTLY and the slot's last use is
//     strictly before the new definition — no same-step read/write aliasing,
//     no partial-overlap risk (exact size match, no suballocation).
//   - Graph OUTPUT slots are PINNED: their buffers back the returned tensors
//     and are never reused while the outputs live.
//   - Reuse requires byte equality, so a reused slot can never overwrite a
//     differently-sized live tensor, and every execution writes its slot
//     fully before any read (kernels write whole outputs).
//   - The plan reports reuse honestly: slots / unique bytes vs the naive
//     all-fresh allocation, so tests can pin the counting (and the executor
//     can verify a planned run matches an unplanned run bit-exactly).
//
// DETERMINISM CONTRACT: same graph + same input descriptors + same
// capabilities + same MemoryPlannerConfig -> byte-identical plan (verified
// by serialize/compare in tests).

#include <cstdint>
#include <string>
#include <vector>

#include "tensor/capability.hpp"
#include "tensor/graph.hpp"
#include "tensor/status.hpp"

namespace vortyx::tensor {

// Memory planner configuration (the planner's only tuning surface; nothing
// here is a performance claim).
struct MemoryPlannerConfig {
    // When false, every node output gets a fresh slot (the correctness
    // baseline the reuse path is tested against).
    bool enable_reuse = true;
};

// One input of one planned step.
struct PlanStepInput {
    enum class Source : std::uint8_t {
        GraphInput = 0,  // 'index' is the graph input slot
        Slot = 1,        // 'index' is the plan slot holding a prior output
    };
    Source source = Source::GraphInput;
    std::int32_t index = 0;
};

// One planned step: WHICH node, in WHICH order, from WHERE it reads, WHERE
// it writes, on WHICH backend.
struct PlanStep {
    NodeId node_id = kInvalidNodeId;
    TensorOp op = TensorOp::Add;
    TensorOpParams params;
    std::vector<PlanStepInput> inputs;

    std::int32_t output_slot = 0;   // the slot this step writes
    TensorShape output_shape;
    DataType output_dtype = DataType::FP32;
    std::string backend;            // resolved backend name (dispatch vocabulary)
};

// One planned buffer slot (the memory plan).
struct PlanSlot {
    std::int32_t slot_id = 0;      // 0-based, assigned in definition order
    std::int64_t byte_size = 0;
    DataType dtype = DataType::FP32;
    TensorShape shape;             // the FIRST definition's shape (exact reuse
                                   // requires equal bytes; shape kept for clarity)
    NodeId defined_by = kInvalidNodeId;
    bool pinned = false;           // graph output slot — never reused
};

struct GraphExecutionPlan {
    std::vector<PlanStep> steps;   // execution order (deterministic)
    std::vector<PlanSlot> slots;   // definition order (deterministic)

    // Honest accounting: what the memory plan saved vs all-fresh allocation.
    std::int64_t naive_bytes = 0;  // sum of every node output's bytes
    std::int64_t planned_bytes = 0;  // sum of unique slot bytes

    // The capabilities the plan was checked against (part of plan identity).
    // Not embedded — the planner's caller records them; the plan itself is
    // the ordering + memory artifact.
};

// Builds the execution plan. Pure. Returns Ok with 'plan' filled, or the
// precise failure:
//   graph validation failure        -> validate_graph's own status
//   node op outside capabilities    -> UnsupportedOperation (names the node)
//   node dtype outside capabilities -> UnsupportedDtype (names the node)
//   rank/byte limits exceeded       -> ResourceLimitExceeded (names the node)
TensorStatus make_execution_plan(const TensorGraph& graph,
                                 const GraphValidation& validation,
                                 const TensorCapabilities& capabilities,
                                 const MemoryPlannerConfig& config,
                                 GraphExecutionPlan& plan, std::string& error);

}  // namespace vortyx::tensor
