#pragma once

// TensorExecutor (Phase 13) — the single-op execution facade.
//
//   validate_op (the one rule set) -> allocate output (Phase 4 resources)
//   -> capability dispatch -> backend kernel -> filled output
//
// Responsibilities (exactly these):
//   - validate the request through validate_op (planning and execution share
//     the same rules — they cannot drift);
//   - enforce the PLACEMENT rules (all device-placed inputs must target the
//     same place; a request needing cross-device data movement is refused
//     with TransferUnsupported — Phase 13 has no transfer and never fakes
//     one);
//   - MATERIALIZES strided input views as contiguous copies (an explicit,
//     documented step — kernels never see strides they did not declare);
//   - dispatch to the FIRST backend whose capabilities satisfy the request
//     (deterministic; broadcast support is part of the capability match);
//   - allocate the output tensor through the caller's ResourceManager (the
//     Phase 4 memory system — the only allocation path in the tensor layer).
//
// NOT its job: scheduling, graphs (see graph_executor), device selection
// across a cluster (see placement_integration), timing (nothing here measures
// or fabricates performance).
//
// Threading: externally serialized, like a Runtime. The executor owns its
// backends; no global mutable state exists anywhere in the tensor layer.

#include <memory>
#include <string>
#include <vector>

#include "tensor/backend.hpp"
#include "tensor/op.hpp"
#include "tensor/status.hpp"
#include "tensor/tensor_value.hpp"

namespace vortyx::tensor {

class TensorExecutor {
public:
    struct Deps {
        // Required: every allocation flows through this manager (the Phase 4
        // memory system). Must outlive the executor.
        vortyx::resource::ResourceManager* resources = nullptr;

        // Optional: when set, the executor includes the runtime adapter
        // backend (int32 elementwise routed through the REAL engine) in
        // front of the reference backend. The runtime must be initialized
        // and must outlive the executor.
        vortyx::compute::Runtime* runtime = nullptr;

        // Optional: extra caller-provided backends, tried in order BEFORE
        // the built-ins (a future custom backend plugs in here).
        std::vector<ITensorBackend*> external_backends;

        // When false, the built-in reference backend is NOT appended — the
        // executor then runs ONLY on the explicitly provided backends (the
        // honest way to build a restricted executor; dispatch failures are
        // precise refusals, never silent fallbacks to the reference).
        bool include_reference = true;
    };

    // Builds an executor. Fails (InvalidInput) when 'deps.resources' is
    // null; a runtime adapter without an initialized runtime is refused
    // (NotInitialized) — never silently dropped.
    static TensorStatus create(const Deps& deps, std::unique_ptr<TensorExecutor>& out,
                               std::string& error);

    ~TensorExecutor();

    TensorExecutor(const TensorExecutor&) = delete;
    TensorExecutor& operator=(const TensorExecutor&) = delete;

    // The registered backends in dispatch order (observability).
    std::vector<ITensorBackend*> backends() const;

    // The resource manager every allocation flows through (the Phase 4
    // memory system; the same instance graph executors use for slot
    // storage — one allocation path, one accounting world).
    vortyx::resource::ResourceManager& resources() const { return *resources_; }

    // Executes ONE operation. Returns Ok with 'out' filled (the allocated,
    // computed output tensor), or the precise failure. The output's
    // placement follows the inputs: the common device placement when any
    // input is device-placed, Host otherwise.
    TensorStatus execute_op(TensorOp op, const TensorOpParams& params,
                            const std::vector<Tensor>& inputs, Tensor& out,
                            std::string& error);

    // Executes ONE operation INTO a caller-provided output tensor (the
    // graph executor's slot path: the memory plan owns allocation, so the
    // executor must not allocate a second buffer). 'provided' must already
    // be allocated with the op's exact inferred output shape/dtype and be
    // contiguous. Validation and dispatch are identical to execute_op.
    TensorStatus execute_op_into(TensorOp op, const TensorOpParams& params,
                                 const std::vector<Tensor>& inputs, Tensor& provided,
                                 std::string& error);

private:
    TensorExecutor() = default;

    // Shared body: when 'provided' is non-null it is used as the output
    // (shape/dtype re-verified); otherwise a fresh output is allocated.
    TensorStatus execute_internal(TensorOp op, const TensorOpParams& params,
                                  const std::vector<Tensor>& inputs, Tensor* provided,
                                  Tensor& out, std::string& error);

    // Materializes 'tensor' when it is not row-major contiguous; returns a
    // contiguous tensor (either the input itself or a fresh copy).
    TensorStatus materialize_contiguous(const Tensor& tensor, Tensor& out, std::string& error);

    vortyx::resource::ResourceManager* resources_ = nullptr;
    std::vector<std::unique_ptr<ITensorBackend>> owned_backends_;
    std::vector<ITensorBackend*> dispatch_order_;  // non-owning view, dispatch order
};

}  // namespace vortyx::tensor
