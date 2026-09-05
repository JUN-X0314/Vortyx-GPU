#pragma once

// Tensor backends (Phase 13) — the kernel execution boundary.
//
//   TensorOp (logical)  ->  validated request  ->  ITensorBackend  ->  kernel
//
// Phase 13 ships exactly TWO backends, both honest about what they are:
//
//   1. CpuReferenceTensorBackend ("tensor_reference")
//      Deterministic host-memory reference kernels for the full Phase 13 op
//      set (see op.hpp). Computes on host bytes obtained through the Phase 4
//      resource system. FP16/BF16 execution is the documented
//      promote-compute-round semantics. MatMul iterates in a blocked
//      structure (tile = capability's preferred tile) with per-element
//      k-ascending accumulation — the blocked structure does NOT change any
//      output element's accumulation order, so results are tile-independent
//      and bit-deterministic. NO hardware acceleration is claimed anywhere.
//
//   2. RuntimeElementwiseTensorBackend ("tensor_runtime")
//      The bridge INTO the existing execution pipeline: int32 CONTIGUOUS
//      elementwise Add / Multiply are executed by the REAL Phase 10 engine
//      (vortyx::compute::Runtime::execute(ComputeTask)) — which means they
//      run on the real CPU backend AND on a real Vulkan GPU when the host
//      has one, with the engine's own bit-exact cross-backend semantics.
//      Everything else is refused with UnsupportedOperation (the backend
//      claims only what truly routes somewhere).
//
// Dispatch rule (select_backend): the FIRST backend in the executor's list
// whose capabilities satisfy the request wins — deterministic order, no
// scoring, no "probably fine". No backend is chosen for an op/dtype it does
// not list. If none matches, the executor reports
// DeviceCapabilityMismatch/UnsupportedOperation/UnsupportedDtype precisely.
//
// Backend threading: like a Runtime, a backend instance is externally
// serialized. Executors own their backends; no global mutable state exists.

#include <string>
#include <vector>

#include "core/compute/runtime.hpp"  // the existing engine (the adapter's target)
#include "tensor/capability.hpp"
#include "tensor/op.hpp"
#include "tensor/status.hpp"
#include "tensor/tensor_value.hpp"

namespace vortyx::tensor {

// One fully-validated execution request (the executor built it; the backend
// re-validates defensively — the same double-check pattern the Phase 4/10
// dispatch path uses).
struct TensorOpRequest {
    TensorOp op = TensorOp::Add;
    TensorOpParams params;
    std::vector<Tensor> inputs;   // validated, live
    Tensor output;                // allocated, contiguous, correct shape/dtype
};

class ITensorBackend {
public:
    virtual ~ITensorBackend() = default;

    ITensorBackend(const ITensorBackend&) = delete;
    ITensorBackend& operator=(const ITensorBackend&) = delete;

    // Stable backend name ("tensor_reference", "tensor_runtime").
    virtual const char* name() const = 0;

    // What this backend can honestly do (the dispatch vocabulary).
    virtual const TensorCapabilities& capabilities() const = 0;

    // Executes the request. The backend writes the result into
    // request.output's storage (the request is non-const for exactly that
    // reason — the same shape the Phase 4/10 dispatch uses). Must not throw;
    // failures are TensorStatus + error. The backend re-validates the
    // request against its own capabilities first (an op or dtype it does
    // not list is refused with UnsupportedOperation / UnsupportedDtype —
    // never executed "as a guess").
    virtual TensorStatus execute(TensorOpRequest& request, std::string& error) = 0;

protected:
    ITensorBackend() = default;
};

// ---------------------------------------------------------------------------
// Backend 1: the deterministic CPU reference implementation
// ---------------------------------------------------------------------------

class CpuReferenceTensorBackend final : public ITensorBackend {
public:
    CpuReferenceTensorBackend();

    const char* name() const override { return "tensor_reference"; }
    const TensorCapabilities& capabilities() const override { return capabilities_; }
    TensorStatus execute(TensorOpRequest& request, std::string& error) override;

private:
    TensorCapabilities capabilities_;
};

// ---------------------------------------------------------------------------
// Backend 2: the existing-execution-pipeline adapter (int32 elementwise)
// ---------------------------------------------------------------------------

// 'runtime' must be an initialized vortyx::compute::Runtime and must outlive
// this backend. The backend is an adapter: it owns nothing of the Runtime.
class RuntimeElementwiseTensorBackend final : public ITensorBackend {
public:
    explicit RuntimeElementwiseTensorBackend(vortyx::compute::Runtime& runtime);

    const char* name() const override { return "tensor_runtime"; }
    const TensorCapabilities& capabilities() const override { return capabilities_; }
    TensorStatus execute(TensorOpRequest& request, std::string& error) override;

private:
    TensorCapabilities capabilities_;
    vortyx::compute::Runtime* runtime_;
};

// Deterministic backend selection (see the module header). Returns nullptr
// when no backend satisfies the requirements — the caller turns that into
// the precise failure via describe_dispatch_failure().
ITensorBackend* select_backend(const std::vector<ITensorBackend*>& backends,
                               const TensorRequirements& requirements);

// Human-readable reason for a failed dispatch (used by the executor to fill
// 'error' with the precise vocabulary: which backend lists what).
std::string describe_dispatch_failure(const std::vector<ITensorBackend*>& backends,
                                      TensorOp op, DataType dtype);

}  // namespace vortyx::tensor
