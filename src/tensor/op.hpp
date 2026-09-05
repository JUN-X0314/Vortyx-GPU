#pragma once

// Tensor operations — vocabulary, per-op rules and shape inference (Phase 13).
//
// The op set Phase 13 implements is EXPLICIT and CLOSED (the Phase 10
// ComputeOp precedent: an op is added together with its rules, never guessed
// from data):
//
//   MatMul      matrix multiplication; 2-D x 2-D and 3-D batched
//               (B x M x K) @ (B x K x N). K mismatch is an InvalidShape —
//               the op NEVER runs on mismatched inputs. Rank < 2 is refused.
//   GEMM        alpha * A(M,K) @ B(K,N) + beta * C(M,N); exactly three
//               inputs (A, B, C) with C shaped M x N and one shared dtype.
//               Floating dtypes only (int32/int8 -> UnsupportedDtype —
//               refused, never reinterpreted).
//   Add         elementwise add with NumPy-style broadcasting (see shape.hpp
//               for the exact implemented semantics; a separate
//               "BroadcastAdd" op label deliberately does not exist — Add
//               covers it, documented).
//   Subtract    elementwise subtract, same broadcasting rules as Add.
//   Multiply    elementwise multiply, same broadcasting rules.
//   Divide      elementwise divide, same broadcasting rules. Integer divide
//               by zero is a DEFINED domain error
//               (NumericalValidationFailure); float divide by zero follows
//               IEEE semantics (inf/nan), documented and tested.
//   ReduceSum   sum over ONE axis (param.axis). Output drops the axis.
//   ReduceMean  mean over ONE axis (param.axis), deterministic sequential
//               accumulation in memory order.
//   Relu        max(0, x), floating dtypes only.
//   Sigmoid     1 / (1 + exp(-x)), floating dtypes only.
//   Tanh        std::tanh, floating dtypes only.
//   Softmax     last-axis softmax with max-subtraction for numerical
//               stability (documented, tested with large magnitudes).
//   Transpose   2-D transpose (materialized output; the Tensor::transpose
//               VIEW is the zero-copy form).
//   Reshape     element-count-preserving reshape of a contiguous tensor.
//
// NOT IMPLEMENTED IN PHASE 13 (honest absence, no TODO-shaped fakes):
// convolution, pooling, normalization, embedding lookup, attention, any
// training/backward pass, any quantized kernel. The op vocabulary has no
// entry for them.
//
// Every op carries:
//   - a stable lowercase label (the observability/contract vocabulary),
//   - an ARITY (1 or 2 inputs),
//   - a pure validation + shape-inference function (validate_op below):
//     given the input descriptors it returns the output descriptor or the
//     precise failing TensorStatus. The graph planner and the executor use
//     the SAME function — the rules cannot drift between planning and
//     execution.

#include <cstdint>
#include <string>
#include <vector>

#include "tensor/dtype.hpp"
#include "tensor/shape.hpp"
#include "tensor/status.hpp"

namespace vortyx::tensor {

enum class TensorOp : std::uint8_t {
    MatMul = 0,
    GEMM,
    Add,
    Subtract,
    Multiply,
    Divide,
    ReduceSum,
    ReduceMean,
    Relu,
    Sigmoid,
    Tanh,
    Softmax,
    Transpose,
    Reshape,
};

// Stable lowercase labels ("matmul", "gemm", "add", ...).
const char* to_string(TensorOp op);

// Parses a stable label. False for anything else.
bool tensor_op_from_string(const std::string& label, TensorOp& out);

// All ops in enum order (observability / capability building).
std::vector<TensorOp> all_tensor_ops();

// Input count for each op (1 or 2). Total function.
std::size_t tensor_op_arity(TensorOp op);

// Ops whose kernels accept INTEGER dtypes (the elementwise arithmetic set).
// Pure.
bool tensor_op_supports_integer(TensorOp op);

// ---------------------------------------------------------------------------
// Op parameters (a plain value struct; ops read only their own fields)
// ---------------------------------------------------------------------------

struct TensorOpParams {
    // GEMM.
    float gemm_alpha = 1.0f;
    float gemm_beta = 0.0f;

    // ReduceSum / ReduceMean: the (single) axis to reduce.
    std::int64_t reduce_axis = 0;

    // Reshape: the target shape (element count must match).
    TensorShape reshape_target;
};

// ---------------------------------------------------------------------------
// Input / output descriptors (shape inference vocabulary)
// ---------------------------------------------------------------------------

struct TensorOpInputDesc {
    TensorShape shape;
    DataType dtype = DataType::FP32;
    bool contiguous = true;  // row-major contiguous?
};

struct TensorOpOutputDesc {
    TensorShape shape;
    DataType dtype = DataType::FP32;
    bool contiguous = true;
};

// Validates 'inputs' for 'op' under 'params' and infers the output
// descriptor. PURE — used by the executor, the graph planner and the tests
// (one rule set, three consumers). Returns TensorStatus::Ok, or the precise
// failure:
//   - wrong input count / null-ish empty shape        -> InvalidInput
//   - shape rule violation (e.g. MatMul K mismatch)   -> InvalidShape
//   - inputs disagree on dtype                        -> DtypeMismatch
//   - op rejects the dtype (e.g. GEMM on int32)       -> UnsupportedDtype
//   - op rejects the layout (e.g. reshape on strided) -> UnsupportedLayout
//   - reduce axis out of range                        -> InvalidShape
TensorStatus validate_op(TensorOp op, const TensorOpParams& params,
                         const std::vector<TensorOpInputDesc>& inputs,
                         TensorOpOutputDesc& out, std::string& error);

}  // namespace vortyx::tensor
