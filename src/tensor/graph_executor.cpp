// GraphExecutor (Phase 13) — implementation.

#include "tensor/graph_executor.hpp"

#include <chrono>
#include <cstring>

namespace vortyx::tensor {

GraphExecutor::GraphExecutor(TensorExecutor& executor) : executor_(&executor) {}

namespace {

// Reads a whole tensor's storage into host bytes.
TensorStatus load_tensor_bytes(const Tensor& tensor, std::vector<std::byte>& out,
                               std::string& error) {
    out.assign(static_cast<std::size_t>(tensor.byte_size()), std::byte{0});
    return tensor.read_host(out.data(), out.size(), error);
}

}  // namespace

GraphExecutionResult GraphExecutor::execute(const TensorGraph& graph,
                                            const GraphExecutionPlan& plan,
                                            const std::vector<Tensor>& inputs) {
    GraphExecutionResult result;

    // --- binding validation (exact contracts, slot order) ----------------------
    if (inputs.size() != graph.inputs().size()) {
        result.status = TensorStatus::InvalidInput;
        result.error = "graph expects " + std::to_string(graph.inputs().size()) +
                       " input(s), got " + std::to_string(inputs.size());
        return result;
    }
    TensorPlacement target = TensorPlacement::host();
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const Tensor& binding = inputs[i];
        const GraphInputDesc& desc = graph.inputs()[i];
        if (!binding.valid()) {
            result.status = TensorStatus::NotInitialized;
            result.error = "input '" + desc.name + "' has no live storage";
            return result;
        }
        if (binding.shape() != desc.shape) {
            result.status = TensorStatus::InvalidShape;
            result.error = "input '" + desc.name + "' shape " + binding.shape().describe() +
                           " does not match the declared " + desc.shape.describe();
            return result;
        }
        if (binding.dtype() != desc.dtype) {
            result.status = TensorStatus::DtypeMismatch;
            result.error = "input '" + desc.name + "' dtype " +
                           std::string(to_string(binding.dtype())) +
                           " does not match the declared " + to_string(desc.dtype);
            return result;
        }
        const TensorStatus placement_status = binding.placement().validate(result.error);
        if (placement_status != TensorStatus::Ok) {
            result.status = placement_status;
            return result;
        }
        if (binding.placement().location == PlacementLocation::Device) {
            if (target.location == PlacementLocation::Device &&
                !target.same_place_as(binding.placement())) {
                result.status = TensorStatus::TransferUnsupported;
                result.error = "bindings span two device placements (" + target.describe() +
                               " vs " + binding.placement().describe() +
                               "); Phase 13 has no cross-device transfer";
                return result;
            }
            target = binding.placement();
        }
    }

    // --- slot allocation (through the SAME Phase 4 allocation path) -------------
    std::vector<Tensor> slot_tensors(plan.slots.size());
    const std::string storage_backend =
        target.backend.empty() ? std::string("cpu") : target.backend;
    vortyx::resource::ResourceManager& resources = executor_->resources();
    for (const PlanSlot& slot : plan.slots) {
        Tensor allocated;
        const TensorStatus status =
            Tensor::create(resources, slot.shape, slot.dtype, target, allocated, result.error,
                           storage_backend);
        if (status != TensorStatus::Ok) {
            result.status = status;
            result.error = "slot " + std::to_string(slot.slot_id) + " allocation failed: " +
                           result.error;
            return result;
        }
        slot_tensors[static_cast<std::size_t>(slot.slot_id)] = std::move(allocated);
        ++result.buffers_allocated;
        result.bytes_allocated += slot.byte_size;
    }

    // --- steps in plan order ------------------------------------------------------
    result.trace.reserve(plan.steps.size());
    std::vector<std::byte> source_bytes;
    for (const PlanStep& step : plan.steps) {
        GraphStepTrace trace;
        trace.node_id = step.node_id;
        trace.op = step.op;

        // Gather the step's inputs: graph bindings (materialized contiguous)
        // or slot tensors (already contiguous kernels outputs).
        std::vector<Tensor> step_inputs;
        step_inputs.reserve(step.inputs.size());
        bool gather_ok = true;
        for (const PlanStepInput& input : step.inputs) {
            switch (input.source) {
                case PlanStepInput::Source::GraphInput: {
                    if (input.index < 0 ||
                        static_cast<std::size_t>(input.index) >= inputs.size()) {
                        result.status = TensorStatus::InvalidState;
                        result.error = "step for node " + std::to_string(step.node_id) +
                                       " references graph input slot " +
                                       std::to_string(input.index);
                        return result;  // structural inconsistency: plan/graph mismatch
                    }
                    // Strided views are materialized by the single-op path;
                    // at graph level bindings must already be contiguous (the
                    // declared contracts are checked; a strided binding is
                    // materialized here explicitly).
                    if (!inputs[static_cast<std::size_t>(input.index)].is_contiguous()) {
                        Tensor materialized;
                        // Materialize through a scratch op path: copy via
                        // byte gather (a view of a view cannot exist here).
                        const Tensor& source = inputs[static_cast<std::size_t>(input.index)];
                        TensorStatus status = Tensor::create(
                            resources, source.shape(), source.dtype(), source.placement(),
                            materialized, result.error, storage_backend);
                        if (status != TensorStatus::Ok) {
                            result.status = status;
                            return result;
                        }
                        status = load_tensor_bytes(source, source_bytes, result.error);
                        if (status != TensorStatus::Ok) {
                            result.status = status;
                            return result;
                        }
                        const std::size_t width = data_type_byte_width(source.dtype());
                        std::vector<std::byte> gathered(source_bytes.size(), std::byte{0});
                        std::vector<std::int64_t> indices(source.shape().rank(), 0);
                        for (std::int64_t linear = 0; linear < source.elements(); ++linear) {
                            std::int64_t remaining = linear;
                            for (std::size_t d = source.shape().rank(); d-- > 0;) {
                                indices[d] = remaining % source.shape().dims[d];
                                remaining /= source.shape().dims[d];
                            }
                            std::int64_t offset = 0;
                            for (std::size_t d = 0; d < indices.size(); ++d) {
                                offset += indices[d] * source.layout().strides[d];
                            }
                            std::memcpy(gathered.data() +
                                            static_cast<std::ptrdiff_t>(linear) * width,
                                        source_bytes.data() +
                                            static_cast<std::ptrdiff_t>(offset) * width,
                                        width);
                        }
                        status = materialized.write_host(gathered.data(), gathered.size(),
                                                         result.error);
                        if (status != TensorStatus::Ok) {
                            result.status = status;
                            return result;
                        }
                        step_inputs.push_back(std::move(materialized));
                    } else {
                        step_inputs.push_back(inputs[static_cast<std::size_t>(input.index)]);
                    }
                    break;
                }
                case PlanStepInput::Source::Slot: {
                    if (input.index < 0 ||
                        static_cast<std::size_t>(input.index) >= slot_tensors.size() ||
                        !slot_tensors[static_cast<std::size_t>(input.index)].valid()) {
                        result.status = TensorStatus::InvalidState;
                        result.error = "step for node " + std::to_string(step.node_id) +
                                       " references plan slot " + std::to_string(input.index) +
                                       " which holds no live tensor";
                        return result;
                    }
                    step_inputs.push_back(slot_tensors[static_cast<std::size_t>(input.index)]);
                    break;
                }
            }
            if (!gather_ok) break;
        }

        // Execute through the single-op path (validation re-runs — the same
        // shared rule set) writing DIRECTLY into the plan's slot storage
        // (the memory plan owns allocation; no second buffer exists).
        Tensor& out_slot = slot_tensors[static_cast<std::size_t>(step.output_slot)];
        const std::chrono::steady_clock::time_point began =
            std::chrono::steady_clock::now();
        const TensorStatus status =
            executor_->execute_op_into(step.op, step.params, step_inputs, out_slot,
                                       result.error);
        const std::chrono::steady_clock::time_point ended = std::chrono::steady_clock::now();
        trace.timing_measured = true;
        trace.elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended - began).count();

        if (status != TensorStatus::Ok) {
            trace.status = status;
            trace.error = result.error;
            result.trace.push_back(std::move(trace));
            result.status = status;
            result.error = "node " + std::to_string(step.node_id) + " ('" + to_string(step.op) +
                           "') failed: " + result.error;
            return result;
        }

        trace.backend = "";  // dispatch detail; the single-op path resolved it
        trace.status = TensorStatus::Ok;
        result.trace.push_back(std::move(trace));
    }

    // --- outputs (share their pinned slot storage — no copies) -------------------
    result.outputs.reserve(graph.outputs().size());
    for (const NodeId node_id : graph.outputs()) {
        // Find the step that produced this node (its output slot).
        std::int32_t slot = -1;
        for (const PlanStep& step : plan.steps) {
            if (step.node_id == node_id) {
                slot = step.output_slot;
                break;
            }
        }
        if (slot < 0 || !slot_tensors[static_cast<std::size_t>(slot)].valid()) {
            result.status = TensorStatus::InvalidState;
            result.error = "output node " + std::to_string(node_id) +
                           " has no live slot (plan/graph mismatch)";
            return result;
        }
        result.outputs.push_back(slot_tensors[static_cast<std::size_t>(slot)]);
    }

    result.status = TensorStatus::Ok;
    return result;
}

GraphExecutionResult GraphExecutor::execute(const TensorGraph& graph,
                                            const std::vector<Tensor>& inputs) {
    // Convenience path: validate + plan + execute. The plan targets the
    // FIRST backend's capabilities (the deterministic dispatch head) — the
    // honest local target.
    GraphExecutionResult result;
    std::vector<ITensorBackend*> backends = executor_->backends();
    if (backends.empty()) {
        result.status = TensorStatus::NotInitialized;
        result.error = "no tensor backend is registered";
        return result;
    }

    GraphValidation validation;
    std::string error;
    const TensorStatus validation_status = validate_graph(graph, validation, error);
    if (validation_status != TensorStatus::Ok) {
        result.status = validation_status;
        result.error = error;
        return result;
    }

    GraphExecutionPlan plan;
    MemoryPlannerConfig config;  // reuse enabled (the default policy)
    const TensorStatus plan_status =
        make_execution_plan(graph, validation, backends.front()->capabilities(), config, plan,
                            error);
    if (plan_status != TensorStatus::Ok) {
        result.status = plan_status;
        result.error = error;
        return result;
    }
    return execute(graph, plan, inputs);
}

}  // namespace vortyx::tensor
