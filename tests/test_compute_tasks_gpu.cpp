// Compute Engine tests (Phase 10) — GPU path.
//
// These tests run FOR REAL when a Vulkan device is available (a software
// implementation like lavapipe counts — it is a real Vulkan backend and is
// reported honestly as a SoftwareGpu). On systems without one they exit with
// an explicit SKIP note — never a faked success.
//
// What they verify:
//   - every op executes on the real Vulkan path and matches the host
//     reference bit-exactly (int32 arithmetic is exact modulo 2^32 on GPU
//     and CPU alike — overflow included),
//   - cross-backend consistency: Vulkan and an independent CPU Virtual GPU
//     produce identical results for the same tasks, bit for bit,
//   - the VectorScale dispatch path (placeholder descriptor binding),
//   - batch execution through the Vulkan-configured Virtual GPU,
//   - repeated-execution determinism,
//   - no-leak accounting through the Virtual GPU's ResourceManager.

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "core/compute/task.hpp"
#include "core/resource/resource_manager.hpp"
#include "core/vgpu/virtual_gpu.hpp"

using vortyx::compute::BatchResult;
using vortyx::compute::ComputeOp;
using vortyx::compute::ComputeTask;
using vortyx::compute::ComputeTaskResult;
using vortyx::compute::Status;
using vortyx::compute::to_string;
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

std::vector<std::int32_t> host_reference(const ComputeTask& task) {
    std::vector<std::int32_t> expected(task.a.size());
    switch (task.op) {
        case ComputeOp::VectorAdd:
            for (std::size_t i = 0; i < task.a.size(); ++i) expected[i] = task.a[i] + task.b[i];
            break;
        case ComputeOp::VectorMultiply:
            for (std::size_t i = 0; i < task.a.size(); ++i)
                expected[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(task.a[i]) *
                                                        static_cast<std::uint32_t>(task.b[i]));
            break;
        case ComputeOp::VectorScale:
            for (std::size_t i = 0; i < task.a.size(); ++i)
                expected[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(task.a[i]) *
                                                        static_cast<std::uint32_t>(task.scalar));
            break;
    }
    return expected;
}

bool matches(const ComputeTaskResult& result, const std::vector<std::int32_t>& expected) {
    return result.status == Status::Ok && result.data == expected;
}

ComputeTask make_task(ComputeOp op, std::size_t count) {
    ComputeTask task;
    task.op = op;
    task.a.resize(count);
    if (op != ComputeOp::VectorScale) task.b.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        task.a[i] = static_cast<std::int32_t>(i % 1000) - 300;
        if (op != ComputeOp::VectorScale) {
            task.b[i] = static_cast<std::int32_t>((i * 7) % 500) + 11;
        }
    }
    if (op == ComputeOp::VectorScale) task.scalar = -9;
    return task;
}

}  // namespace

int main() {
    // Probe the REAL availability through an independent Virtual GPU.
    {
        VirtualGpu probe;
        VirtualGpuDesc desc;
        desc.backend = "vulkan";
        if (probe.initialize(desc) != Status::Ok) {
            std::cerr << "FAIL: vulkan VirtualGpu probe initialize() must not fail for a "
                         "known backend\n";
            return 1;
        }
        const bool available = probe.backend_available();
        const std::string reason =
            available ? std::string() : probe.backend_unavailable_reason();
        probe.shutdown();

        if (!available) {
            std::cout << "SKIP: GPU Compute Engine tests require a Vulkan device. "
                      << "This machine reports: " << reason << "\n"
                      << "The CPU path is covered by test_compute_tasks.\n";
            return 0;  // honest skip — never a faked success
        }
    }

    VirtualGpu gpu;
    VirtualGpuDesc desc;
    desc.backend = "vulkan";
    check(gpu.initialize(desc) == Status::Ok, "vulkan VirtualGpu must initialize");
    check(gpu.backend_available(), "the probed device must still be available");

    // =====================================================================
    // 1. Every op executes on the real Vulkan path, bit-exact vs the host
    //    reference, at several sizes.
    // =====================================================================
    const std::size_t sizes[] = {1, 64, 1000, 5000};
    for (const ComputeOp op : {ComputeOp::VectorAdd, ComputeOp::VectorMultiply,
                               ComputeOp::VectorScale}) {
        for (const std::size_t count : sizes) {
            ComputeTask task = make_task(op, count);
            const ComputeTaskResult r = gpu.execute(task);
            check(matches(r, host_reference(task)),
                  "op=" + std::string(to_string(op)) + " count=" + std::to_string(count) +
                      " must match the host reference bit-exactly (status=" +
                      to_string(r.status) + (r.error.empty() ? "" : ": " + r.error) + ")");
        }
    }

    // =====================================================================
    // 2. Modular int32 semantics on the GPU: overflow wraps identically.
    // =====================================================================
    {
        ComputeTask mul;
        mul.op = ComputeOp::VectorMultiply;
        mul.a = {std::numeric_limits<std::int32_t>::max(), -1,
                 std::numeric_limits<std::int32_t>::min() / 2};
        mul.b = {2, std::numeric_limits<std::int32_t>::min(), 2};
        const ComputeTaskResult r = gpu.execute(mul);
        check(r.status == Status::Ok && r.data.size() == 3 && r.data[0] == -2 &&
                  r.data[1] == std::numeric_limits<std::int32_t>::min() &&
                  r.data[2] == std::numeric_limits<std::int32_t>::min(),
              "GPU multiply wraps modulo 2^32 exactly like the CPU baseline");
    }

    // =====================================================================
    // 3. Cross-backend consistency: the same tasks through an independent
    //    CPU Virtual GPU produce bit-identical results.
    // =====================================================================
    {
        VirtualGpu cpu_gpu;  // explicit cpu
        check(cpu_gpu.initialize() == Status::Ok, "independent cpu VirtualGpu initializes");
        for (const ComputeOp op : {ComputeOp::VectorAdd, ComputeOp::VectorMultiply,
                                   ComputeOp::VectorScale}) {
            ComputeTask task = make_task(op, 777);
            const ComputeTaskResult via_gpu = gpu.execute(task);
            const ComputeTaskResult via_cpu = cpu_gpu.execute(task);
            check(via_gpu.status == Status::Ok && via_cpu.status == Status::Ok &&
                      via_gpu.data == via_cpu.data,
                  "vulkan and cpu results are bit-identical (op=" + std::string(to_string(op)) +
                      ")");
        }
        cpu_gpu.shutdown();
    }

    // =====================================================================
    // 4. Batch through the Vulkan-configured Virtual GPU.
    // =====================================================================
    {
        std::vector<ComputeTask> tasks;
        tasks.push_back(make_task(ComputeOp::VectorAdd, 128));
        tasks.push_back(make_task(ComputeOp::VectorMultiply, 128));
        tasks.push_back(make_task(ComputeOp::VectorScale, 128));
        const BatchResult batch = gpu.execute_batch(tasks);
        check(batch.status == Status::Ok && batch.succeeded == 3 && batch.failed == 0,
              "batch on the real device succeeds");
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            check(matches(batch.results[i], host_reference(tasks[i])),
                  "batch item " + std::to_string(i) + " matches its reference");
        }
    }

    // =====================================================================
    // 5. Repeated execution is deterministic on the GPU path.
    // =====================================================================
    {
        ComputeTask task = make_task(ComputeOp::VectorMultiply, 4096);
        const ComputeTaskResult first = gpu.execute(task);
        const ComputeTaskResult second = gpu.execute(task);
        check(first.status == Status::Ok && first.data == second.data,
              "repeated GPU execution is deterministic");
    }

    // =====================================================================
    // 6. No-leak accounting: the engine released every per-call buffer.
    // =====================================================================
    {
        const vortyx::resource::ResourceStats stats = gpu.resources()->stats();
        check(stats.live_buffers == 0,
              "no live buffers may survive the test (RAII per execution)");
    }

    gpu.shutdown();

    if (failures == 0) {
        std::cout << "Compute Engine GPU-path tests passed.\n";
        return 0;
    }
    std::cerr << failures << " compute engine GPU-path check(s) FAILED.\n";
    return 1;
}
