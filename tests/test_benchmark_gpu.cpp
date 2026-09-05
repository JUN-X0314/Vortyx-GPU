// Benchmark GPU-path tests (Phase 8).
//
// Design rules (per project requirements):
//  - When the Vulkan backend is unavailable, the test exits successfully
//    with an explicit, visible note saying it did NOT run. A GPU benchmark
//    is never faked and the SKIP is never reported as a success.
//  - When the backend IS available, the benchmark must be proven to run the
//    REAL Vulkan execution path: the measured backend is 'vulkan', the
//    results are bit-exact against an independently executed CPU reference,
//    the statistics hold their structural invariants, and the reported
//    device is the backend's own honest DeviceInfo (a software Vulkan
//    implementation such as lavapipe is reported as SoftwareGpu, never
//    relabeled as hardware).
//  - No timing-value assertions anywhere (invariants only).

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "core/benchmark/benchmark.hpp"
#include "core/compute/task.hpp"
#include "core/device/device.hpp"
#include "core/vgpu/virtual_gpu.hpp"

using vortyx::benchmark::BenchmarkConfig;
using vortyx::benchmark::BenchmarkResult;
using vortyx::benchmark::benchmark_vector_add;
using vortyx::compute::Status;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;
using vortyx::vgpu::VirtualGpu;
using vortyx::vgpu::VirtualGpuDesc;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

VectorAddTask make_task(std::size_t count) {
    VectorAddTask task;
    task.a.resize(count);
    task.b.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        // Deterministic values well inside int32 range (no overflow).
        task.a[i] = static_cast<std::int32_t>(i % 1000) - 300;
        task.b[i] = static_cast<std::int32_t>((i * 7) % 500) + 11;
    }
    return task;
}

bool data_matches(const std::vector<std::int32_t>& data, const VectorAddTask& task) {
    if (data.size() != task.a.size()) return false;
    for (std::size_t i = 0; i < task.a.size(); ++i) {
        if (data[i] != task.a[i] + task.b[i]) return false;
    }
    return true;
}

}  // namespace

int main() {
    // The reference CPU Virtual GPU exists in every environment (created
    // first so every GPU benchmark below is verified against a real,
    // independently executed CPU result of the same task).
    VirtualGpu cpu_gpu;
    VirtualGpuDesc cpu_desc;
    cpu_desc.backend = "cpu";
    if (cpu_gpu.initialize(cpu_desc) != Status::Ok) {
        std::cerr << "FAIL: cpu VirtualGpu initialize() failed\n";
        return 1;
    }

    // An independent Vulkan Virtual GPU probe decides whether this machine
    // can run the GPU-path assertions at all.
    VirtualGpu probe;
    VirtualGpuDesc probe_desc;
    probe_desc.backend = "vulkan";
    if (probe.initialize(probe_desc) != Status::Ok) {
        std::cerr << "FAIL: vulkan VirtualGpu probe initialize() must not fail for a known "
                     "backend\n";
        return 1;
    }

    if (!probe.backend_available()) {
        // Environment without a usable Vulkan device: a normal, non-fatal
        // condition. Report it clearly; do NOT fake a GPU benchmark.
        std::cout << "SKIPPED (environment): Vulkan backend unavailable - "
                  << probe.backend_unavailable_reason() << "\n";
        std::cout << "Note: GPU benchmarks were NOT run on this machine. The benchmark "
                     "CPU path is covered by BenchmarkTest.\n";
        probe.shutdown();
        cpu_gpu.shutdown();
        return 0;
    }

    // =====================================================================
    // The Vulkan backend IS available: benchmark the real device.
    // =====================================================================
    probe.shutdown();

    {
        VirtualGpu gpu;
        VirtualGpuDesc desc;
        desc.backend = "vulkan";
        check(gpu.initialize(desc) == Status::Ok, "vulkan Virtual GPU initializes");

        // --- 1. Real-path benchmark on the Vulkan backend ----------------
        const VectorAddTask task = make_task(4096);
        BenchmarkConfig config;
        config.iterations = 10;
        config.warmup_iterations = 2;

        const BenchmarkResult r = benchmark_vector_add(gpu, task, config);
        check(r.status == Status::Ok, "1: Vulkan benchmark succeeds on the real device");
        if (r.status != Status::Ok) {
            std::cerr << "  reason: " << r.error << "\n";
        } else {
            check(r.backend == "vulkan", "1: measured backend is the requested 'vulkan'");
            check(r.element_count == task.a.size(), "1: element count matches the task");
            check(r.iterations == 10, "1: measured iteration count matches the request");
            check(r.correctness_verified, "1: correctness verdict present (all iterations verified)");
            check(r.timing.max >= r.timing.min, "1: max >= min");
            check(r.timing.average_ns >= static_cast<double>(r.timing.min.count()) &&
                      r.timing.average_ns <= static_cast<double>(r.timing.max.count()),
                  "1: min <= average <= max");
            check(r.timing.max.count() > 0, "1: samples are real time, not zeros");
            // The honest device report: a software Vulkan implementation
            // must stay SoftwareGpu, hardware stays Gpu — whatever the
            // backend itself reports, never relabeled.
            check(r.device.type == gpu.device_info().type,
                  "1: device type matches the backend's own report");
            check(r.device.name == gpu.device_info().name,
                  "1: device name matches the backend's own report");

            // --- 2. Bit-exact cross-check against the CPU path ----------
            const VectorAddResult cpu_result = cpu_gpu.execute(task);
            check(cpu_result.status == Status::Ok && data_matches(cpu_result.data, task),
                  "2: independent CPU reference is itself correct");

            // The benchmark verified the same correctness property; run one
            // more measured execution and compare against the CPU result.
            const VectorAddResult gpu_result = gpu.execute(task);
            check(gpu_result.status == Status::Ok, "2: post-benchmark GPU execution succeeds");
            check(cpu_result.data == gpu_result.data,
                  "2: GPU path result is bit-exact with the CPU path result");

            // --- 3. Repeated benchmark: deterministic verdict ----------
            const BenchmarkResult again = benchmark_vector_add(gpu, task, config);
            check(again.status == Status::Ok && again.correctness_verified,
                  "3: repeated Vulkan benchmark verifies again");
            check(again.backend == "vulkan", "3: repeated run reports the same backend");
            // Timing values are intentionally NOT compared between runs.

            // --- 4. Machine-readable export -----------------------------
            const auto kv = vortyx::benchmark::to_key_values(r);
            bool has_backend = false, has_throughput = false;
            for (const auto& pair : kv) {
                if (pair.first == "backend" && pair.second == "vulkan") has_backend = true;
                if (pair.first == "throughput_elements_per_second") has_throughput = true;
            }
            check(has_backend, "4: export names the vulkan backend");
            check(has_throughput, "4: export carries the throughput");
        }

        gpu.shutdown();
    }

    cpu_gpu.shutdown();

    if (failures == 0) {
        std::cout << "Benchmark GPU-path tests passed.\n";
        return 0;
    }
    std::cerr << failures << " benchmark GPU-path check(s) FAILED.\n";
    return 1;
}
