// Tensor operations — vocabulary and rules (Phase 13) — implementation.

#include "tensor/op.hpp"

namespace vortyx::tensor {

const char* to_string(TensorOp op) {
    switch (op) {
        case TensorOp::MatMul: return "matmul";
        case TensorOp::GEMM: return "gemm";
        case TensorOp::Add: return "add";
        case TensorOp::Subtract: return "subtract";
        case TensorOp::Multiply: return "multiply";
        case TensorOp::Divide: return "divide";
        case TensorOp::ReduceSum: return "reduce_sum";
        case TensorOp::ReduceMean: return "reduce_mean";
        case TensorOp::Relu: return "relu";
        case TensorOp::Sigmoid: return "sigmoid";
        case TensorOp::Tanh: return "tanh";
        case TensorOp::Softmax: return "softmax";
        case TensorOp::Transpose: return "transpose";
        case TensorOp::Reshape: return "reshape";
    }
    return "unknown";
}

bool tensor_op_from_string(const std::string& label, TensorOp& out) {
    static const struct {
        TensorOp op;
        const char* label;
    } kTable[] = {
        {TensorOp::MatMul, "matmul"},     {TensorOp::GEMM, "gemm"},
        {TensorOp::Add, "add"},           {TensorOp::Subtract, "subtract"},
        {TensorOp::Multiply, "multiply"}, {TensorOp::Divide, "divide"},
        {TensorOp::ReduceSum, "reduce_sum"},   {TensorOp::ReduceMean, "reduce_mean"},
        {TensorOp::Relu, "relu"},         {TensorOp::Sigmoid, "sigmoid"},
        {TensorOp::Tanh, "tanh"},         {TensorOp::Softmax, "softmax"},
        {TensorOp::Transpose, "transpose"},    {TensorOp::Reshape, "reshape"},
    };
    for (const auto& entry : kTable) {
        if (label == entry.label) {
            out = entry.op;
            return true;
        }
    }
    return false;
}

std::vector<TensorOp> all_tensor_ops() {
    return {TensorOp::MatMul, TensorOp::GEMM,   TensorOp::Add,   TensorOp::Subtract,
            TensorOp::Multiply,   TensorOp::Divide,  TensorOp::ReduceSum,
            TensorOp::ReduceMean, TensorOp::Relu,    TensorOp::Sigmoid,
            TensorOp::Tanh,       TensorOp::Softmax, TensorOp::Transpose,
            TensorOp::Reshape};
}

std::size_t tensor_op_arity(TensorOp op) {
    switch (op) {
        case TensorOp::Add:
        case TensorOp::Subtract:
        case TensorOp::Multiply:
        case TensorOp::Divide:
        case TensorOp::MatMul:
            return 2;
        case TensorOp::GEMM:
            return 3;
        case TensorOp::ReduceSum:
        case TensorOp::ReduceMean:
        case TensorOp::Relu:
        case TensorOp::Sigmoid:
        case TensorOp::Tanh:
        case TensorOp::Softmax:
        case TensorOp::Transpose:
        case TensorOp::Reshape:
            return 1;
    }
    return 0;
}

bool tensor_op_supports_integer(TensorOp op) {
    switch (op) {
        case TensorOp::Add:
        case TensorOp::Subtract:
        case TensorOp::Multiply:
        case TensorOp::Divide:
            return true;  // elementwise arithmetic is defined on integers too
        default:
            return false;  // matmul/gemm/activations/reductions are float-only in P13
    }
}

namespace {

// Common checks for the elementwise broadcast ops (Add/Subtract/Multiply/
// Divide): dtypes must agree; the broadcast of the two shapes is the output
// shape. Broadcast output element count overflow is refused here (checked),
// before any allocation.
TensorStatus validate_elementwise(const std::vector<TensorOpInputDesc>& inputs,
                                  TensorOpOutputDesc& out, std::string& error) {
    if (inputs.size() != 2) {
        error = "elementwise ops take exactly two inputs";
        return TensorStatus::InvalidInput;
    }
    if (inputs[0].dtype != inputs[1].dtype) {
        error = std::string("elementwise inputs must share one dtype (") +
                to_string(inputs[0].dtype) + " vs " + to_string(inputs[1].dtype) + ")";
        return TensorStatus::DtypeMismatch;
    }
    TensorShape broadcast;
    const TensorStatus status =
        broadcast_shapes(inputs[0].shape, inputs[1].shape, broadcast, error);
    if (status != TensorStatus::Ok) return status;
    std::int64_t elements = 0;
    if (!broadcast.total_elements(elements)) {
        error = "broadcast output element count overflows int64";
        return TensorStatus::ResourceLimitExceeded;
    }
    out.shape = std::move(broadcast);
    out.dtype = inputs[0].dtype;
    out.contiguous = true;  // elementwise outputs are freshly allocated, contiguous
    return TensorStatus::Ok;
}

TensorStatus validate_reduce(TensorOp op, const TensorOpParams& params,
                             const std::vector<TensorOpInputDesc>& inputs,
                             TensorOpOutputDesc& out, std::string& error) {
    if (inputs.size() != 1) {
        error = "reductions take exactly one input";
        return TensorStatus::InvalidInput;
    }
    const TensorShape& shape = inputs[0].shape;
    if (shape.rank() == 0) {
        error = "reducing a rank-0 tensor is not defined in Phase 13";
        return TensorStatus::InvalidShape;
    }
    if (params.reduce_axis < 0 || params.reduce_axis >= static_cast<std::int64_t>(shape.rank())) {
        error = "reduce axis " + std::to_string(params.reduce_axis) + " out of range for rank " +
                std::to_string(shape.rank());
        return TensorStatus::InvalidShape;
    }
    TensorShape result;
    result.dims.reserve(shape.rank() - 1);
    for (std::size_t d = 0; d < shape.rank(); ++d) {
        if (static_cast<std::int64_t>(d) != params.reduce_axis) result.dims.push_back(shape.dims[d]);
    }
    out.shape = std::move(result);
    out.dtype = inputs[0].dtype;
    out.contiguous = true;
    (void)op;
    return TensorStatus::Ok;
}

TensorStatus validate_matmul(const std::vector<TensorOpInputDesc>& inputs,
                             TensorOpOutputDesc& out, std::string& error) {
    if (inputs.size() != 2) {
        error = "matmul takes exactly two inputs";
        return TensorStatus::InvalidInput;
    }
    if (inputs[0].dtype != inputs[1].dtype) {
        error = std::string("matmul inputs must share one dtype (") +
                to_string(inputs[0].dtype) + " vs " + to_string(inputs[1].dtype) + ")";
        return TensorStatus::DtypeMismatch;
    }
    if (data_type_is_integer(inputs[0].dtype)) {
        error = std::string("Phase 13 matmul is defined for floating dtypes only (got ") +
                to_string(inputs[0].dtype) + "); integer matmul is an explicit future extension";
        return TensorStatus::UnsupportedDtype;
    }
    const TensorShape& a = inputs[0].shape;
    const TensorShape& b = inputs[1].shape;
    if (a.rank() == 2 && b.rank() == 2) {
        if (a.dims[1] != b.dims[0]) {
            error = "matmul K dimension mismatch: A is " + a.describe() + ", B is " +
                    b.describe() + " (A.cols=" + std::to_string(a.dims[1]) + " != B.rows=" +
                    std::to_string(b.dims[0]) + ")";
            return TensorStatus::InvalidShape;
        }
        out.shape = TensorShape::make({a.dims[0], b.dims[1]});
    } else if (a.rank() == 3 && b.rank() == 3) {
        if (a.dims[0] != b.dims[0]) {
            error = "batched matmul batch mismatch: " + std::to_string(a.dims[0]) + " vs " +
                    std::to_string(b.dims[0]);
            return TensorStatus::InvalidShape;
        }
        if (a.dims[2] != b.dims[1]) {
            error = "batched matmul K dimension mismatch: A is " + a.describe() + ", B is " +
                    b.describe();
            return TensorStatus::InvalidShape;
        }
        out.shape = TensorShape::make({a.dims[0], a.dims[1], b.dims[2]});
    } else {
        error = "matmul supports rank-2 x rank-2 and rank-3 x rank-3 (batched) only; got " +
                a.describe() + " @ " + b.describe();
        return TensorStatus::InvalidShape;
    }
    out.dtype = inputs[0].dtype;
    out.contiguous = true;
    return TensorStatus::Ok;
}

TensorStatus validate_gemm(const std::vector<TensorOpInputDesc>& inputs,
                           TensorOpOutputDesc& out, std::string& error) {
    // A and B follow the matmul 2-D rules; C is the additive term.
    if (inputs.size() != 3) {
        error = "gemm takes exactly three inputs (A, B, C)";
        return TensorStatus::InvalidInput;
    }
    if (inputs[0].dtype != inputs[1].dtype || inputs[0].dtype != inputs[2].dtype) {
        error = "gemm inputs must share one dtype (" + std::string(to_string(inputs[0].dtype)) +
                ", " + to_string(inputs[1].dtype) + ", " + to_string(inputs[2].dtype) + ")";
        return TensorStatus::DtypeMismatch;
    }
    if (data_type_is_integer(inputs[0].dtype)) {
        error = std::string("Phase 13 gemm is defined for floating dtypes only (got ") +
                to_string(inputs[0].dtype) + ")";
        return TensorStatus::UnsupportedDtype;
    }
    std::vector<TensorOpInputDesc> ab = {inputs[0], inputs[1]};
    TensorOpOutputDesc ab_out;
    const TensorStatus status = validate_matmul(ab, ab_out, error);
    if (status != TensorStatus::Ok) return status;
    if (inputs[2].shape != ab_out.shape) {
        error = "gemm C must be " + ab_out.shape.describe() + " to match A@B, got " +
                inputs[2].shape.describe();
        return TensorStatus::InvalidShape;
    }
    out.shape = ab_out.shape;
    out.dtype = inputs[0].dtype;
    out.contiguous = true;
    return TensorStatus::Ok;
}

TensorStatus validate_softmax(const std::vector<TensorOpInputDesc>& inputs,
                              TensorOpOutputDesc& out, std::string& error) {
    if (inputs.size() != 1) {
        error = "softmax takes exactly one input";
        return TensorStatus::InvalidInput;
    }
    if (inputs[0].shape.rank() == 0) {
        error = "softmax over a rank-0 tensor is not defined";
        return TensorStatus::InvalidShape;
    }
    out.shape = inputs[0].shape;
    out.dtype = inputs[0].dtype;
    out.contiguous = true;
    return TensorStatus::Ok;
}

}  // namespace

TensorStatus validate_op(TensorOp op, const TensorOpParams& params,
                         const std::vector<TensorOpInputDesc>& inputs, TensorOpOutputDesc& out,
                         std::string& error) {
    // Arity check first (a shared, precise refusal).
    if (inputs.size() != tensor_op_arity(op)) {
        error = std::string("op '") + to_string(op) + "' takes " +
                std::to_string(tensor_op_arity(op)) + " input(s), got " +
                std::to_string(inputs.size());
        return TensorStatus::InvalidInput;
    }
    // Every input shape must at least be a valid shape (rank cap, no
    // negatives) before per-op rules run.
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const TensorStatus shape_status = inputs[i].shape.validate(error);
        if (shape_status != TensorStatus::Ok) {
            error = "input " + std::to_string(i) + ": " + error;
            return shape_status;
        }
        for (const TensorDim d : inputs[i].shape.dims) {
            if (d == 0) {
                error = "input " + std::to_string(i) +
                        " has a zero dimension: zero-element tensors never execute "
                        "(the project-wide zero-element rule)";
                return TensorStatus::InvalidShape;
            }
        }
    }

    switch (op) {
        case TensorOp::Add:
        case TensorOp::Subtract:
        case TensorOp::Multiply:
        case TensorOp::Divide:
            return validate_elementwise(inputs, out, error);
        case TensorOp::ReduceSum:
        case TensorOp::ReduceMean:
            return validate_reduce(op, params, inputs, out, error);
        case TensorOp::MatMul:
            return validate_matmul(inputs, out, error);
        case TensorOp::GEMM:
            return validate_gemm(inputs, out, error);
        case TensorOp::Relu:
        case TensorOp::Sigmoid:
        case TensorOp::Tanh:
        case TensorOp::Softmax: {
            if (data_type_is_integer(inputs[0].dtype)) {
                error = std::string("activation '") + to_string(op) +
                        "' is defined for floating dtypes only (got " +
                        to_string(inputs[0].dtype) + ")";
                return TensorStatus::UnsupportedDtype;
            }
            if (op == TensorOp::Softmax) return validate_softmax(inputs, out, error);
            out.shape = inputs[0].shape;
            out.dtype = inputs[0].dtype;
            out.contiguous = true;
            return TensorStatus::Ok;
        }
        case TensorOp::Transpose: {
            if (inputs[0].shape.rank() != 2) {
                error = "transpose is defined for rank-2 tensors only (got rank " +
                        std::to_string(inputs[0].shape.rank()) + ")";
                return TensorStatus::InvalidShape;
            }
            out.shape = TensorShape::make({inputs[0].shape.dims[1], inputs[0].shape.dims[0]});
            out.dtype = inputs[0].dtype;
            out.contiguous = true;
            return TensorStatus::Ok;
        }
        case TensorOp::Reshape: {
            if (!inputs[0].contiguous) {
                error = "reshape requires a row-major contiguous input (a strided tensor "
                        "must be materialized first — no hidden copy)";
                return TensorStatus::UnsupportedLayout;
            }
            const TensorStatus target_status = params.reshape_target.validate(error);
            if (target_status != TensorStatus::Ok) return target_status;
            std::int64_t source_elements = 0;
            std::int64_t target_elements = 0;
            if (!inputs[0].shape.total_elements(source_elements) ||
                !params.reshape_target.total_elements(target_elements)) {
                error = "reshape element count computation overflowed";
                return TensorStatus::ResourceLimitExceeded;
            }
            if (source_elements != target_elements) {
                error = "reshape must preserve the element count (" +
                        std::to_string(source_elements) + " -> " +
                        std::to_string(target_elements) + ")";
                return TensorStatus::InvalidShape;
            }
            out.shape = params.reshape_target;
            out.dtype = inputs[0].dtype;
            out.contiguous = true;
            return TensorStatus::Ok;
        }
    }
    error = "unknown tensor op";
    return TensorStatus::Internal;
}

}  // namespace vortyx::tensor
