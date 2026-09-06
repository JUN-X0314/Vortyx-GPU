// TensorGraph -> WorkloadGraph adapter implementation (Phase 16) — see fabric_adapter.hpp.

#include "tensor/fabric_adapter.hpp"

#include <algorithm>

#include "fabric/workload.hpp"
#include "core/compute/task.hpp"
#include "tensor/dtype.hpp"
#include "tensor/op.hpp"

namespace vortyx::tensor {

using vortyx::platform::Status;

bool fabric_equivalent_operation(const GraphNode& node, const TensorOpOutputDesc& output,
                                 vortyx::compute::ComputeOp& out_op) {
    // The same rule the Phase 13 runtime adapter implements (backend.cpp):
    // int32 contiguous elementwise Add / Multiply route into the real
    // engine as VectorAdd / VectorMultiply. Nothing else has an executable
    // equivalent in the current operation vocabulary — and the adapter
    // invents none.
    if (output.dtype != DataType::INT32) return false;
    if (!output.contiguous) return false;
    switch (node.op) {
        case TensorOp::Add:
            out_op = vortyx::compute::ComputeOp::VectorAdd;
            return true;
        case TensorOp::Multiply:
            out_op = vortyx::compute::ComputeOp::VectorMultiply;
            return true;
        default:
            return false;
    }
}

Status tensor_graph_to_workload(const TensorGraph& graph, const GraphValidation& validation,
                                const vortyx::platform::UserId& owner_user_id,
                                std::uint32_t preferred_shard_count,
                                vortyx::fabric::WorkloadGraph& out, std::string& error) {
    out = vortyx::fabric::WorkloadGraph{};

    // Defensive re-validation (the caller validates first; the same
    // double-check pattern the Phase 4/10 dispatch path uses).
    GraphValidation revalidation;
    if (validate_graph(graph, revalidation, error) != TensorStatus::Ok) {
        error = "tensor graph is not valid: " + error;
        return Status::InvalidInput;
    }
    if (revalidation.node_outputs.size() != validation.node_outputs.size()) {
        error = "stale GraphValidation passed to the adapter (node count mismatch)";
        return Status::InvalidInput;
    }

    // Pass 1: create the nodes (insertion order = the graph's own id
    // space, so node ids line up 1:1).
    for (std::size_t i = 0; i < graph.nodes().size(); ++i) {
        const GraphNode& node = graph.nodes()[i];
        const TensorOpOutputDesc& output = revalidation.node_outputs[i];

        vortyx::compute::ComputeOp equivalent = vortyx::compute::ComputeOp::VectorAdd;
        if (!fabric_equivalent_operation(node, output, equivalent)) {
            error = std::string("tensor node ") + to_string(node.op) +
                    " (node id " + std::to_string(node.node_id) +
                    ") has no executable equivalent in the fabric's operation vocabulary — "
                    "the graph cannot be planned honestly";
            return Status::InvalidInput;
        }

        // The node's data-parallel domain: its validated output size
        // (checked arithmetic; a size the control plane could not accept
        // is refused here, never truncated).
        std::int64_t elements = 0;
        if (!output.shape.total_elements(elements)) {
            error = "tensor node id " + std::to_string(node.node_id) +
                    ": output element count overflows";
            return Status::InvalidInput;
        }
        if (elements <= 0) {
            error = "tensor node id " + std::to_string(node.node_id) +
                    ": output has no elements";
            return Status::InvalidInput;
        }

        vortyx::fabric::WorkloadDescriptor descriptor;
        descriptor.workload_id = "t" + std::to_string(node.node_id);
        descriptor.owner_user_id = owner_user_id;
        descriptor.operation = equivalent;
        descriptor.element_count = static_cast<std::uint64_t>(elements);
        descriptor.preferred_shard_count = preferred_shard_count;

        vortyx::fabric::WorkloadNodeId node_id = vortyx::fabric::kInvalidWorkloadNodeId;
        Status status = out.add_node(descriptor, {}, node_id, error);
        if (status != Status::Ok) {
            error = "tensor node id " + std::to_string(node.node_id) + ": " + error;
            return status;
        }
    }

    // Pass 2: bind the dependencies (data inputs that come from other
    // nodes). Graph-input-consuming inputs are the graph's own boundary —
    // not a fabric dependency.
    for (const GraphNode& node : graph.nodes()) {
        for (std::size_t slot = 0; slot < node.inputs.size(); ++slot) {
            const GraphNodeInput& input = node.inputs[slot];
            if (input.source != GraphNodeInput::Source::NodeOutput) continue;
            Status status = out.bind_dependency(
                node.node_id, static_cast<vortyx::fabric::WorkloadNodeId>(input.index), error);
            if (status != Status::Ok) {
                error = "tensor node id " + std::to_string(node.node_id) + ": " + error;
                return status;
            }
        }
    }
    return Status::Ok;
}

}  // namespace vortyx::tensor
