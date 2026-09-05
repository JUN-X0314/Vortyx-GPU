// TensorExecutor (Phase 13) — implementation.

#include "tensor/executor.hpp"

#include <algorithm>
#include <cstring>

namespace vortyx::tensor {

TensorStatus TensorExecutor::create(const Deps& deps, std::unique_ptr<TensorExecutor>& out,
                                    std::string& error) {
    if (deps.resources == nullptr) {
        error = "tensor executor requires a ResourceManager (the Phase 4 memory system "
                "is the only allocation path)";
        return TensorStatus::InvalidInput;
    }
    if (deps.runtime != nullptr && !deps.runtime->is_initialized()) {
        error = "the runtime adapter requires an initialized compute Runtime";
        return TensorStatus::NotInitialized;
    }

    std::unique_ptr<TensorExecutor> executor(new TensorExecutor());
    executor->resources_ = deps.resources;

    // Dispatch order: external backends first (explicit extension point),
    // then the runtime adapter (when provided), then the reference backend.
    for (ITensorBackend* backend : deps.external_backends) {
        if (backend == nullptr) {
            error = "null external backend";
            return TensorStatus::InvalidInput;
        }
        executor->dispatch_order_.push_back(backend);
    }
    if (deps.runtime != nullptr) {
        executor->owned_backends_.push_back(
            std::make_unique<RuntimeElementwiseTensorBackend>(*deps.runtime));
    }
    if (deps.include_reference) {
        executor->owned_backends_.push_back(std::make_unique<CpuReferenceTensorBackend>());
    }
    if (executor->owned_backends_.empty() && executor->dispatch_order_.empty()) {
        error = "tensor executor needs at least one backend";
        return TensorStatus::InvalidInput;
    }
    for (auto& owned : executor->owned_backends_) {
        executor->dispatch_order_.push_back(owned.get());
    }

    out = std::move(executor);
    return TensorStatus::Ok;
}

TensorExecutor::~TensorExecutor() = default;

std::vector<ITensorBackend*> TensorExecutor::backends() const { return dispatch_order_; }

TensorStatus TensorExecutor::materialize_contiguous(const Tensor& tensor, Tensor& out,
                                                    std::string& error) {
    if (tensor.is_contiguous()) {
        out = tensor;
        return TensorStatus::Ok;
    }
    // Explicit, documented copy of a strided view into fresh contiguous
    // storage (through the Phase 4 resource system, like every allocation).
    Tensor copy;
    TensorStatus status =
        Tensor::create(*resources_, tensor.shape(), tensor.dtype(), tensor.placement(), copy,
                       error);
    if (status != TensorStatus::Ok) return status;

    std::vector<std::byte> source(static_cast<std::size_t>(tensor.byte_size()), std::byte{0});
    status = tensor.read_host(source.data(), source.size(), error);
    if (status != TensorStatus::Ok) return status;

    const std::size_t width =
        data_type_byte_width(tensor.dtype()) == 0 ? 1 : data_type_byte_width(tensor.dtype());
    const std::int64_t elements = tensor.elements();
    std::vector<std::byte> gathered(static_cast<std::size_t>(tensor.byte_size()), std::byte{0});
    std::vector<std::int64_t> indices(tensor.shape().rank(), 0);
    for (std::int64_t linear = 0; linear < elements; ++linear) {
        // Decode the logical multi-index and take the strided offset.
        std::int64_t remaining = linear;
        for (std::size_t d = tensor.shape().rank(); d-- > 0;) {
            indices[d] = remaining % tensor.shape().dims[d];
            remaining /= tensor.shape().dims[d];
        }
        std::int64_t offset = 0;
        for (std::size_t d = 0; d < indices.size(); ++d) {
            offset += indices[d] * tensor.layout().strides[d];
        }
        std::memcpy(gathered.data() + static_cast<std::ptrdiff_t>(linear) * width,
                    source.data() + static_cast<std::ptrdiff_t>(offset) * width, width);
    }
    status = copy.write_host(gathered.data(), gathered.size(), error);
    if (status != TensorStatus::Ok) return status;
    out = std::move(copy);
    return TensorStatus::Ok;
}

TensorStatus TensorExecutor::execute_op(TensorOp op, const TensorOpParams& params,
                                        const std::vector<Tensor>& inputs, Tensor& out,
                                        std::string& error) {
    return execute_internal(op, params, inputs, nullptr, out, error);
}

TensorStatus TensorExecutor::execute_op_into(TensorOp op, const TensorOpParams& params,
                                             const std::vector<Tensor>& inputs,
                                             Tensor& provided, std::string& error) {
    Tensor unused;
    return execute_internal(op, params, inputs, &provided, unused, error);
}

TensorStatus TensorExecutor::execute_internal(TensorOp op, const TensorOpParams& params,
                                              const std::vector<Tensor>& inputs,
                                              Tensor* provided, Tensor& out,
                                              std::string& error) {
    if (inputs.empty()) {
        error = "tensor execution requires at least one input";
        return TensorStatus::InvalidInput;
    }
    for (const Tensor& tensor : inputs) {
        if (!tensor.valid()) {
            error = "tensor execution requires live input tensors";
            return TensorStatus::NotInitialized;
        }
        const TensorStatus placement_status = tensor.placement().validate(error);
        if (placement_status != TensorStatus::Ok) return placement_status;
    }

    // --- placement rules (documented; no cross-device movement exists) ----
    TensorPlacement target = TensorPlacement::host();
    for (const Tensor& tensor : inputs) {
        const TensorPlacement& placement = tensor.placement();
        if (placement.location == PlacementLocation::Device) {
            if (target.location == PlacementLocation::Device &&
                !target.same_place_as(placement)) {
                error = "inputs span two device placements (" + target.describe() + " vs " +
                        placement.describe() + "); Phase 13 has no cross-device tensor "
                                             "transfer and refuses the request";
                return TensorStatus::TransferUnsupported;
            }
            target = placement;
        }
    }

    // --- validation + shape inference (the shared rule set) ----------------
    std::vector<TensorOpInputDesc> descs;
    descs.reserve(inputs.size());
    for (const Tensor& tensor : inputs) {
        TensorOpInputDesc desc;
        desc.shape = tensor.shape();
        desc.dtype = tensor.dtype();
        desc.contiguous = tensor.is_contiguous();
        descs.push_back(desc);
    }
    TensorOpOutputDesc output_desc;
    const TensorStatus validation = validate_op(op, params, descs, output_desc, error);
    if (validation != TensorStatus::Ok) return validation;

    // --- dispatch decision (capability-based, deterministic) ----------------
    const bool broadcast = op == TensorOp::Add || op == TensorOp::Subtract ||
                           op == TensorOp::Multiply || op == TensorOp::Divide;
    TensorRequirements requirements;
    requirements.required_ops = {op};
    requirements.required_dtypes = {output_desc.dtype};
    requirements.max_input_rank = 0;
    for (const TensorOpInputDesc& desc : descs) {
        requirements.max_input_rank = std::max(requirements.max_input_rank, desc.shape.rank());
    }
    std::int64_t worst_bytes = 0;
    for (const Tensor& tensor : inputs) {
        worst_bytes = std::max(worst_bytes, tensor.byte_size());
    }
    requirements.max_tensor_bytes = worst_bytes;

    std::vector<ITensorBackend*> eligible;
    bool needs_broadcast = false;
    if (broadcast) {
        for (const TensorOpInputDesc& desc : descs) {
            if (desc.shape != output_desc.shape) {
                needs_broadcast = true;
                break;
            }
        }
    }
    for (ITensorBackend* backend : dispatch_order_) {
        if (!requirements.satisfied_by(backend->capabilities())) continue;
        if (needs_broadcast && !backend->capabilities().supports_broadcast) {
            // The runtime adapter cannot execute a broadcast elementwise
            // case; the reference backend can. Capability match, not
            // fallback.
            continue;
        }
        eligible.push_back(backend);
    }
    if (eligible.empty()) {
        // With the built-in reference backend this is unreachable for every
        // request validate_op lets through (it claims the full op x dtype
        // surface); reaching it means the caller injected ONLY restricted
        // external backends. Report the full capability table.
        error = describe_dispatch_failure(dispatch_order_, op, output_desc.dtype);
        if (needs_broadcast) {
            error = "no backend with broadcast support for this request; " + error;
        }
        return TensorStatus::UnsupportedOperation;
    }
    ITensorBackend* backend = eligible.front();

    // --- materialize strided inputs (explicit, documented) -------------------
    std::vector<Tensor> contiguous_inputs;
    contiguous_inputs.reserve(inputs.size());
    for (const Tensor& tensor : inputs) {
        Tensor materialized;
        const TensorStatus status = materialize_contiguous(tensor, materialized, error);
        if (status != TensorStatus::Ok) return status;
        contiguous_inputs.push_back(std::move(materialized));
    }

    // --- output allocation (or the caller-provided slot tensor) ---------------
    Tensor output;
    const std::string storage_backend =
        target.backend.empty() ? std::string("cpu") : target.backend;
    if (provided != nullptr) {
        // The memory plan owns allocation: verify the provided tensor is
        // EXACTLY the inferred output (shape/dtype/contiguity) and use it.
        if (!provided->valid()) {
            error = "provided output tensor has no live storage";
            return TensorStatus::NotInitialized;
        }
        if (provided->shape() != output_desc.shape || provided->dtype() != output_desc.dtype ||
            !provided->is_contiguous()) {
            error = "provided output tensor does not match the inferred output (" +
                    provided->shape().describe() + " / " + to_string(provided->dtype()) +
                    ")";
            return TensorStatus::InvalidShape;
        }
        output = *provided;
    } else {
        const TensorStatus status =
            Tensor::create(*resources_, output_desc.shape, output_desc.dtype, target, output,
                           error, storage_backend);
        if (status != TensorStatus::Ok) return status;
    }

    // --- execute --------------------------------------------------------------
    TensorOpRequest request;
    request.op = op;
    request.params = params;
    request.inputs = std::move(contiguous_inputs);
    request.output = output;

    const TensorStatus status = backend->execute(request, error);
    if (status != TensorStatus::Ok) return status;

    out = std::move(request.output);
    return TensorStatus::Ok;
}

}  // namespace vortyx::tensor
