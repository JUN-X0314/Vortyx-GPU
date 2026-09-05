#pragma once

// CPU compute backend (Phase 4/10).
// Reference implementation for the elementwise compute engine: deterministic,
// always available, and the correctness baseline the GPU backend is checked
// against.
//
// Phase 4: the backend computes directly on Buffer resources created through
// the Resource Manager (host-memory buffers). Task->buffer translation lives
// in the Runtime; the backend itself never allocates scratch memory.
//
// Phase 10 (Compute Engine):
//   - Implements the generic buffer-level dispatch (ComputeDispatch) for the
//     elementwise int32 ops: VectorAdd, VectorMultiply, VectorScale. The
//     Phase 4 execute(a, b, c) signature is unchanged and now routes through
//     the same dispatch as its VectorAdd specialization — the compute loops
//     exist exactly once.
//   - PARALLEL EXECUTION POLICY (documented, honest):
//       * The ops are elementwise: every output element depends only on the
//         inputs at the same index, so partitioning the index range cannot
//         change a single bit of the result. Parallel and sequential
//         execution are bit-exact identical by construction (pinned by
//         tests), which is what makes parallelism safe here.
//       * Workloads below kParallelThreshold elements run SEQUENTIALLY:
//         for small workloads the thread setup/join overhead can make
//         parallel execution SLOWER, not faster — that possibility is
//         explicit policy here, not something to hide. Actual speedups are
//         measurements for the benchmark tool, never assumptions.
//       * Worker count: min(std::thread::hardware_concurrency(),
//         kMaxCpuWorkers); hardware_concurrency() == 0 (unknown) means 1.
//       * Threads are fork-join per dispatch: the calling thread
//         participates, workers are joined before execute() returns. No
//         persistent pool, no shared mutable state, nothing to shut down —
//         the Runtime's external-serialization contract is unaffected.
//       * If a worker thread cannot be created (resource exhaustion), the
//         affected range runs on the calling thread instead. The result is
//         identical either way; only the speed differs. This is an internal
//         execution detail, never a backend semantic change.

#include "core/compute/backend.hpp"
#include "core/resource/cpu_buffer.hpp"

namespace vortyx::compute {

class CpuBackend final : public IComputeBackend {
public:
    // Workloads with fewer elements than this always run sequentially.
    // Measurement hook (A/B harness): builds configured with
    // VORTYX_CPU_FORCE_SEQUENTIAL=ON compile the engine sequential-only so
    // parallel-vs-sequential can be measured on the SAME machine with the
    // SAME ladder (reproducible before/after numbers). Correctness is
    // identical in both configurations and pinned by the test suite.
#if defined(VORTYX_CPU_FORCE_SEQUENTIAL)
    static constexpr std::size_t kParallelThreshold = ~std::size_t{0};
#else
    static constexpr std::size_t kParallelThreshold = 1u << 16;  // 65536 elements
#endif

    // Upper bound for fork-join workers (avoids oversubscription on very
    // large machines; the calling thread participates in addition).
    static constexpr unsigned kMaxCpuWorkers = 8;

    CpuBackend() = default;

    const char* name() const override { return "cpu"; }
    bool available() const override { return true; }
    std::string unavailable_reason() const override { return {}; }

    vortyx::device::DeviceInfo device_info() const override;

    // Buffer-based execution: a, b, c must be CpuBuffer resources. Reads the
    // inputs' host storage, writes the sum into the output's host storage.
    // (Phase 4 contract, unchanged; routed through the generic dispatch.)
    ComputeResult execute(const vortyx::resource::IBufferImpl& a,
                          const vortyx::resource::IBufferImpl& b,
                          vortyx::resource::IBufferImpl& c) override;

    // Generic dispatch (Phase 10): VectorAdd / VectorMultiply / VectorScale
    // over CpuBuffer resources, with the parallel policy documented above.
    ComputeResult execute(const ComputeDispatch& dispatch) override;
};

}  // namespace vortyx::compute
