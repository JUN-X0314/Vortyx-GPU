// Tensor dispatch tests (Phase 13) — capability-based backend selection,
// the runtime adapter's honest restrictions, unsupported op/dtype refusals,
// and the documented error-code vocabulary (stability + the mapping to the
// existing compute status model).
//
// Convention: plain main() + check(), like every other test in this project.

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "core/compute/runtime.hpp"
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

std::vector<float> floats_of(const Tensor& tensor) {
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

}  // namespace

int main() {
    // The manager MUST live in a shared_ptr (the Phase 4 contract: Buffer
    // handles observe it weakly).
    auto manager = std::make_shared<vortyx::resource::ResourceManager>();
    vortyx::resource::CpuBufferProvider cpu_provider;
    check(manager->register_provider(&cpu_provider), "cpu provider registers");

    // =====================================================================
    // 1. The capability vocabulary: validation + honest defaults
    // =====================================================================
    {
        std::string error;
        TensorCapabilities caps;
        caps.supported_ops = {TensorOp::MatMul, TensorOp::Add};
        caps.supported_dtypes = {DataType::FP32};
        caps.max_rank = 4;
        caps.max_elements = 1024;
        caps.max_bytes = 4096;
        check(caps.validate(error) == ST::Ok, "valid caps accepted");
        check(caps.supports_op(TensorOp::MatMul) && !caps.supports_op(TensorOp::Tanh),
              "op membership");
        check(caps.supports_dtype(DataType::FP32) && !caps.supports_dtype(DataType::INT8),
              "dtype membership");

        TensorCapabilities duplicated = caps;
        duplicated.supported_ops.push_back(TensorOp::Add);
        check(duplicated.validate(error) == ST::InvalidInput, "duplicate op refused");

        TensorCapabilities zero_limits = caps;
        zero_limits.max_elements = 0;
        check(zero_limits.validate(error) == ST::InvalidInput, "non-positive limits refused");

        // The acceleration claim defaults to NotClaimed and stays there.
        check(caps.matrix_acceleration == MatrixAcceleration::NotClaimed,
              "no hardware acceleration is claimed by default");
        check(std::string(to_string(MatrixAcceleration::NotClaimed)) == "not_claimed",
              "stable acceleration labels");

        // Requirements satisfaction: the pure compatibility decision.
        TensorRequirements req;
        req.required_ops = {TensorOp::MatMul};
        req.required_dtypes = {DataType::FP32};
        req.max_input_rank = 4;
        req.max_tensor_bytes = 2048;
        check(req.satisfied_by(caps), "satisfiable requirement");
        req.required_ops.push_back(TensorOp::Softmax);
        check(!req.satisfied_by(caps), "a missing op breaks satisfaction");
        req.required_ops = {TensorOp::MatMul};
        req.max_tensor_bytes = 8192;
        check(!req.satisfied_by(caps), "a byte bound beyond the target breaks satisfaction");

        // The reference backend's REAL capability table covers the full
        // surface it implements (the honest self-description).
        CpuReferenceTensorBackend reference;
        const TensorCapabilities& ref_caps = reference.capabilities();
        check(ref_caps.validate(error) == ST::Ok, "reference caps are internally valid");
        for (const TensorOp op : all_tensor_ops()) {
            check(ref_caps.supports_op(op), "reference implements every Phase 13 op");
        }
        check(ref_caps.supported_dtypes.size() == 5, "reference supports all five dtypes");
        check(ref_caps.matrix_acceleration == MatrixAcceleration::NotClaimed,
              "the reference claims NO acceleration");
    }

    // =====================================================================
    // 2. Deterministic dispatch: first satisfying backend wins
    // =====================================================================
    {
        vortyx::compute::Runtime runtime;
        check(runtime.initialize() == vortyx::compute::Status::Ok, "runtime initialized");

        TensorExecutor::Deps deps;
        deps.resources = manager.get();
        deps.runtime = &runtime;  // adapter FIRST in dispatch order
        std::unique_ptr<TensorExecutor> executor;
        std::string error;
        check(TensorExecutor::create(deps, executor, error) == ST::Ok, "executor with adapter");
        check(executor->backends().size() == 2, "adapter + reference registered");
        check(std::string(executor->backends()[0]->name()) == "tensor_runtime",
              "the runtime adapter is the dispatch head");
        check(std::string(executor->backends()[1]->name()) == "tensor_reference",
              "the reference kernel is the second choice");

        // int32 same-shape add: routes through the RUNTIME ADAPTER -> the
        // REAL Phase 10 engine (and therefore through the real CPU backend;
        // on a host with a working Vulkan device it would be the GPU path).
        const std::int32_t a[4] = {1, 2, 3, 4};
        const std::int32_t b[4] = {10, 20, 30, 40};
        Tensor ta;
        Tensor tb;
        check(Tensor::from_host(*manager, TensorShape::make({4}), DataType::INT32, a,
                                sizeof(a), ta, error) == ST::Ok, "int A");
        check(Tensor::from_host(*manager, TensorShape::make({4}), DataType::INT32, b,
                                sizeof(b), tb, error) == ST::Ok, "int B");
        Tensor sum;
        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {ta, tb}, sum, error),
                     ST::Ok, "int32 add through the runtime adapter");
        check(int32s(sum) == std::vector<std::int32_t>({11, 22, 33, 44}),
              "the adapter result matches the engine's semantics");

        // The engine identity: the same numbers through Runtime::execute
        // directly are identical (bit-exact, the Phase 10 guarantee).
        vortyx::compute::ComputeTask task;
        task.op = vortyx::compute::ComputeOp::VectorAdd;
        task.a.assign(a, a + 4);
        task.b.assign(b, b + 4);
        const vortyx::compute::ComputeTaskResult direct = runtime.execute(task);
        check(direct.status == vortyx::compute::Status::Ok &&
                  int32s(sum) == direct.data,
              "the tensor path and the engine path agree bit-exact");

        // int32 BROADCAST add: the adapter does not claim broadcast ->
        // capability dispatch selects the REFERENCE backend (not a fallback:
        // a capability-based selection). {2,2} matrix + {2} bias.
        const std::int32_t matrix[4] = {10, 20, 30, 40};
        Tensor tmatrix;
        check(Tensor::from_host(*manager, TensorShape::make({2, 2}), DataType::INT32, matrix,
                                sizeof(matrix), tmatrix, error) == ST::Ok, "matrix");
        const std::int32_t bias[2] = {100, 200};
        Tensor tbias;
        check(Tensor::from_host(*manager, TensorShape::make({2}), DataType::INT32, bias,
                                sizeof(bias), tbias, error) == ST::Ok, "bias");
        Tensor broadcast_sum;
        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {tmatrix, tbias},
                                          broadcast_sum, error),
                     ST::Ok, "broadcast add dispatches to the reference backend");
        check(broadcast_sum.shape() == TensorShape::make({2, 2}), "broadcast shape");
        check(int32s(broadcast_sum) == std::vector<std::int32_t>({110, 220, 130, 240}),
              "broadcast values via the reference kernel");

        // int32 MATMUL: the adapter does not claim it; the reference does not
        // either (float-only op) -> validate_op refuses BEFORE dispatch.
        Tensor refused;
        check_status(executor->execute_op(TensorOp::MatMul, TensorOpParams{}, {ta, ta},
                                          refused, error),
                     ST::UnsupportedDtype, "int32 matmul refused by the op rules");
    }

    // =====================================================================
    // 3. Unsupported operations and dtypes are precise refusals
    // =====================================================================
    {
        TensorExecutor::Deps deps;
        deps.resources = manager.get();
        std::unique_ptr<TensorExecutor> executor;
        std::string error;
        check(TensorExecutor::create(deps, executor, error) == ST::Ok, "reference-only executor");

        // A restricted external backend that supports ONLY fp32 relu: the
        // executor must refuse everything it cannot honestly run.
        struct ReluOnlyBackend final : public ITensorBackend {
            TensorCapabilities caps;
            ReluOnlyBackend() {
                caps.supported_ops = {TensorOp::Relu};
                caps.supported_dtypes = {DataType::FP32};
                caps.max_rank = 2;
                caps.max_elements = 16;
                caps.max_bytes = 64;
            }
            const char* name() const override { return "relu_only"; }
            const TensorCapabilities& capabilities() const override { return caps; }
            TensorStatus execute(TensorOpRequest& request, std::string& error) override {
                // A real (tiny) fp32 relu on the host — no fake success.
                std::vector<float> in(static_cast<std::size_t>(request.inputs[0].elements()));
                if (request.inputs[0].read_host(in.data(), in.size() * sizeof(float), error) !=
                    ST::Ok) {
                    return ST::ExecutionFailure;
                }
                for (float& x : in) x = x > 0.0f ? x : 0.0f;
                if (request.output.write_host(in.data(), in.size() * sizeof(float), error) !=
                    ST::Ok) {
                    return ST::ExecutionFailure;
                }
                return ST::Ok;
            }
        };
        ReluOnlyBackend relu_only;
        TensorExecutor::Deps restricted;
        restricted.resources = manager.get();
        restricted.external_backends = {&relu_only};
        restricted.include_reference = false;  // ONLY the restricted backend exists
        std::unique_ptr<TensorExecutor> restricted_executor;
        check(TensorExecutor::create(restricted, restricted_executor, error) == ST::Ok,
              "restricted executor created");
        check(restricted_executor->backends().size() == 1, "exactly the injected backend");

        const float v[2] = {1.0f, -2.0f};
        Tensor tv;
        check(Tensor::from_host(*manager, TensorShape::make({2}), DataType::FP32, v, sizeof(v),
                                tv, error) == ST::Ok, "float tensor");
        Tensor out;
        check_status(restricted_executor->execute_op(TensorOp::Relu, TensorOpParams{}, {tv},
                                                     out, error),
                     ST::Ok, "the restricted backend runs its one op");
        const std::vector<float> relu_result = floats_of(out);
        check(relu_result.size() == 2 && relu_result[0] == 1.0f && relu_result[1] == 0.0f,
              "the restricted backend computed a real relu");

        Tensor refused;
        check_status(restricted_executor->execute_op(TensorOp::Tanh, TensorOpParams{}, {tv},
                                                     refused, error),
                     ST::UnsupportedOperation, "unlisted op refused by dispatch");
        check(error.find("relu_only") != std::string::npos,
              "the failure names the backend and its table");

        const std::int32_t iv[2] = {1, 2};
        Tensor tiv;
        check(Tensor::from_host(*manager, TensorShape::make({2}), DataType::INT32, iv,
                                sizeof(iv), tiv, error) == ST::Ok, "int tensor");
        check_status(restricted_executor->execute_op(TensorOp::Relu, TensorOpParams{}, {tiv},
                                                     refused, error),
                     ST::UnsupportedDtype, "unlisted dtype refused by the op rules");

        // Null resources / uninitialized runtime refused at construction.
        TensorExecutor::Deps null_deps;
        std::unique_ptr<TensorExecutor> null_executor;
        check_status(TensorExecutor::create(null_deps, null_executor, error), ST::InvalidInput,
                     "null ResourceManager refused");
        vortyx::compute::Runtime uninitialized;
        TensorExecutor::Deps bad_runtime;
        bad_runtime.resources = manager.get();
        bad_runtime.runtime = &uninitialized;
        std::unique_ptr<TensorExecutor> bad_executor;
        check_status(TensorExecutor::create(bad_runtime, bad_executor, error),
                     ST::NotInitialized, "uninitialized runtime adapter refused");
    }

    // =====================================================================
    // 4. The error vocabulary: stable codes + the documented mapping
    // =====================================================================
    {
        // Codes are stable snake_case strings.
        check(std::string(tensor_status_code(ST::InvalidShape)) == "invalid_shape",
              "invalid_shape code");
        check(std::string(tensor_status_code(ST::UnsupportedDtype)) == "unsupported_dtype",
              "unsupported_dtype code");
        check(std::string(tensor_status_code(ST::DeviceCapabilityMismatch)) ==
                  "device_capability_mismatch",
              "device_capability_mismatch code");
        check(std::string(tensor_status_code(ST::TransferUnsupported)) ==
                  "transfer_unsupported",
              "transfer_unsupported code");

        // Round trip through the code table.
        ST parsed = ST::Ok;
        check(tensor_status_from_code("memory_allocation_failure", parsed) &&
                  parsed == ST::MemoryAllocationFailure,
              "code round trip");
        check(!tensor_status_from_code("not_a_real_code", parsed), "unknown code refused");

        // The documented compute-status mapping (the boundary contract).
        check(tensor_status_to_compute_status(ST::Ok) == vortyx::compute::Status::Ok, "map ok");
        check(tensor_status_to_compute_status(ST::InvalidShape) ==
                  vortyx::compute::Status::InvalidInput,
              "shape -> invalid_input");
        check(tensor_status_to_compute_status(ST::MemoryAllocationFailure) ==
                  vortyx::compute::Status::BackendError,
              "alloc -> backend_error");
        check(tensor_status_to_compute_status(ST::TransferUnsupported) ==
                  vortyx::compute::Status::BackendUnavailable,
              "transfer -> backend_unavailable");
        check(tensor_status_from_compute_status(vortyx::compute::Status::BackendUnavailable) ==
                  ST::DeviceCapabilityMismatch,
              "reverse map backend_unavailable");

        // Every enum value has a unique, non-empty code.
        const ST all[] = {ST::Ok,
                          ST::InvalidInput,
                          ST::InvalidShape,
                          ST::InvalidStride,
                          ST::DtypeMismatch,
                          ST::UnsupportedDtype,
                          ST::UnsupportedOperation,
                          ST::UnsupportedLayout,
                          ST::InvalidPlacement,
                          ST::ResourceLimitExceeded,
                          ST::MemoryAllocationFailure,
                          ST::InvalidState,
                          ST::DeviceCapabilityMismatch,
                          ST::TransferUnsupported,
                          ST::ExecutionFailure,
                          ST::NumericalValidationFailure,
                          ST::NotInitialized,
                          ST::Internal};
        for (std::size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
            for (std::size_t j = i + 1; j < sizeof(all) / sizeof(all[0]); ++j) {
                check(std::string(tensor_status_code(all[i])) !=
                          std::string(tensor_status_code(all[j])),
                      "codes are unique");
            }
        }
    }

    if (failures == 0) {
        std::cout << "Tensor dispatch tests passed.\n";
        return 0;
    }
    std::cerr << failures << " tensor dispatch test(s) failed.\n";
    return 1;
}
