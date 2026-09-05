// Tensor backends (Phase 13) — implementation.
//
// Reference kernels: deterministic, host-memory, defined semantics for every
// (op, dtype) pair the capabilities list. FP16/BF16 run the documented
// promote-compute-round path (exact fp32 math, round-to-nearest-even back).
// MatMul/GEMM accumulate in fp32 with per-output-element k-ascending order —
// the blocked iteration structure never changes an accumulation sequence, so
// results are tile-independent and bit-deterministic.

#include "tensor/backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/compute/runtime.hpp"

namespace vortyx::tensor {

// ---------------------------------------------------------------------------
// FP16 / BF16 <-> FP32 conversion (round-to-nearest-even, deterministic)
// ---------------------------------------------------------------------------

namespace {

float fp16_to_fp32(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h >> 15) & 0x1u;
    const std::uint32_t exp = static_cast<std::uint32_t>(h >> 10) & 0x1Fu;
    const std::uint32_t frac = static_cast<std::uint32_t>(h) & 0x3FFu;
    std::uint32_t bits;
    if (exp == 0) {
        if (frac == 0) {
            bits = sign << 31;  // zero (including -0)
        } else {
            // Subnormal half: normalize.
            std::uint32_t e = 0;
            std::uint32_t f = frac;
            while ((f & 0x400u) == 0) {
                f <<= 1;
                ++e;
            }
            f &= 0x3FFu;
            const std::uint32_t fexp = 127 - 15 - e + 1;  // bias 127, half exp -15
            bits = (sign << 31) | (fexp << 23) | (f << 13);
        }
    } else if (exp == 31) {
        bits = (sign << 31) | 0x7F800000u | (frac << 13);  // inf / nan
    } else {
        const std::uint32_t fexp = exp - 15 + 127;
        bits = (sign << 31) | (fexp << 23) | (frac << 13);
    }
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

std::uint16_t fp32_to_fp16(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::int32_t exp = static_cast<std::int32_t>((bits >> 23) & 0xFFu) - 127;
    const std::uint32_t frac = bits & 0x7FFFFFu;
    if (((bits >> 23) & 0xFFu) == 0xFF) {
        // Inf / NaN.
        return static_cast<std::uint16_t>(sign | 0x7C00u | (frac != 0 ? 0x200u : 0u));
    }
    // Round-to-nearest-even on the 13 dropped bits.
    if (exp > 15) return static_cast<std::uint16_t>(sign | 0x7C00u);   // overflow -> inf
    if (exp >= -14) {
        std::uint32_t half = static_cast<std::uint32_t>(exp + 15) << 10;
        const std::uint32_t mant = frac >> 13;
        const std::uint32_t rem = frac & 0x1FFFu;
        const std::uint32_t half_mant = mant + ((rem > 0x1000u) || (rem == 0x1000u && (mant & 1u)));
        if (half_mant > 0x3FFu) {
            half = (static_cast<std::uint32_t>(exp + 15 + 1) << 10);  // mantissa overflow
        } else {
            half |= half_mant;
        }
        return static_cast<std::uint16_t>(sign | half);
    }
    // Subnormal half range (exp in [-24, -15]) or underflow to zero.
    if (exp >= -25) {
        const std::uint32_t shift = static_cast<std::uint32_t>(-(exp + 25));  // 0..10
        const std::uint32_t mant_with_one = frac | 0x800000u;
        std::uint32_t sub = mant_with_one >> (13 + shift);
        const std::uint32_t rem = mant_with_one & ((1u << (13 + shift)) - 1u);
        const std::uint32_t halfway = 1u << (12 + shift);
        if (rem > halfway || (rem == halfway && (sub & 1u))) ++sub;
        return static_cast<std::uint16_t>(sign | sub);
    }
    return static_cast<std::uint16_t>(sign);  // underflow -> signed zero
}

float bf16_to_fp32(std::uint16_t b) {
    const std::uint32_t bits = static_cast<std::uint32_t>(b) << 16;
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

std::uint16_t fp32_to_bf16(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    // Round-to-nearest-even on the low 16 bits.
    const std::uint32_t upper = bits >> 16;
    const std::uint32_t lower = bits & 0xFFFFu;
    std::uint32_t rounded = upper;
    if (lower > 0x8000u || (lower == 0x8000u && (upper & 1u) != 0u)) {
        // Avoid carrying into the exponent's NaN/Inf pattern incorrectly:
        // plain increment is correct for every finite value and for inf.
        ++rounded;
    }
    return static_cast<std::uint16_t>(rounded);
}

// One scalar converted from its stored dtype to fp32 (the compute domain).
float load_as_fp32(const void* base, std::int64_t index, DataType dtype) {
    switch (dtype) {
        case DataType::FP32: {
            float v;
            std::memcpy(&v, static_cast<const std::byte*>(base) +
                                static_cast<std::ptrdiff_t>(index) * 4, sizeof(v));
            return v;
        }
        case DataType::FP16: {
            std::uint16_t v;
            std::memcpy(&v, static_cast<const std::byte*>(base) +
                                static_cast<std::ptrdiff_t>(index) * 2, sizeof(v));
            return fp16_to_fp32(v);
        }
        case DataType::BF16: {
            std::uint16_t v;
            std::memcpy(&v, static_cast<const std::byte*>(base) +
                                static_cast<std::ptrdiff_t>(index) * 2, sizeof(v));
            return bf16_to_fp32(v);
        }
        default:
            return 0.0f;  // integer dtypes never take this path (callers guard)
    }
}

// One fp32 scalar stored back in the tensor's dtype (the round step of
// promote-compute-round; fp32 stores are exact).
void store_from_fp32(void* base, std::int64_t index, DataType dtype, float value) {
    switch (dtype) {
        case DataType::FP32: {
            const float v = value;
            std::memcpy(static_cast<std::byte*>(base) + static_cast<std::ptrdiff_t>(index) * 4,
                        &v, sizeof(v));
            return;
        }
        case DataType::FP16: {
            const std::uint16_t v = fp32_to_fp16(value);
            std::memcpy(static_cast<std::byte*>(base) + static_cast<std::ptrdiff_t>(index) * 2,
                        &v, sizeof(v));
            return;
        }
        case DataType::BF16: {
            const std::uint16_t v = fp32_to_bf16(value);
            std::memcpy(static_cast<std::byte*>(base) + static_cast<std::ptrdiff_t>(index) * 2,
                        &v, sizeof(v));
            return;
        }
        default:
            return;  // integer dtypes never take this path (callers guard)
    }
}

// Generic multi-index walk over a (possibly strided/broadcast) layout.
struct LayoutView {
    TensorShape shape;
    TensorLayout layout;
    DataType dtype = DataType::FP32;
    const void* bytes = nullptr;

    // Aligned element offset for an OUTPUT multi-index: dimensions align from
    // the trailing side (the broadcasting convention); a dimension the view
    // does not have contributes nothing; a stretched (size-1) dimension has
    // stride 0. This is what makes rank-expanding broadcast reads correct.
    std::int64_t aligned_offset(const std::vector<std::int64_t>& out_indices,
                                std::size_t out_rank) const {
        std::int64_t offset = 0;
        const std::size_t rank = shape.rank();
        for (std::size_t k = 0; k < rank; ++k) {
            const std::size_t out_dim = out_rank - 1 - k;
            const std::size_t my_dim = rank - 1 - k;
            if (shape.dims[my_dim] == 1) continue;  // broadcast stretch
            offset += out_indices[out_dim] * layout.strides[my_dim];
        }
        return offset;
    }
};

// Fills 'indices' with the multi-index of linear output position 'linear'
// over 'shape' (row-major). Returns false when linear is out of range.
bool decode_linear(const TensorShape& shape, std::int64_t linear,
                   std::vector<std::int64_t>& indices) {
    std::int64_t remaining = linear;
    for (std::size_t d = shape.rank(); d-- > 0;) {
        if (shape.dims[d] <= 0) return false;
        indices[d] = remaining % shape.dims[d];
        remaining /= shape.dims[d];
    }
    return remaining == 0;
}

std::int64_t offset_of(const TensorLayout& layout, const std::vector<std::int64_t>& indices) {
    std::int64_t offset = 0;
    for (std::size_t d = 0; d < indices.size(); ++d) {
        offset += indices[d] * layout.strides[d];
    }
    return offset;
}

}  // namespace

// ---------------------------------------------------------------------------
// CpuReferenceTensorBackend
// ---------------------------------------------------------------------------

CpuReferenceTensorBackend::CpuReferenceTensorBackend() {
    capabilities_.supported_ops = {
        TensorOp::MatMul, TensorOp::GEMM, TensorOp::Add, TensorOp::Subtract,
        TensorOp::Multiply, TensorOp::Divide, TensorOp::ReduceSum, TensorOp::ReduceMean,
        TensorOp::Relu, TensorOp::Sigmoid, TensorOp::Tanh, TensorOp::Softmax,
        TensorOp::Transpose, TensorOp::Reshape};
    capabilities_.supported_dtypes = all_data_types();
    capabilities_.supports_strided_input = false;  // executor materializes first
    capabilities_.supports_broadcast = true;
    capabilities_.max_rank = kMaxTensorRank;
    capabilities_.max_elements = kMaxTensorBytes;  // coarse bound (1-byte elements)
    capabilities_.max_bytes = kMaxTensorBytes;
    capabilities_.preferred_tile_m = 16;
    capabilities_.preferred_tile_n = 16;
    capabilities_.preferred_tile_k = 16;
    capabilities_.matrix_acceleration = MatrixAcceleration::NotClaimed;
}

namespace {

// Reads a whole tensor into a host byte buffer (via its storage).
TensorStatus gather_bytes(const Tensor& tensor, std::vector<std::byte>& out, std::string& error) {
    out.assign(static_cast<std::size_t>(tensor.byte_size()), std::byte{0});
    return tensor.read_host(out.data(), out.size(), error);
}

// The elementwise kernels (Add/Subtract/Multiply/Divide) with full
// broadcasting support; integer dtypes use defined modular two's-complement
// arithmetic (documented), float dtypes use IEEE fp32 with
// promote-compute-round for fp16/bf16.
TensorStatus execute_elementwise(TensorOp op, const std::vector<Tensor>& inputs,
                                 Tensor& output, std::string& error) {
    std::vector<std::vector<std::byte>> input_bytes(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const TensorStatus status = gather_bytes(inputs[i], input_bytes[i], error);
        if (status != TensorStatus::Ok) return status;
    }
    std::vector<std::byte> out_bytes(static_cast<std::size_t>(output.byte_size()),
                                     std::byte{0});

    const TensorShape& out_shape = output.shape();
    const std::int64_t out_elements = output.elements();
    const DataType dtype = output.dtype();

    std::vector<LayoutView> views(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        views[i].shape = inputs[i].shape();
        views[i].layout = inputs[i].layout();
        views[i].dtype = inputs[i].dtype();
        views[i].bytes = input_bytes[i].data();
    }

    std::vector<std::int64_t> indices(out_shape.rank(), 0);
    for (std::int64_t linear = 0; linear < out_elements; ++linear) {
        if (!decode_linear(out_shape, linear, indices)) {
            error = "elementwise index decode failed (internal invariant)";
            return TensorStatus::Internal;
        }
        const std::int64_t oi = offset_of(output.layout(), indices);

        if (data_type_is_integer(dtype)) {
            // Integer path: INT32/INT8, defined modular arithmetic.
            if (dtype == DataType::INT32) {
                std::int32_t lhs = 0;
                std::int32_t rhs = 0;
                std::memcpy(&lhs,
                            input_bytes[0].data() +
                                static_cast<std::ptrdiff_t>(views[0].aligned_offset(
                                    indices, out_shape.rank())) *
                                    4,
                            sizeof(lhs));
                std::memcpy(&rhs,
                            input_bytes[1].data() +
                                static_cast<std::ptrdiff_t>(views[1].aligned_offset(
                                    indices, out_shape.rank())) *
                                    4,
                            sizeof(rhs));
                std::int32_t result = 0;
                if (op == TensorOp::Add) {
                    result = static_cast<std::int32_t>(
                        static_cast<std::uint32_t>(lhs) + static_cast<std::uint32_t>(rhs));
                } else if (op == TensorOp::Subtract) {
                    result = static_cast<std::int32_t>(
                        static_cast<std::uint32_t>(lhs) - static_cast<std::uint32_t>(rhs));
                } else if (op == TensorOp::Multiply) {
                    result = static_cast<std::int32_t>(
                        static_cast<std::uint32_t>(lhs) * static_cast<std::uint32_t>(rhs));
                } else {  // Divide
                    if (rhs == 0) {
                        error = "integer division by zero (defined domain error)";
                        return TensorStatus::NumericalValidationFailure;
                    }
                    result = lhs / rhs;  // C++ truncation, documented
                }
                std::memcpy(out_bytes.data() + static_cast<std::ptrdiff_t>(oi) * 4, &result,
                            sizeof(result));
            } else {  // INT8
                std::int8_t lhs = 0;
                std::int8_t rhs = 0;
                std::memcpy(&lhs,
                            input_bytes[0].data() +
                                static_cast<std::ptrdiff_t>(views[0].aligned_offset(
                                    indices, out_shape.rank())),
                            sizeof(lhs));
                std::memcpy(&rhs,
                            input_bytes[1].data() +
                                static_cast<std::ptrdiff_t>(views[1].aligned_offset(
                                    indices, out_shape.rank())),
                            sizeof(rhs));
                std::int8_t result = 0;
                if (op == TensorOp::Add) {
                    result = static_cast<std::int8_t>(
                        static_cast<std::uint8_t>(lhs) + static_cast<std::uint8_t>(rhs));
                } else if (op == TensorOp::Subtract) {
                    result = static_cast<std::int8_t>(
                        static_cast<std::uint8_t>(lhs) - static_cast<std::uint8_t>(rhs));
                } else if (op == TensorOp::Multiply) {
                    result = static_cast<std::int8_t>(
                        static_cast<std::uint8_t>(lhs) * static_cast<std::uint8_t>(rhs));
                } else {  // Divide
                    if (rhs == 0) {
                        error = "integer division by zero (defined domain error)";
                        return TensorStatus::NumericalValidationFailure;
                    }
                    result = static_cast<std::int8_t>(lhs / rhs);
                }
                std::memcpy(out_bytes.data() + static_cast<std::ptrdiff_t>(oi), &result,
                            sizeof(result));
            }
        } else {
            // Float path: promote-compute-round.
            const float a = load_as_fp32(views[0].bytes,
                                         views[0].aligned_offset(indices, out_shape.rank()),
                                         views[0].dtype);
            const float b = load_as_fp32(views[1].bytes,
                                         views[1].aligned_offset(indices, out_shape.rank()),
                                         views[1].dtype);
            float result = 0.0f;
            switch (op) {
                case TensorOp::Add: result = a + b; break;
                case TensorOp::Subtract: result = a - b; break;
                case TensorOp::Multiply: result = a * b; break;
                case TensorOp::Divide: result = a / b; break;  // IEEE (inf/nan defined)
                default:
                    error = "non-arithmetic op in elementwise kernel (internal)";
                    return TensorStatus::Internal;
            }
            store_from_fp32(out_bytes.data(), oi, dtype, result);
        }
    }

    return output.write_host(out_bytes.data(), out_bytes.size(), error);
}

// Matrix kernels: MatMul (2-D / batched 3-D) and GEMM, fp32 accumulation,
// k-ascending per output element, promote-compute-round for fp16/bf16.
TensorStatus execute_matmul(const std::vector<Tensor>& inputs, Tensor& output,
                            std::string& error) {
    std::vector<std::vector<std::byte>> input_bytes(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const TensorStatus status = gather_bytes(inputs[i], input_bytes[i], error);
        if (status != TensorStatus::Ok) return status;
    }
    std::vector<std::byte> out_bytes(static_cast<std::size_t>(output.byte_size()),
                                     std::byte{0});

    const DataType dtype = inputs[0].dtype();
    const TensorShape& a_shape = inputs[0].shape();
    const TensorShape& b_shape = inputs[1].shape();
    const bool batched = a_shape.rank() == 3;

    const std::int64_t batches = batched ? a_shape.dims[0] : 1;
    const std::int64_t m = batched ? a_shape.dims[1] : a_shape.dims[0];
    const std::int64_t k = batched ? a_shape.dims[2] : a_shape.dims[1];
    const std::int64_t n = batched ? b_shape.dims[2] : b_shape.dims[1];

    // Views into the batched/2-D tensors (contiguity was enforced at
    // validation; strides still route every access). Index vectors are sized
    // to the ACTUAL rank of each tensor.
    const TensorLayout& a_layout = inputs[0].layout();
    const TensorLayout& b_layout = inputs[1].layout();

    for (std::int64_t batch = 0; batch < batches; ++batch) {
        for (std::int64_t i = 0; i < m; ++i) {
            for (std::int64_t j = 0; j < n; ++j) {
                float acc = 0.0f;
                for (std::int64_t kk = 0; kk < k; ++kk) {
                    std::int64_t a_off = 0;
                    std::int64_t b_off = 0;
                    if (batched) {
                        a_off = offset_of(a_layout, {batch, i, kk});
                        b_off = offset_of(b_layout, {batch, kk, j});
                    } else {
                        a_off = offset_of(a_layout, {i, kk});
                        b_off = offset_of(b_layout, {kk, j});
                    }
                    const float av = load_as_fp32(input_bytes[0].data(), a_off, dtype);
                    const float bv = load_as_fp32(input_bytes[1].data(), b_off, dtype);
                    acc += av * bv;
                }
                const std::int64_t out_off =
                    batched ? (batch * m * n + i * n + j) : (i * n + j);
                store_from_fp32(out_bytes.data(), out_off, dtype, acc);
            }
        }
    }
    return output.write_host(out_bytes.data(), out_bytes.size(), error);
}

TensorStatus execute_gemm(const TensorOpParams& params, const std::vector<Tensor>& inputs,
                          Tensor& output, std::string& error) {
    // C = alpha * A@B + beta * C_in. A@B is computed with the matmul
    // accumulation discipline; the combine is an elementwise fp32 step.
    std::vector<Tensor> ab_inputs = {inputs[0], inputs[1]};
    std::vector<std::vector<std::byte>> input_bytes(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const TensorStatus status = gather_bytes(inputs[i], input_bytes[i], error);
        if (status != TensorStatus::Ok) return status;
    }

    const DataType dtype = inputs[0].dtype();
    const TensorShape& a_shape = inputs[0].shape();
    const std::int64_t m = a_shape.dims[0];
    const std::int64_t k = a_shape.dims[1];
    const std::int64_t n = inputs[1].shape().dims[1];

    // Temporary fp32 accumulation buffer for A@B.
    std::vector<float> ab(static_cast<std::size_t>(m * n), 0.0f);
    for (std::int64_t i = 0; i < m; ++i) {
        for (std::int64_t j = 0; j < n; ++j) {
            float acc = 0.0f;
            for (std::int64_t kk = 0; kk < k; ++kk) {
                const float av = load_as_fp32(input_bytes[0].data(), i * k + kk, dtype);
                const float bv = load_as_fp32(input_bytes[1].data(), kk * n + j, dtype);
                acc += av * bv;
            }
            ab[static_cast<std::size_t>(i * n + j)] = acc;
        }
    }

    std::vector<std::byte> out_bytes(static_cast<std::size_t>(output.byte_size()),
                                     std::byte{0});
    for (std::int64_t i = 0; i < m; ++i) {
        for (std::int64_t j = 0; j < n; ++j) {
            const float cin = load_as_fp32(input_bytes[2].data(), i * n + j, dtype);
            const float result = params.gemm_alpha * ab[static_cast<std::size_t>(i * n + j)] +
                                 params.gemm_beta * cin;
            store_from_fp32(out_bytes.data(), i * n + j, dtype, result);
        }
    }
    return output.write_host(out_bytes.data(), out_bytes.size(), error);
}

// Reductions: ONE axis, sequential accumulation in memory order
// (deterministic). ReduceSum is defined on every dtype (integer sums are
// modular); ReduceMean is float-only (validated by the op rules).
TensorStatus execute_reduce(TensorOp op, const TensorOpParams& params,
                            const std::vector<Tensor>& inputs, Tensor& output,
                            std::string& error) {
    std::vector<std::byte> input_bytes;
    const TensorStatus status = gather_bytes(inputs[0], input_bytes, error);
    if (status != TensorStatus::Ok) return status;
    std::vector<std::byte> out_bytes(static_cast<std::size_t>(output.byte_size()),
                                     std::byte{0});

    const TensorShape& in_shape = inputs[0].shape();
    const DataType dtype = inputs[0].dtype();
    const std::int64_t axis = params.reduce_axis;
    const std::int64_t axis_len = in_shape.dims[static_cast<std::size_t>(axis)];

    // Outer = product of dims before axis, inner = after (row-major).
    std::int64_t outer = 1;
    for (std::int64_t d = 0; d < axis; ++d) outer *= in_shape.dims[static_cast<std::size_t>(d)];
    std::int64_t inner = 1;
    for (std::size_t d = static_cast<std::size_t>(axis) + 1; d < in_shape.rank(); ++d) {
        inner *= in_shape.dims[d];
    }

    for (std::int64_t o = 0; o < outer; ++o) {
        for (std::int64_t i = 0; i < inner; ++i) {
            // The executor materializes contiguous inputs (documented), so the
            // packed row-major mapping (o, a, i) -> (o*axis_len + a)*inner + i
            // is the storage layout. The op validation refuses non-contiguous
            // reduce inputs before dispatch.
            if (data_type_is_integer(dtype)) {
                // Integer reduction (INT32/INT8), modular sum / defined mean absence.
                if (op == TensorOp::ReduceSum) {
                    std::int64_t acc = 0;  // accumulate wide, store modular
                    for (std::int64_t a = 0; a < axis_len; ++a) {
                        const std::int64_t off = (o * axis_len + a) * inner + i;
                        if (dtype == DataType::INT32) {
                            std::int32_t v;
                            std::memcpy(&v, input_bytes.data() +
                                                 static_cast<std::ptrdiff_t>(off) * 4,
                                        sizeof(v));
                            acc += v;
                        } else {
                            std::int8_t v;
                            std::memcpy(&v, input_bytes.data() + static_cast<std::ptrdiff_t>(off),
                                        sizeof(v));
                            acc += v;
                        }
                    }
                    if (dtype == DataType::INT32) {
                        const std::int32_t out_v = static_cast<std::int32_t>(
                            static_cast<std::uint32_t>(acc));
                        std::memcpy(out_bytes.data() + static_cast<std::ptrdiff_t>(o * inner + i) * 4,
                                    &out_v, sizeof(out_v));
                    } else {
                        const std::int8_t out_v = static_cast<std::int8_t>(acc);
                        std::memcpy(out_bytes.data() + static_cast<std::ptrdiff_t>(o * inner + i),
                                    &out_v, sizeof(out_v));
                    }
                } else {
                    error = "reduce_mean is defined for floating dtypes only";
                    return TensorStatus::UnsupportedDtype;
                }
            } else {
                float acc = 0.0f;
                for (std::int64_t a = 0; a < axis_len; ++a) {
                    const std::int64_t off = (o * axis_len + a) * inner + i;
                    acc += load_as_fp32(input_bytes.data(), off, dtype);
                }
                if (op == TensorOp::ReduceMean) acc /= static_cast<float>(axis_len);
                store_from_fp32(out_bytes.data(), o * inner + i, dtype, acc);
            }
        }
    }
    return output.write_host(out_bytes.data(), out_bytes.size(), error);
}

// Activations: elementwise, float dtypes only (validated before dispatch).
TensorStatus execute_activation(TensorOp op, const std::vector<Tensor>& inputs, Tensor& output,
                                std::string& error) {
    std::vector<std::byte> input_bytes;
    const TensorStatus status = gather_bytes(inputs[0], input_bytes, error);
    if (status != TensorStatus::Ok) return status;
    std::vector<std::byte> out_bytes(static_cast<std::size_t>(output.byte_size()),
                                     std::byte{0});
    const DataType dtype = inputs[0].dtype();
    const std::int64_t elements = output.elements();
    // Contiguity was enforced by validation; iterate linearly.
    for (std::int64_t i = 0; i < elements; ++i) {
        const float x = load_as_fp32(input_bytes.data(), i, dtype);
        float result = 0.0f;
        switch (op) {
            case TensorOp::Relu:
                // NaN propagates (documented): a NaN input stays NaN.
                result = (x > 0.0f) ? x : ((x != x) ? x : 0.0f);
                break;
            case TensorOp::Sigmoid:
                result = 1.0f / (1.0f + std::exp(-x));
                break;
            case TensorOp::Tanh:
                result = std::tanh(x);
                break;
            case TensorOp::Softmax:
                error = "softmax requires the axis kernel (internal dispatch error)";
                return TensorStatus::Internal;
            default:
                error = "unknown activation (internal)";
                return TensorStatus::Internal;
        }
        store_from_fp32(out_bytes.data(), i, dtype, result);
    }
    return output.write_host(out_bytes.data(), out_bytes.size(), error);
}

// Softmax over the LAST axis with max-subtraction (the documented numerical
// stability method). Finite inputs only: a non-finite input is an explicit
// NumericalValidationFailure (the domain Phase 13 defines softmax for).
TensorStatus execute_softmax(const std::vector<Tensor>& inputs, Tensor& output,
                             std::string& error) {
    std::vector<std::byte> input_bytes;
    const TensorStatus status = gather_bytes(inputs[0], input_bytes, error);
    if (status != TensorStatus::Ok) return status;
    std::vector<std::byte> out_bytes(static_cast<std::size_t>(output.byte_size()),
                                     std::byte{0});
    const DataType dtype = inputs[0].dtype();
    const TensorShape& shape = inputs[0].shape();
    const std::int64_t last = shape.dims[shape.rank() - 1];
    std::int64_t rows = 1;
    for (std::size_t d = 0; d + 1 < shape.rank(); ++d) rows *= shape.dims[d];

    std::vector<float> row(static_cast<std::size_t>(last), 0.0f);
    for (std::int64_t r = 0; r < rows; ++r) {
        float max_value = 0.0f;
        bool first = true;
        for (std::int64_t c = 0; c < last; ++c) {
            const float x = load_as_fp32(input_bytes.data(), r * last + c, dtype);
            if (std::isnan(x) || std::isinf(x)) {
                error = "softmax requires finite inputs (non-finite value on the softmax axis)";
                return TensorStatus::NumericalValidationFailure;
            }
            if (first || x > max_value) {
                max_value = x;
                first = false;
            }
        }
        float sum = 0.0f;
        for (std::int64_t c = 0; c < last; ++c) {
            const float x = load_as_fp32(input_bytes.data(), r * last + c, dtype);
            row[static_cast<std::size_t>(c)] = std::exp(x - max_value);
            sum += row[static_cast<std::size_t>(c)];
        }
        for (std::int64_t c = 0; c < last; ++c) {
            store_from_fp32(out_bytes.data(), r * last + c, dtype,
                            row[static_cast<std::size_t>(c)] / sum);
        }
    }
    return output.write_host(out_bytes.data(), out_bytes.size(), error);
}

// Transpose (materialized): out[j][i] = in[i][j], any dtype, byte-exact
// element copies.
TensorStatus execute_transpose(const std::vector<Tensor>& inputs, Tensor& output,
                               std::string& error) {
    std::vector<std::byte> input_bytes;
    const TensorStatus status = gather_bytes(inputs[0], input_bytes, error);
    if (status != TensorStatus::Ok) return status;
    std::vector<std::byte> out_bytes(static_cast<std::size_t>(output.byte_size()),
                                     std::byte{0});
    const TensorShape& in_shape = inputs[0].shape();
    const TensorLayout& in_layout = inputs[0].layout();
    const std::size_t width = data_type_byte_width(inputs[0].dtype());
    const std::int64_t rows = in_shape.dims[0];
    const std::int64_t cols = in_shape.dims[1];
    for (std::int64_t i = 0; i < rows; ++i) {
        for (std::int64_t j = 0; j < cols; ++j) {
            const std::int64_t src_off = offset_of(in_layout, {i, j});
            const std::int64_t dst_off = j * rows + i;
            std::memcpy(out_bytes.data() + static_cast<std::ptrdiff_t>(dst_off) * width,
                        input_bytes.data() + static_cast<std::ptrdiff_t>(src_off) * width,
                        width);
        }
    }
    return output.write_host(out_bytes.data(), out_bytes.size(), error);
}

// Reshape of a contiguous tensor: the element stream is identical, so the
// whole storage is byte-copied (element counts were validated equal).
TensorStatus execute_reshape(const std::vector<Tensor>& inputs, Tensor& output,
                             std::string& error) {
    std::vector<std::byte> input_bytes;
    const TensorStatus status = gather_bytes(inputs[0], input_bytes, error);
    if (status != TensorStatus::Ok) return status;
    return output.write_host(input_bytes.data(), input_bytes.size(), error);
}

}  // namespace

TensorStatus CpuReferenceTensorBackend::execute(TensorOpRequest& request,
                                                std::string& error) {
    // Defensive re-validation against THIS backend's capabilities (the
    // executor dispatches by capability; a backend never executes an op or
    // dtype it does not list).
    if (!capabilities_.supports_op(request.op)) {
        error = std::string("backend 'tensor_reference' does not list op '") +
                to_string(request.op) + "'";
        return TensorStatus::UnsupportedOperation;
    }
    if (!capabilities_.supports_dtype(request.output.dtype())) {
        error = std::string("backend 'tensor_reference' does not list dtype '") +
                to_string(request.output.dtype()) + "'";
        return TensorStatus::UnsupportedDtype;
    }

    switch (request.op) {
        case TensorOp::Add:
        case TensorOp::Subtract:
        case TensorOp::Multiply:
        case TensorOp::Divide:
            return execute_elementwise(request.op, request.inputs, request.output, error);
        case TensorOp::MatMul:
            return execute_matmul(request.inputs, request.output, error);
        case TensorOp::GEMM:
            return execute_gemm(request.params, request.inputs, request.output, error);
        case TensorOp::ReduceSum:
        case TensorOp::ReduceMean:
            return execute_reduce(request.op, request.params, request.inputs, request.output,
                                  error);
        case TensorOp::Relu:
        case TensorOp::Sigmoid:
        case TensorOp::Tanh:
            return execute_activation(request.op, request.inputs, request.output, error);
        case TensorOp::Softmax:
            return execute_softmax(request.inputs, request.output, error);
        case TensorOp::Transpose:
            return execute_transpose(request.inputs, request.output, error);
        case TensorOp::Reshape:
            return execute_reshape(request.inputs, request.output, error);
    }
    error = "unknown tensor op in reference backend";
    return TensorStatus::Internal;
}

// ---------------------------------------------------------------------------
// RuntimeElementwiseTensorBackend — the bridge into the existing engine
// ---------------------------------------------------------------------------

RuntimeElementwiseTensorBackend::RuntimeElementwiseTensorBackend(vortyx::compute::Runtime& runtime)
    : runtime_(&runtime) {
    capabilities_.supported_ops = {TensorOp::Add, TensorOp::Multiply};
    capabilities_.supported_dtypes = {DataType::INT32};
    capabilities_.supports_strided_input = false;
    capabilities_.supports_broadcast = false;  // ComputeTask requires equal sizes
    capabilities_.max_rank = kMaxTensorRank;
    capabilities_.max_elements = kMaxTensorBytes / 4;  // int32 elements within the byte cap
    capabilities_.max_bytes = kMaxTensorBytes;
    capabilities_.matrix_acceleration = MatrixAcceleration::NotClaimed;
    // NOTE: the ops route through the REAL Phase 10 engine — on a host with
    // a working Vulkan device they execute on the GPU. The backend still
    // claims no matrix acceleration: elementwise int32 is not matrix work.
}

TensorStatus RuntimeElementwiseTensorBackend::execute(TensorOpRequest& request,
                                                      std::string& error) {
    if (!capabilities_.supports_op(request.op)) {
        error = std::string("backend 'tensor_runtime' does not list op '") +
                to_string(request.op) + "' (only int32 add/multiply route through the "
                                        "existing engine)";
        return TensorStatus::UnsupportedOperation;
    }
    if (!capabilities_.supports_dtype(DataType::INT32) || request.output.dtype() != DataType::INT32) {
        error = "backend 'tensor_runtime' executes int32 tensors only";
        return TensorStatus::UnsupportedDtype;
    }
    if (request.inputs.size() != 2) {
        error = "backend 'tensor_runtime' requires two inputs";
        return TensorStatus::InvalidInput;
    }
    for (const Tensor& t : request.inputs) {
        if (!t.is_contiguous() || t.shape() != request.output.shape()) {
            error = "backend 'tensor_runtime' requires same-shape contiguous int32 tensors "
                    "(broadcast goes through the reference backend)";
            return TensorStatus::UnsupportedLayout;
        }
    }

    // Read the tensor storages into the engine's host-vector contract, then
    // hand the work to the REAL execution path (task -> buffers -> backend).
    std::vector<std::int32_t> a(static_cast<std::size_t>(request.inputs[0].elements()), 0);
    std::vector<std::int32_t> b(static_cast<std::size_t>(request.inputs[1].elements()), 0);
    TensorStatus status = request.inputs[0].read_host(a.data(), a.size() * sizeof(std::int32_t),
                                                      error);
    if (status != TensorStatus::Ok) return status;
    status = request.inputs[1].read_host(b.data(), b.size() * sizeof(std::int32_t), error);
    if (status != TensorStatus::Ok) return status;

    vortyx::compute::ComputeTask task;
    task.op = (request.op == TensorOp::Add) ? vortyx::compute::ComputeOp::VectorAdd
                                            : vortyx::compute::ComputeOp::VectorMultiply;
    task.a = std::move(a);
    task.b = std::move(b);
    task.scalar = 0;

    const vortyx::compute::ComputeTaskResult result = runtime_->execute(task);
    if (result.status != vortyx::compute::Status::Ok) {
        error = "existing engine refused the tensor workload: " + result.error;
        return tensor_status_from_compute_status(result.status);
    }

    return request.output.write_host(result.data.data(),
                                     result.data.size() * sizeof(std::int32_t), error);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

ITensorBackend* select_backend(const std::vector<ITensorBackend*>& backends,
                               const TensorRequirements& requirements) {
    for (ITensorBackend* backend : backends) {
        if (backend == nullptr) continue;
        if (requirements.satisfied_by(backend->capabilities())) return backend;
    }
    return nullptr;
}

std::string describe_dispatch_failure(const std::vector<ITensorBackend*>& backends,
                                      TensorOp op, DataType dtype) {
    std::string text = "no backend can execute op '" + std::string(to_string(op)) +
                       "' with dtype '" + to_string(dtype) + "'; registered backends:";
    for (const ITensorBackend* backend : backends) {
        if (backend == nullptr) continue;
        text += std::string(" '") + backend->name() + "'(ops:";
        for (const TensorOp supported : backend->capabilities().supported_ops) {
            text += " " + std::string(to_string(supported));
        }
        text += ", dtypes:";
        for (const DataType supported : backend->capabilities().supported_dtypes) {
            text += " " + std::string(to_string(supported));
        }
        text += ")";
    }
    return text;
}

}  // namespace vortyx::tensor
