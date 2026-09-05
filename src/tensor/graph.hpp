#pragma once

// TensorGraph (Phase 13) — the AI/ML compute graph.
//
// A graph is a small DAG of tensor operations: NAMED INPUT SLOTS (declared
// shape + dtype) -> op nodes (validated at planning time) -> an explicit
// OUTPUT list. It is the unit a future production scheduler submits; Phase
// 13 executes it in-process through the GraphExecutor.
//
// DETERMINISM (the property the spec demands and the tests pin):
//   - Node ids are assigned by insertion order (1-based, 0 = invalid); the
//     same construction sequence always produces the same ids.
//   - The execution order is a deterministic topological order: Kahn's
//     algorithm processing the SMALLEST ready node id first (explicit
//     ordering rule — no map iteration order, no thread scheduling input).
//   - The same graph + same inputs + same capabilities + same planner
//     configuration always produce the same plan (pinned by tests through
//     plan serialization equality).
//
// VALIDATION (validate(), pure): every node input bound and resolvable,
// every op's rules hold via validate_op (shape/dtype inference propagates
// through the graph), cycle detection (two-phase binding CAN create one —
// the planner refuses it with InvalidState and names the nodes involved),
// duplicate outputs refused, and the explicit size caps enforced
// (kMaxGraphNodes / kMaxGraphInputs / kMaxGraphOutputs — resource-exhaustion
// defense for any future remote-submission path).
//
// EXTENSION POINTS (deliberate, documented — nothing here pretends they
// exist yet): backward/autograd metadata on nodes (the struct has none — a
// future training phase adds it additively), fusion candidates (the plan
// layer, not the graph, decides what fuses), quantization parameters (the
// dtype vocabulary has no quantized entry yet).

#include <cstdint>
#include <string>
#include <vector>

#include "tensor/op.hpp"
#include "tensor/status.hpp"

namespace vortyx::tensor {

// Explicit graph size caps (resource-exhaustion defense; generous for real
// graphs, small enough that a hostile submission cannot exhaust anything).
inline constexpr std::uint32_t kMaxGraphNodes = 256;
inline constexpr std::uint32_t kMaxGraphInputs = 16;
inline constexpr std::uint32_t kMaxGraphOutputs = 16;

using NodeId = std::uint32_t;
inline constexpr NodeId kInvalidNodeId = 0;

// One declared graph input (shape/dtype are CONTRACTS checked against the
// actual binding at execution time).
struct GraphInputDesc {
    std::string name;             // unique, non-empty
    TensorShape shape;
    DataType dtype = DataType::FP32;
};

// Where one node input comes from.
struct GraphNodeInput {
    enum class Source : std::uint8_t {
        Unbound = 0,      // not yet bound (two-phase construction) — validation refuses it
        GraphInput = 1,   // 'index' is the graph input slot (0-based)
        NodeOutput = 2,   // 'index' is the producing node's id
    };
    Source source = Source::Unbound;
    std::int32_t index = 0;
};

// One op node. Phase 13 nodes carry no scheduling hints, no backward
// metadata, no quantization parameters — those are future additive fields.
struct GraphNode {
    NodeId node_id = kInvalidNodeId;  // assigned by the graph (insertion order)
    TensorOp op = TensorOp::Add;
    TensorOpParams params;
    std::vector<GraphNodeInput> inputs;  // exactly tensor_op_arity(op) entries
};

class TensorGraph {
public:
    TensorGraph() = default;

    // -- construction -------------------------------------------------------

    // Adds a named input slot. Duplicated names are refused (InvalidInput);
    // the slot count cap is enforced (ResourceLimitExceeded).
    TensorStatus add_input(const GraphInputDesc& desc, std::string& error);

    // Two-phase node creation (allows building patterns where a consumer is
    // declared before its producer binds — cycles are then possible and are
    // caught by validation, which is exactly the contract the spec asks
    // for). Returns the new deterministic node id.
    TensorStatus add_node(TensorOp op, const TensorOpParams& params, NodeId& out_id,
                          std::string& error);

    // Convenience: add a node with inputs bound immediately to EXISTING
    // sources (unknown sources are refused at bind time).
    TensorStatus add_node(TensorOp op, const TensorOpParams& params,
                          const std::vector<GraphNodeInput>& inputs, NodeId& out_id,
                          std::string& error);

    // Binds one input slot of a node. Re-binding the same slot replaces the
    // binding (the last binding is the truth — deterministic).
    TensorStatus bind_input(NodeId node_id, std::size_t slot, GraphNodeInput source,
                            std::string& error);

    // Sets the output list (node ids). Duplicate outputs are refused
    // (InvalidState) — one output per node per graph.
    TensorStatus set_outputs(const std::vector<NodeId>& outputs, std::string& error);

    // -- inspection -----------------------------------------------------------

    const std::vector<GraphInputDesc>& inputs() const { return inputs_; }
    const std::vector<GraphNode>& nodes() const { return nodes_; }
    const std::vector<NodeId>& outputs() const { return outputs_; }
    std::uint32_t node_count() const { return static_cast<std::uint32_t>(nodes_.size()); }

    const GraphNode* find(NodeId node_id) const;  // nullptr when unknown

private:
    std::vector<GraphInputDesc> inputs_;
    std::vector<GraphNode> nodes_;  // insertion order; node_id == index + 1
    std::vector<NodeId> outputs_;
};

// ---------------------------------------------------------------------------
// Validation result: per-node inferred output descriptors (the planner's
// input), produced by the SAME validate_op rules the executor uses.
// ---------------------------------------------------------------------------

struct GraphValidation {
    // Indexed by node position (nodes_[i] -> outputs[i]).
    std::vector<TensorOpOutputDesc> node_outputs;
};

// Full validation (pure). Returns Ok and fills 'validation', or the precise
// failing status with 'error' naming the offending node/slot:
//   unbound node input            -> InvalidState
//   input slot out of range       -> InvalidState
//   reference to unknown node     -> InvalidState
//   graph input slot unknown      -> InvalidState
//   op rule violation             -> validate_op's own status (InvalidShape,
//                                    DtypeMismatch, UnsupportedDtype, ...)
//   cycle                         -> InvalidState (names the nodes)
//   duplicate output              -> InvalidState
//   output references unknown node-> InvalidState
//   node/input/output caps        -> ResourceLimitExceeded
TensorStatus validate_graph(const TensorGraph& graph, GraphValidation& validation,
                            std::string& error);

// The deterministic topological order (smallest-ready-node-id-first Kahn).
// Returns Ok with 'order' filled (node ids in execution order), or
// InvalidState when the graph contains a cycle (validate_graph reports the
// same condition; this function is the ordering primitive).
TensorStatus graph_topological_order(const TensorGraph& graph, std::vector<NodeId>& order,
                                     std::string& error);

}  // namespace vortyx::tensor
