// vortyx_bench — the Phase 8/10 standalone benchmark tool.
//
// Measures the REAL Vortyx compute path (Virtual GPU -> Runtime ->
// Resource Manager -> Backend) over a fixed ladder of workload sizes, on
// the CPU backend always and on the Vulkan backend when a device is really
// available on this system. Phase 10 extends the ladder to every ComputeOp
// the engine supports (vector_add, vector_multiply, vector_scale) — each
// measured and verified independently, never collapsed into one number.
//
// Results are printed in the human-readable form; the machine-readable
// key=value export of the same real numbers follows each result.
//
// Scope of every reported number (documented, identical for all backends
// and ops): one sample = one VirtualGpu::execute(task) call — that is
// allocation + upload + execution + readback + release, end to end, CPU
// wall-clock (steady_clock). The Runtime API does not expose GPU-internal
// execution boundaries, so nothing here is labeled "GPU time". Setup
// (initialization, input construction, reference computation) is outside
// the timed window.
//
// This tool is a measurement utility, not a pass/fail test: the timing
// numbers are real measurements that vary by machine, and no output of
// this tool should be read as a performance claim (numbers from DIFFERENT
// operations are labeled separately and are never compared as "better").
// Run the CTest suite for the deterministic benchmark invariants instead.
//
// Exit code: 0 when every benchmark that RAN verified its correctness;
// 1 when a benchmark failed unexpectedly. A Vulkan benchmark that cannot
// run (no device) is an explicitly reported SKIP, not a failure.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "core/benchmark/benchmark.hpp"
#include "core/compute/task.hpp"
#include "core/device/device.hpp"
#include "core/version.hpp"
#include "core/vgpu/virtual_gpu.hpp"

#ifndef VORTYX_BUILD_CONFIG
#define VORTYX_BUILD_CONFIG "Unknown"
#endif

namespace {

using vortyx::benchmark::BenchmarkConfig;
using vortyx::benchmark::BenchmarkResult;
using vortyx::compute::ComputeOp;
using vortyx::compute::ComputeTask;

// Deterministic inputs over safe int32 ranges for the two-input ops. The
// ops use int32 modular arithmetic (bit-exact on every backend), so the
// verification reference is exact regardless of ranges; these ranges simply
// keep the ladder readable and far away from accidental overflow surprises.
ComputeTask make_task(ComputeOp op, std::size_t count) {
    ComputeTask task;
    task.op = op;
    task.a.resize(count);
    if (op != ComputeOp::VectorScale) {
        task.b.resize(count);
    }
    for (std::size_t i = 0; i < count; ++i) {
        task.a[i] = static_cast<std::int32_t>(i % 1000) - 300;
        if (op != ComputeOp::VectorScale) {
            task.b[i] = static_cast<std::int32_t>((i * 7) % 500) + 11;
        }
    }
    if (op == ComputeOp::VectorScale) {
        task.scalar = -7;  // fixed deterministic scale factor
    }
    return task;
}

// The measurement ladder. Small enough to keep the tool quick even on the
// largest size (1M elements = 4 MiB per input, ~3 live buffers — far below
// the Phase 4 per-buffer safety cap of 1 GiB), large enough to make
// per-iteration cost visible above noise. Iterations are modest for the
// same reason; raise them locally when you want tighter statistics.
struct LadderStep {
    std::size_t element_count;
    std::uint32_t iterations;
};

const LadderStep kLadder[] = {
    {1024, 50},
    {16 * 1024, 30},
    {256 * 1024, 15},
    {1024 * 1024, 10},
};

const ComputeOp kOps[] = {
    ComputeOp::VectorAdd,
    ComputeOp::VectorMultiply,
    ComputeOp::VectorScale,
};

// Runs the whole ladder (every op x every size) on one backend. Returns
// true when every benchmark that ran verified correctness; reports skips
// and failures honestly.
bool run_ladder(const std::string& backend) {
    vortyx::vgpu::VirtualGpuDesc desc;
    desc.backend = backend;
    vortyx::vgpu::VirtualGpu gpu;

    // A known-but-unavailable backend still initializes (Phase 5 rule); the
    // honest answer is backend_available().
    if (gpu.initialize(desc) != vortyx::compute::Status::Ok) {
        std::cout << "Virtual GPU (" << backend << ") failed to initialize: "
                  << gpu.backend_unavailable_reason() << "\n";
        return false;
    }
    if (!gpu.backend_available()) {
        std::cout << "Backend '" << backend << "' is not usable on this system: "
                  << gpu.backend_unavailable_reason() << "\n";
        std::cout << "  -> SKIP (" << backend << " benchmarks; no fallback was attempted)\n";
        gpu.shutdown();
        return true;  // a skip is not a failure
    }

    std::cout << "Backend '" << backend << "' device: "
              << (gpu.device_info().name.empty() ? std::string("unknown")
                                                 : gpu.device_info().name)
              << "\n";

    bool all_ok = true;
    for (const ComputeOp op : kOps) {
        for (const LadderStep& step : kLadder) {
            const ComputeTask task = make_task(op, step.element_count);
            BenchmarkConfig config;
            config.iterations = step.iterations;
            config.warmup_iterations = 2;

            const BenchmarkResult result = vortyx::benchmark::benchmark_compute(gpu, task, config);

            std::cout << vortyx::benchmark::describe(result) << "\n";
            for (const auto& kv : vortyx::benchmark::to_key_values(result)) {
                std::cout << "  " << kv.first << "=" << kv.second << "\n";
            }

            if (result.status != vortyx::compute::Status::Ok || !result.correctness_verified) {
                all_ok = false;
                std::cout << "  -> FAILED (see status/error above)\n";
            }
        }
    }

    gpu.shutdown();
    return all_ok;
}

}  // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "  Vortyx GPU benchmark tool\n";
    std::cout << "  Version: " << VORTYX_VERSION_STRING << "\n";
    std::cout << "  Build:   " << VORTYX_BUILD_CONFIG << "\n";
    std::cout << "========================================\n";
    std::cout << "Measured scope per iteration: VirtualGpu::execute(task) end to end\n"
              << "(allocation + upload + execution + readback + release), CPU wall-clock.\n"
              << "Ops are measured and labeled separately; timings vary by machine;\n"
              << "numbers are measurements, not claims.\n\n";

    bool ok = true;
    ok = run_ladder("cpu") && ok;      // every system
    ok = run_ladder("vulkan") && ok;   // runs for real only with a device; SKIP otherwise

    std::cout << (ok ? "Benchmark run complete: all executed benchmarks verified.\n"
                     : "Benchmark run FAILED: at least one benchmark did not verify.\n");
    return ok ? 0 : 1;
}
