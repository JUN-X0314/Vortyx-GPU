// Tensor operation tests (Phase 13) — the reference kernels, one op at a
// time, against hand-computed deterministic values and the documented
// semantics. Exact comparisons where the math is exact (integers, small
// float values), tolerance-based verification where fp32 accumulation is
// involved (checked against an FP32 baseline).
//
// Convention: plain main() + check(), like every other test in this project.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "tensor/tensor.hpp"

using namespace vortyx::tensor;
using ST = TensorStatus;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_status(ST actual, ST expected, const std::string& message) {
    check(actual == expected,
          message + " (expected " + tensor_status_code(expected) + ", got " +
              tensor_status_code(actual) + ")");
}

// Reads a float tensor's logical elements (contiguous) into a vector.
std::vector<float> floats(const Tensor& tensor) {
    std::vector<float> out(static_cast<std::size_t>(tensor.elements()), 0.0f);
    std::string error;
    if (tensor.read_host(out.data(), out.size() * sizeof(float), error) != ST::Ok) {
        std::cerr << "FAIL: read failed: " << error << "\n";
        ++failures;
        return {};
    }
    return out;
}

std::vector<std::int32_t> int32s(const Tensor& tensor) {
    std::vector<std::int32_t> out(static_cast<std::size_t>(tensor.elements()), 0);
    std::string error;
    if (tensor.read_host(out.data(), out.size() * sizeof(std::int32_t), error) != ST::Ok) {
        std::cerr << "FAIL: read failed: " << error << "\n";
        ++failures;
        return {};
    }
    return out;
}

bool near(float a, float b, float tolerance = 1e-5f) {
    return std::fabs(a - b) <= tolerance;
}

bool same_floats(const std::vector<float>& got, const std::vector<float>& want,
                 float tolerance = 1e-5f) {
    if (got.size() != want.size()) return false;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (!near(got[i], want[i], tolerance)) return false;
    }
    return true;
}

}  // namespace

int main() {
    // The manager MUST live in a shared_ptr (the Phase 4 contract: Buffer
    // handles observe it weakly).
    auto manager = std::make_shared<vortyx::resource::ResourceManager>();
    vortyx::resource::CpuBufferProvider cpu_provider;
    check(manager->register_provider(&cpu_provider), "cpu provider registers");

    TensorExecutor::Deps deps;
    deps.resources = manager.get();
    std::unique_ptr<TensorExecutor> executor;
    std::string error;
    check(TensorExecutor::create(deps, executor, error) == ST::Ok, "executor created");

    // =====================================================================
    // 1. MatMul 2x3 * 3x2 — hand-verified deterministic values
    // =====================================================================
    {
        // A = [[1,2,3],[4,5,6]]  B = [[7,8],[9,10],[11,12]]
        // A@B = [[58,64],[139,154]]
        const float a[6] = {1, 2, 3, 4, 5, 6};
        const float b[6] = {7, 8, 9, 10, 11, 12};
        Tensor ta;
        Tensor tb;
        check(Tensor::from_host(*manager, TensorShape::make({2, 3}), DataType::FP32, a,
                                sizeof(a), ta, error) == ST::Ok, "matmul A");
        check(Tensor::from_host(*manager, TensorShape::make({3, 2}), DataType::FP32, b,
                                sizeof(b), tb, error) == ST::Ok, "matmul B");

        Tensor out;
        check_status(executor->execute_op(TensorOp::MatMul, TensorOpParams{}, {ta, tb}, out,
                                          error),
                     ST::Ok, "matmul executes");
        check(out.shape() == TensorShape::make({2, 2}), "matmul output shape");
        check(same_floats(floats(out), {58, 64, 139, 154}, 1e-3f), "matmul values hand-verified");

        // K mismatch refused BEFORE execution (the exact error).
        const float wide[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        Tensor wrong_k;
        check(Tensor::from_host(*manager, TensorShape::make({2, 4}), DataType::FP32, wide,
                                sizeof(wide), wrong_k, error) == ST::Ok, "K-mismatch tensor");
        Tensor refused;
        check_status(executor->execute_op(TensorOp::MatMul, TensorOpParams{}, {ta, wrong_k},
                                          refused, error),
                     ST::InvalidShape, "K mismatch refused");
        check(error.find("K dimension mismatch") != std::string::npos,
              "the K mismatch error names the rule");

        // Rank mismatch refused.
        Tensor rank1;
        const float v[3] = {1, 2, 3};
        check(Tensor::from_host(*manager, TensorShape::make({3}), DataType::FP32, v, sizeof(v),
                                rank1, error) == ST::Ok, "rank-1 tensor");
        check_status(executor->execute_op(TensorOp::MatMul, TensorOpParams{}, {ta, rank1},
                                          refused, error),
                     ST::InvalidShape, "rank mismatch refused");

        // Batched 3D: two batches of 1x2 @ 2x1.
        const float ba[4] = {1, 2, 3, 4};   // batch0 [[1,2]] batch1 [[3,4]]
        const float bb[4] = {5, 6, 7, 8};   // batch0 [[5],[6]] batch1 [[7],[8]]
        Tensor tba;
        Tensor tbb;
        check(Tensor::from_host(*manager, TensorShape::make({2, 1, 2}), DataType::FP32, ba,
                                sizeof(ba), tba, error) == ST::Ok, "batched A");
        check(Tensor::from_host(*manager, TensorShape::make({2, 2, 1}), DataType::FP32, bb,
                                sizeof(bb), tbb, error) == ST::Ok, "batched B");
        Tensor batched_out;
        check_status(executor->execute_op(TensorOp::MatMul, TensorOpParams{}, {tba, tbb},
                                          batched_out, error),
                     ST::Ok, "batched matmul executes");
        check(batched_out.shape() == TensorShape::make({2, 1, 1}), "batched output shape");
        // 1*5+2*6=17, 3*7+4*8=53.
        check(same_floats(floats(batched_out), {17, 53}, 1e-3f), "batched matmul values");
    }

    // =====================================================================
    // 2. GEMM: C = alpha*A@B + beta*C
    // =====================================================================
    {
        const float a[2] = {1, 2};  // 1x2
        const float b[2] = {3, 4};  // 2x1
        const float c[1] = {100};
        Tensor ta;
        Tensor tb;
        Tensor tc;
        check(Tensor::from_host(*manager, TensorShape::make({1, 2}), DataType::FP32, a,
                                sizeof(a), ta, error) == ST::Ok, "gemm A");
        check(Tensor::from_host(*manager, TensorShape::make({2, 1}), DataType::FP32, b,
                                sizeof(b), tb, error) == ST::Ok, "gemm B");
        check(Tensor::from_host(*manager, TensorShape::make({1, 1}), DataType::FP32, c,
                                sizeof(c), tc, error) == ST::Ok, "gemm C");

        TensorOpParams params;
        params.gemm_alpha = 2.0f;
        params.gemm_beta = 0.5f;
        Tensor out;
        check_status(executor->execute_op(TensorOp::GEMM, params, {ta, tb, tc}, out, error),
                     ST::Ok, "gemm executes");
        // 2*(1*3+2*4) + 0.5*100 = 22 + 50 = 72.
        check(same_floats(floats(out), {72.0f}, 1e-3f), "gemm alpha/beta hand-verified");

        // Wrong C shape refused.
        Tensor wrong_c;
        const float c9[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        check(Tensor::from_host(*manager, TensorShape::make({3, 3}), DataType::FP32, c9,
                                sizeof(c9), wrong_c, error) == ST::Ok, "wrong C");
        Tensor refused;
        check_status(executor->execute_op(TensorOp::GEMM, params, {ta, tb, wrong_c}, refused,
                                          error),
                     ST::InvalidShape, "gemm C shape enforced");
    }

    // =====================================================================
    // 3. Elementwise: int32 (exact) with broadcast, defined modular semantics
    // =====================================================================
    {
        const std::int32_t a[4] = {1, 2, 3, 4};
        const std::int32_t b[4] = {10, 20, 30, 40};
        Tensor ta;
        Tensor tb;
        check(Tensor::from_host(*manager, TensorShape::make({4}), DataType::INT32, a,
                                sizeof(a), ta, error) == ST::Ok, "int32 A");
        check(Tensor::from_host(*manager, TensorShape::make({4}), DataType::INT32, b,
                                sizeof(b), tb, error) == ST::Ok, "int32 B");

        Tensor sum;
        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {ta, tb}, sum, error),
                     ST::Ok, "int32 add executes");
        check(int32s(sum) == std::vector<std::int32_t>({11, 22, 33, 44}), "int32 add exact");

        Tensor diff;
        check_status(executor->execute_op(TensorOp::Subtract, TensorOpParams{}, {tb, ta}, diff,
                                          error),
                     ST::Ok, "int32 subtract");
        check(int32s(diff) == std::vector<std::int32_t>({9, 18, 27, 36}), "int32 subtract exact");

        Tensor product;
        check_status(executor->execute_op(TensorOp::Multiply, TensorOpParams{}, {ta, tb},
                                          product, error),
                     ST::Ok, "int32 multiply");
        check(int32s(product) == std::vector<std::int32_t>({10, 40, 90, 160}),
              "int32 multiply exact");

        Tensor quotient;
        check_status(executor->execute_op(TensorOp::Divide, TensorOpParams{}, {tb, ta},
                                          quotient, error),
                     ST::Ok, "int32 divide");
        check(int32s(quotient) == std::vector<std::int32_t>({10, 10, 10, 10}),
              "int32 divide (truncating) exact");

        // Integer division by zero: the DEFINED domain error.
        const std::int32_t zero[4] = {0, 0, 0, 0};
        Tensor tz;
        check(Tensor::from_host(*manager, TensorShape::make({4}), DataType::INT32, zero,
                                sizeof(zero), tz, error) == ST::Ok, "zero tensor");
        Tensor refused;
        check_status(executor->execute_op(TensorOp::Divide, TensorOpParams{}, {ta, tz}, refused,
                                          error),
                     ST::NumericalValidationFailure, "int divide-by-zero refused");

        // Integer overflow is DEFINED modular arithmetic (documented).
        const std::int32_t big[1] = {2147483647};
        const std::int32_t one[1] = {1};
        Tensor tbig;
        Tensor tone;
        check(Tensor::from_host(*manager, TensorShape::make({1}), DataType::INT32, big,
                                sizeof(big), tbig, error) == ST::Ok, "INT32_MAX");
        check(Tensor::from_host(*manager, TensorShape::make({1}), DataType::INT32, one,
                                sizeof(one), tone, error) == ST::Ok, "one");
        Tensor wrapped;
        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {tbig, tone}, wrapped,
                                          error),
                     ST::Ok, "overflowing add executes");
        check(int32s(wrapped) == std::vector<std::int32_t>({static_cast<std::int32_t>(2147483648u)}),
              "int32 add is defined modular arithmetic");

        // int32 add routes through the runtime adapter path check happens in
        // the dispatch test; here verify the BROADCAST add through the
        // reference kernel (the adapter does not claim broadcast).
        const std::int32_t bias[3] = {100, 200, 300};
        Tensor tbias;
        check(Tensor::from_host(*manager, TensorShape::make({3}), DataType::INT32, bias,
                                sizeof(bias), tbias, error) == ST::Ok, "bias");
        Tensor matrix;
        const std::int32_t m[6] = {1, 2, 3, 4, 5, 6};
        check(Tensor::from_host(*manager, TensorShape::make({2, 3}), DataType::INT32, m,
                                sizeof(m), matrix, error) == ST::Ok, "matrix");
        Tensor broadcast_sum;
        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {matrix, tbias},
                                          broadcast_sum, error),
                     ST::Ok, "broadcast add executes");
        check(broadcast_sum.shape() == TensorShape::make({2, 3}), "broadcast add shape");
        check(int32s(broadcast_sum) ==
                  std::vector<std::int32_t>({101, 202, 303, 104, 205, 306}),
              "broadcast add values");
    }

    // =====================================================================
    // 4. Elementwise float: IEEE divide semantics (inf/nan defined)
    // =====================================================================
    {
        const float a[3] = {1.0f, 0.0f, -4.0f};
        const float b[3] = {0.0f, 0.0f, 2.0f};
        Tensor ta;
        Tensor tb;
        check(Tensor::from_host(*manager, TensorShape::make({3}), DataType::FP32, a, sizeof(a),
                                ta, error) == ST::Ok, "float A");
        check(Tensor::from_host(*manager, TensorShape::make({3}), DataType::FP32, b, sizeof(b),
                                tb, error) == ST::Ok, "float B");
        Tensor out;
        check_status(executor->execute_op(TensorOp::Divide, TensorOpParams{}, {ta, tb}, out,
                                          error),
                     ST::Ok, "float divide executes");
        const std::vector<float> got = floats(out);
        check(std::isinf(got[0]) && got[0] > 0, "1/0 = +inf (IEEE, documented)");
        check(std::isnan(got[1]), "0/0 = NaN (IEEE, documented)");
        check(near(got[2], -2.0f), "-4/2 = -2");
    }

    // =====================================================================
    // 5. Reductions: sum (one axis, all dtypes) + mean (float only)
    // =====================================================================
    {
        const float m[6] = {1, 2, 3, 4, 5, 6};  // [[1,2,3],[4,5,6]]
        Tensor tm;
        check(Tensor::from_host(*manager, TensorShape::make({2, 3}), DataType::FP32, m,
                                sizeof(m), tm, error) == ST::Ok, "reduce input");

        TensorOpParams axis0;
        axis0.reduce_axis = 0;
        Tensor col_sum;
        check_status(executor->execute_op(TensorOp::ReduceSum, axis0, {tm}, col_sum, error),
                     ST::Ok, "reduce sum axis 0");
        check(col_sum.shape() == TensorShape::make({3}), "axis-0 output shape");
        check(same_floats(floats(col_sum), {5, 7, 9}), "axis-0 sum values");

        TensorOpParams axis1;
        axis1.reduce_axis = 1;
        Tensor row_mean;
        check_status(executor->execute_op(TensorOp::ReduceMean, axis1, {tm}, row_mean, error),
                     ST::Ok, "reduce mean axis 1");
        check(row_mean.shape() == TensorShape::make({2}), "axis-1 output shape");
        check(same_floats(floats(row_mean), {2, 5}), "axis-1 mean values");

        // Axis out of range refused.
        TensorOpParams bad_axis;
        bad_axis.reduce_axis = 2;
        Tensor refused;
        check_status(executor->execute_op(TensorOp::ReduceSum, bad_axis, {tm}, refused, error),
                     ST::InvalidShape, "reduce axis out of range refused");

        // INT32 sum (exact); ReduceMean on int refused (float-only, documented).
        const std::int32_t im[2] = {2147483000, 1000};
        Tensor tim;
        check(Tensor::from_host(*manager, TensorShape::make({2}), DataType::INT32, im,
                                sizeof(im), tim, error) == ST::Ok, "int32 reduce input");
        TensorOpParams last_axis;
        last_axis.reduce_axis = 0;
        Tensor int_sum;
        check_status(executor->execute_op(TensorOp::ReduceSum, last_axis, {tim}, int_sum, error),
                     ST::Ok, "int32 reduce sum executes");
        const std::vector<std::int32_t> want_sum = {
            static_cast<std::int32_t>(static_cast<std::uint32_t>(2147483000) + 1000u)};
        check(int32s(int_sum) == want_sum, "int32 sum wraps modularly (defined)");
        Tensor int_mean;
        check_status(executor->execute_op(TensorOp::ReduceMean, last_axis, {tim}, int_mean,
                                          error),
                     ST::UnsupportedDtype, "reduce_mean on int refused (float-only)");
    }

    // =====================================================================
    // 6. Activations: ReLU (NaN propagates), Sigmoid, Tanh — known values
    // =====================================================================
    {
        const float v[5] = {-2.0f, -0.5f, 0.0f, 0.5f, 2.0f};
        Tensor tv;
        check(Tensor::from_host(*manager, TensorShape::make({5}), DataType::FP32, v, sizeof(v),
                                tv, error) == ST::Ok, "activation input");

        Tensor relu;
        check_status(executor->execute_op(TensorOp::Relu, TensorOpParams{}, {tv}, relu, error),
                     ST::Ok, "relu executes");
        check(same_floats(floats(relu), {0, 0, 0, 0.5f, 2.0f}), "relu values");

        Tensor sigmoid;
        check_status(executor->execute_op(TensorOp::Sigmoid, TensorOpParams{}, {tv}, sigmoid,
                                          error),
                     ST::Ok, "sigmoid executes");
        const std::vector<float> sg = floats(sigmoid);
        check(near(sg[0], 1.0f / (1.0f + std::exp(2.0f))), "sigmoid(-2)");
        check(near(sg[4], 1.0f / (1.0f + std::exp(-2.0f))), "sigmoid(2)");

        Tensor tanh_out;
        check_status(executor->execute_op(TensorOp::Tanh, TensorOpParams{}, {tv}, tanh_out,
                                          error),
                     ST::Ok, "tanh executes");
        const std::vector<float> th = floats(tanh_out);
        check(near(th[0], std::tanh(-2.0f)) && near(th[4], std::tanh(2.0f)), "tanh values");

        // Activation on int refused (float-only, documented).
        const std::int32_t iv[2] = {1, 2};
        Tensor tiv;
        check(Tensor::from_host(*manager, TensorShape::make({2}), DataType::INT32, iv,
                                sizeof(iv), tiv, error) == ST::Ok, "int tensor");
        Tensor refused;
        check_status(executor->execute_op(TensorOp::Relu, TensorOpParams{}, {tiv}, refused,
                                          error),
                     ST::UnsupportedDtype, "relu on int32 refused");
    }

    // =====================================================================
    // 7. Softmax: max-subtraction stability (large magnitudes), finite domain
    // =====================================================================
    {
        const float v[3] = {1000.0f, 1000.0f, 1000.0f};
        Tensor tv;
        check(Tensor::from_host(*manager, TensorShape::make({3}), DataType::FP32, v, sizeof(v),
                                tv, error) == ST::Ok, "softmax input");
        Tensor out;
        check_status(executor->execute_op(TensorOp::Softmax, TensorOpParams{}, {tv}, out, error),
                     ST::Ok, "softmax on large values executes (stable)");
        check(same_floats(floats(out), {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f}, 1e-6f),
              "equal large inputs -> uniform (no overflow)");

        const float mixed[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        Tensor tm;
        check(Tensor::from_host(*manager, TensorShape::make({2, 2}), DataType::FP32, mixed,
                                sizeof(mixed), tm, error) == ST::Ok, "softmax matrix");
        Tensor sm;
        check_status(executor->execute_op(TensorOp::Softmax, TensorOpParams{}, {tm}, sm, error),
                     ST::Ok, "2D softmax executes (last axis)");
        const std::vector<float> got = floats(sm);
        const float e0 = std::exp(1.0f - 2.0f);
        const float e1 = std::exp(0.0f);
        check(near(got[0], e0 / (e0 + e1)) && near(got[1], e1 / (e0 + e1)),
              "last-axis normalization");

        // Non-finite input: the explicit domain error (documented).
        const float with_inf[2] = {1.0f, std::numeric_limits<float>::infinity()};
        Tensor tinf;
        check(Tensor::from_host(*manager, TensorShape::make({2}), DataType::FP32, with_inf,
                                sizeof(with_inf), tinf, error) == ST::Ok, "inf tensor");
        Tensor refused;
        check_status(executor->execute_op(TensorOp::Softmax, TensorOpParams{}, {tinf}, refused,
                                          error),
                     ST::NumericalValidationFailure, "softmax on non-finite refused");
    }

    // =====================================================================
    // 8. Transpose + Reshape as ops (materialized, any dtype)
    // =====================================================================
    {
        const float m[6] = {1, 2, 3, 4, 5, 6};  // [[1,2,3],[4,5,6]]
        Tensor tm;
        check(Tensor::from_host(*manager, TensorShape::make({2, 3}), DataType::FP32, m,
                                sizeof(m), tm, error) == ST::Ok, "transpose input");
        Tensor out;
        check_status(executor->execute_op(TensorOp::Transpose, TensorOpParams{}, {tm}, out,
                                          error),
                     ST::Ok, "transpose executes");
        check(out.shape() == TensorShape::make({3, 2}), "transpose output shape");
        check(out.is_contiguous(), "transpose output is materialized contiguous");
        // Column-major read of the original = [[1,4],[2,5],[3,6]].
        check(same_floats(floats(out), {1, 4, 2, 5, 3, 6}), "transpose values");

        // Transpose of a strided view: materializes correctly.
        Tensor view;
        check(tm.transpose(view, error) == ST::Ok, "strided view");
        Tensor materialized_view;
        check_status(executor->execute_op(TensorOp::Transpose, TensorOpParams{}, {view},
                                          materialized_view, error),
                     ST::Ok, "transpose of a view executes (double transpose)");
        check(same_floats(floats(materialized_view), {1, 2, 3, 4, 5, 6}),
              "double transpose restores the row-major stream");

        // Reshape as an op (element-count preserving, byte-exact).
        TensorOpParams params;
        params.reshape_target = TensorShape::make({6});
        Tensor flat;
        check_status(executor->execute_op(TensorOp::Reshape, params, {tm}, flat, error), ST::Ok,
                     "reshape op executes");
        check(flat.shape() == TensorShape::make({6}), "reshape output shape");
        check(same_floats(floats(flat), {1, 2, 3, 4, 5, 6}), "reshape preserves the stream");

        TensorOpParams bad_params;
        bad_params.reshape_target = TensorShape::make({4});
        Tensor refused;
        check_status(executor->execute_op(TensorOp::Reshape, bad_params, {tm}, refused, error),
                     ST::InvalidShape, "element-count-changing reshape refused");
    }

    // =====================================================================
    // 9. Dtype mismatch and mixed-arity refusals
    // =====================================================================
    {
        const float f[2] = {1.0f, 2.0f};
        const std::int32_t i[2] = {1, 2};
        Tensor tf;
        Tensor ti;
        check(Tensor::from_host(*manager, TensorShape::make({2}), DataType::FP32, f, sizeof(f),
                                tf, error) == ST::Ok, "float tensor");
        check(Tensor::from_host(*manager, TensorShape::make({2}), DataType::INT32, i, sizeof(i),
                                ti, error) == ST::Ok, "int tensor");

        Tensor refused;
        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {tf, ti}, refused,
                                          error),
                     ST::DtypeMismatch, "mixed-dtype add refused");

        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {tf}, refused, error),
                     ST::InvalidInput, "wrong arity refused");

        // MatMul on int32 refused (float-only, documented).
        const std::int32_t im[4] = {1, 2, 3, 4};
        Tensor tim;
        check(Tensor::from_host(*manager, TensorShape::make({2, 2}), DataType::INT32, im,
                                sizeof(im), tim, error) == ST::Ok, "int matrix");
        check_status(executor->execute_op(TensorOp::MatMul, TensorOpParams{}, {tim, tim},
                                          refused, error),
                     ST::UnsupportedDtype, "int32 matmul refused");
    }

    // =====================================================================
    // 10. FP16 / BF16: promote-compute-round semantics, verified via FP32
    // =====================================================================
    {
        // Genuine fp16 storage bytes: 1.0=0x3C00, 2.0=0x4000, 3.0=0x4200,
        // 4.0=0x4400 (all exact in binary16).
        const std::uint16_t a16[4] = {0x3C00, 0x4000, 0x4200, 0x4400};  // 1,2,3,4
        const std::uint16_t four16[4] = {0x4400, 0x4400, 0x4400, 0x4400};  // 4,4,4,4
        Tensor ta;
        Tensor tfour;
        check(Tensor::create(*manager, TensorShape::make({2, 2}), DataType::FP16,
                             TensorPlacement::host(), ta, error) == ST::Ok, "fp16 A alloc");
        check(Tensor::create(*manager, TensorShape::make({2, 2}), DataType::FP16,
                             TensorPlacement::host(), tfour, error) == ST::Ok, "fp16 four alloc");
        check(ta.write_host(a16, sizeof(a16), error) == ST::Ok, "genuine fp16 bytes for A");
        check(tfour.write_host(four16, sizeof(four16), error) == ST::Ok,
              "genuine fp16 bytes for four");

        // B = A + 4 (elementwise fp16 promote-compute-round: exact here).
        Tensor b_plus;
        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {ta, tfour}, b_plus,
                                          error),
                     ST::Ok, "fp16 add executes");
        check(b_plus.shape() == TensorShape::make({2, 2}), "fp16 add shape");

        // FP32 BASELINE: A=[[1,2],[3,4]] B=[[5,6],[7,8]] -> [[19,22],[43,50]].
        Tensor result;
        check_status(executor->execute_op(TensorOp::MatMul, TensorOpParams{}, {ta, b_plus},
                                          result, error),
                     ST::Ok, "fp16 matmul executes");
        // The result is an fp16 tensor: read its raw bytes (result.byte_size())
        // and decode each binary16 exactly (the expected values 19/22/43/50
        // are exact in fp16, so the decoded comparison IS the FP32 baseline).
        std::vector<std::byte> raw(static_cast<std::size_t>(result.byte_size()));
        check(result.read_host(raw.data(), raw.size(), error) == ST::Ok, "fp16 result read");
        auto decode_fp16 = [](std::uint16_t h) -> float {
            const std::uint32_t sign = static_cast<std::uint32_t>(h >> 15) & 0x1u;
            const std::uint32_t exp = static_cast<std::uint32_t>(h >> 10) & 0x1Fu;
            const std::uint32_t frac = static_cast<std::uint32_t>(h) & 0x3FFu;
            std::uint32_t bits;
            if (exp == 0) {
                bits = (frac == 0) ? (sign << 31) : 0;  // test domain: no subnormals expected
            } else if (exp == 31) {
                bits = (sign << 31) | 0x7F800000u | (frac << 13);
            } else {
                bits = (sign << 31) | ((exp - 15 + 127) << 23) | (frac << 13);
            }
            float out;
            std::memcpy(&out, &bits, sizeof(out));
            return out;
        };
        std::vector<float> decoded;
        for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
            std::uint16_t h = 0;
            std::memcpy(&h, raw.data() + i, 2);
            decoded.push_back(decode_fp16(h));
        }
        check(same_floats(decoded, {19, 22, 43, 50}, 1e-3f),
              "fp16 matmul matches the FP32 baseline (promote-compute-round)");

        // FP16 rounds at the store: 1 + 0.5 -> exact; verify one rounding case
        // via the promote-compute-round contract: 1.5*2^0 stored as fp16 is
        // exact (0x3E00), so add 1.0 + 0.5 gives exactly 1.5.
        const std::uint16_t half16[1] = {0x3800};  // 0.5
        Tensor thalf;
        check(Tensor::create(*manager, TensorShape::make({1}), DataType::FP16,
                             TensorPlacement::host(), thalf, error) == ST::Ok, "fp16 half alloc");
        check(thalf.write_host(half16, sizeof(half16), error) == ST::Ok, "fp16 half bytes");
        const std::uint16_t one16[1] = {0x3C00};  // 1.0
        Tensor tone;
        check(Tensor::create(*manager, TensorShape::make({1}), DataType::FP16,
                             TensorPlacement::host(), tone, error) == ST::Ok, "fp16 one alloc");
        check(tone.write_host(one16, sizeof(one16), error) == ST::Ok, "fp16 one bytes");
        Tensor one_and_half;
        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {tone, thalf},
                                          one_and_half, error),
                     ST::Ok, "fp16 1+0.5 executes");
        std::vector<std::byte> raw16(2);
        check(one_and_half.read_host(raw16.data(), 2, error) == ST::Ok, "fp16 result read");
        std::uint16_t got16 = 0;
        std::memcpy(&got16, raw16.data(), 2);
        check(got16 == 0x3E00, "fp16 1.0 + 0.5 stores exactly 1.5 (0x3E00)");

        // BF16: 1.0 (0x3F80) + 4.0 (0x4080) = 5.0 (0x40A0), exact in bf16.
        const std::uint16_t one_b[1] = {0x3F80};
        const std::uint16_t four_b[1] = {0x4080};
        Tensor tone_b;
        Tensor tfour_b;
        check(Tensor::create(*manager, TensorShape::make({1}), DataType::BF16,
                             TensorPlacement::host(), tone_b, error) == ST::Ok, "bf16 one alloc");
        check(Tensor::create(*manager, TensorShape::make({1}), DataType::BF16,
                             TensorPlacement::host(), tfour_b, error) == ST::Ok,
              "bf16 four alloc");
        check(tone_b.write_host(one_b, sizeof(one_b), error) == ST::Ok, "bf16 one bytes");
        check(tfour_b.write_host(four_b, sizeof(four_b), error) == ST::Ok, "bf16 four bytes");
        Tensor bf_sum;
        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {tone_b, tfour_b},
                                          bf_sum, error),
                     ST::Ok, "bf16 add executes");
        std::vector<std::byte> raw_b(2);
        check(bf_sum.read_host(raw_b.data(), 2, error) == ST::Ok, "bf16 result read");
        std::uint16_t got_b = 0;
        std::memcpy(&got_b, raw_b.data(), 2);
        check(got_b == 0x40A0, "bf16 1+4=5 round-trips to 0x40A0");
    }

    if (failures == 0) {
        std::cout << "Tensor op tests passed.\n";
        return 0;
    }
    std::cerr << failures << " tensor op test(s) failed.\n";
    return 1;
}
