// TensorGraph (Phase 13) — implementation.

#include "tensor/graph.hpp"

#include <algorithm>

namespace vortyx::tensor {

TensorStatus TensorGraph::add_input(const GraphInputDesc& desc, std::string& error) {
    if (inputs_.size() >= kMaxGraphInputs) {
        error = "graph input count exceeds the limit of " + std::to_string(kMaxGraphInputs);
        return TensorStatus::ResourceLimitExceeded;
    }
    if (desc.name.empty()) {
        error = "graph input slots must be named";
        return TensorStatus::InvalidInput;
    }
    const TensorStatus shape_status = desc.shape.validate(error);
    if (shape_status != TensorStatus::Ok) {
        error = "graph input '" + desc.name + "': " + error;
        return shape_status;
    }
    for (const GraphInputDesc& existing : inputs_) {
        if (existing.name == desc.name) {
            error = "duplicate graph input name '" + desc.name + "'";
            return TensorStatus::InvalidInput;
        }
    }
    inputs_.push_back(desc);
    return TensorStatus::Ok;
}

TensorStatus TensorGraph::add_node(TensorOp op, const TensorOpParams& params, NodeId& out_id,
                                   std::string& error) {
    if (nodes_.size() >= kMaxGraphNodes) {
        error = "graph node count exceeds the limit of " + std::to_string(kMaxGraphNodes);
        return TensorStatus::ResourceLimitExceeded;
    }
    GraphNode node;
    node.node_id = static_cast<NodeId>(nodes_.size() + 1);  // deterministic, 1-based
    node.op = op;
    node.params = params;
    node.inputs.assign(tensor_op_arity(op), GraphNodeInput{});
    nodes_.push_back(std::move(node));
    out_id = nodes_.back().node_id;
    return TensorStatus::Ok;
}

TensorStatus TensorGraph::add_node(TensorOp op, const TensorOpParams& params,
                                   const std::vector<GraphNodeInput>& inputs, NodeId& out_id,
                                   std::string& error) {
    const TensorStatus status = add_node(op, params, out_id, error);
    if (status != TensorStatus::Ok) return status;
    if (inputs.size() != tensor_op_arity(op)) {
        error = std::string("node ") + std::to_string(out_id) + " ('" + to_string(op) +
                "') requires " + std::to_string(tensor_op_arity(op)) + " inputs, got " +
                std::to_string(inputs.size());
        return TensorStatus::InvalidInput;
    }
    GraphNode& node = nodes_[out_id - 1];
    node.inputs = inputs;
    return TensorStatus::Ok;
}

TensorStatus TensorGraph::bind_input(NodeId node_id, std::size_t slot, GraphNodeInput source,
                                     std::string& error) {
    if (node_id == kInvalidNodeId || node_id > nodes_.size()) {
        error = "bind_input references unknown node " + std::to_string(node_id);
        return TensorStatus::InvalidState;
    }
    GraphNode& node = nodes_[node_id - 1];
    if (slot >= node.inputs.size()) {
        error = "node " + std::to_string(node_id) + " input slot " + std::to_string(slot) +
                " out of range";
        return TensorStatus::InvalidState;
    }
    node.inputs[slot] = source;
    return TensorStatus::Ok;
}

TensorStatus TensorGraph::set_outputs(const std::vector<NodeId>& outputs, std::string& error) {
    if (outputs.empty()) {
        error = "a graph must declare at least one output";
        return TensorStatus::InvalidInput;
    }
    if (outputs.size() > kMaxGraphOutputs) {
        error = "graph output count exceeds the limit of " + std::to_string(kMaxGraphOutputs);
        return TensorStatus::ResourceLimitExceeded;
    }
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i] == kInvalidNodeId || outputs[i] > nodes_.size()) {
            error = "output " + std::to_string(i) + " references unknown node " +
                    std::to_string(outputs[i]);
            return TensorStatus::InvalidState;
        }
        for (std::size_t j = i + 1; j < outputs.size(); ++j) {
            if (outputs[i] == outputs[j]) {
                error = "duplicate output: node " + std::to_string(outputs[i]) +
                        " appears twice in the output list";
                return TensorStatus::InvalidState;
            }
        }
    }
    outputs_ = outputs;
    return TensorStatus::Ok;
}

const GraphNode* TensorGraph::find(NodeId node_id) const {
    if (node_id == kInvalidNodeId || node_id > nodes_.size()) return nullptr;
    return &nodes_[node_id - 1];
}

namespace {

// Resolves one node input to its (shape, dtype) description. 'validation'
// carries the already-inferred outputs of earlier-processed nodes.
struct SourceDesc {
    TensorShape shape;
    DataType dtype = DataType::FP32;
    bool contiguous = true;
};

bool resolve_node_input(const TensorGraph& graph, const GraphValidation& partial,
                        const GraphNode& node, std::size_t slot, SourceDesc& out,
                        std::string& error) {
    const GraphNodeInput& input = node.inputs[slot];
    switch (input.source) {
        case GraphNodeInput::Source::Unbound:
            error = "node " + std::to_string(node.node_id) + " input " + std::to_string(slot) +
                    " is not bound";
            return false;
        case GraphNodeInput::Source::GraphInput:
            if (input.index < 0 || static_cast<std::size_t>(input.index) >= graph.inputs().size()) {
                error = "node " + std::to_string(node.node_id) + " input " +
                        std::to_string(slot) + " references graph input slot " +
                        std::to_string(input.index) + " which does not exist";
                return false;
            }
            out.shape = graph.inputs()[static_cast<std::size_t>(input.index)].shape;
            out.dtype = graph.inputs()[static_cast<std::size_t>(input.index)].dtype;
            out.contiguous = true;  // bindings arrive contiguous (checked at execution)
            return true;
        case GraphNodeInput::Source::NodeOutput:
            if (input.index <= 0 || static_cast<std::size_t>(input.index) > graph.node_count()) {
                error = "node " + std::to_string(node.node_id) + " input " +
                        std::to_string(slot) + " references node " + std::to_string(input.index) +
                        " which does not exist";
                return false;
            }
            {
                const TensorOpOutputDesc& desc =
                    partial.node_outputs[static_cast<std::size_t>(input.index) - 1];
                out.shape = desc.shape;
                out.dtype = desc.dtype;
                out.contiguous = desc.contiguous;
            }
            return true;
    }
    error = "unknown input source kind";
    return false;
}

}  // namespace

TensorStatus graph_topological_order(const TensorGraph& graph, std::vector<NodeId>& order,
                                     std::string& error) {
    const std::uint32_t n = graph.node_count();
    order.clear();
    order.reserve(n);

    // in-degree per node (1-based ids -> index id-1), edges: producer -> consumer.
    std::vector<std::uint32_t> indegree(n, 0);
    std::vector<std::vector<NodeId>> consumers(n);
    for (const GraphNode& node : graph.nodes()) {
        for (std::size_t slot = 0; slot < node.inputs.size(); ++slot) {
            const GraphNodeInput& input = node.inputs[slot];
            if (input.source == GraphNodeInput::Source::NodeOutput) {
                if (input.index <= 0 || static_cast<std::size_t>(input.index) > n) {
                    error = "node " + std::to_string(node.node_id) + " references unknown node " +
                            std::to_string(input.index);
                    return TensorStatus::InvalidState;
                }
                ++indegree[node.node_id - 1];
                consumers[static_cast<std::size_t>(input.index) - 1].push_back(node.node_id);
            }
        }
    }

    // Kahn with smallest-ready-id-first (deterministic; no heap needed at
    // n <= 256 — a linear scan in id order is honest and simple).
    std::vector<bool> done(n, false);
    for (std::uint32_t emitted = 0; emitted < n; ++emitted) {
        NodeId next = kInvalidNodeId;
        for (std::uint32_t id = 1; id <= n; ++id) {
            if (!done[id - 1] && indegree[id - 1] == 0) {
                next = id;
                break;
            }
        }
        if (next == kInvalidNodeId) {
            error = "graph contains a cycle (nodes remaining: " + std::to_string(n - emitted) +
                    ")";
            return TensorStatus::InvalidState;
        }
        done[next - 1] = true;
        order.push_back(next);
        for (const NodeId consumer : consumers[next - 1]) {
            --indegree[consumer - 1];
        }
    }
    return TensorStatus::Ok;
}

TensorStatus validate_graph(const TensorGraph& graph, GraphValidation& validation,
                            std::string& error) {
    validation.node_outputs.assign(graph.node_count(), TensorOpOutputDesc{});

    // Topological order FIRST: it doubles as cycle detection and fixes the
    // processing order (deterministic).
    std::vector<NodeId> order;
    const TensorStatus order_status = graph_topological_order(graph, order, error);
    if (order_status != TensorStatus::Ok) return order_status;

    // Propagate shape/dtype through the graph with the SHARED op rules.
    for (const NodeId node_id : order) {
        const GraphNode& node = *graph.find(node_id);
        if (node.inputs.size() != tensor_op_arity(node.op)) {
            error = "node " + std::to_string(node_id) + " ('" + to_string(node.op) +
                    "') has " + std::to_string(node.inputs.size()) + " input bindings, expected " +
                    std::to_string(tensor_op_arity(node.op));
            return TensorStatus::InvalidState;
        }
        std::vector<TensorOpInputDesc> descs;
        descs.reserve(node.inputs.size());
        for (std::size_t slot = 0; slot < node.inputs.size(); ++slot) {
            // An unbound binding (default GraphInput at index 0) that does not
            // resolve is refused: binding to graph input slot 0 IS valid, so
            // "unbound" is only detectable as an unresolvable reference — the
            // two-phase API's default-constructed binding resolves like any
            // other and is validated uniformly.
            SourceDesc source;
            if (!resolve_node_input(graph, validation, node, slot, source, error)) {
                return TensorStatus::InvalidState;
            }
            TensorOpInputDesc desc;
            desc.shape = source.shape;
            desc.dtype = source.dtype;
            desc.contiguous = source.contiguous;
            descs.push_back(desc);
        }
        TensorOpOutputDesc output;
        const TensorStatus status = validate_op(node.op, node.params, descs, output, error);
        if (status != TensorStatus::Ok) {
            error = "node " + std::to_string(node_id) + " ('" + to_string(node.op) + "'): " +
                    error;
            return status;
        }
        validation.node_outputs[node_id - 1] = output;
    }

    // Outputs must reference known nodes (set_outputs enforces at write time;
    // re-checked here because validation is the pure entry point).
    for (const NodeId node_id : graph.outputs()) {
        if (graph.find(node_id) == nullptr) {
            error = "output references unknown node " + std::to_string(node_id);
            return TensorStatus::InvalidState;
        }
    }
    return TensorStatus::Ok;
}

}  // namespace vortyx::tensor
