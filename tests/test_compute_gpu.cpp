// GPU compute path tests (Phase 3).
//
// Design rules (per project requirements):
//  - No hardcoded hardware expectations (no "there must be exactly 1 GPU",
//    no vendor names). Tests pass on AMD, Intel, NVIDIA, software Vulkan
//    implementations (lavapipe/llvmpipe) and on machines with NO GPU at all.
//  - When the Vulkan backend is unavailable, the test exits successfully
//    with an explicit, visible note saying it did NOT run. It is never
//    reported as a GPU execution success.
//  - When the backend IS available, real vector additions run on the device
//    and must match the CPU reference bit-exactly.

#include <iostream>
#include <string>
#include <vector>

#include "core/compute/task.hpp"
#include "core/compute/runtime.hpp"
#include "core/device/device.hpp"

using vortyx::compute::Runtime;
using vortyx::compute::Status;
using vortyx::compute::to_string;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;

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

bool result_matches(const VectorAddResult& result, const VectorAddTask& task) {
    if (result.status != Status::Ok) return false;
    if (result.data.size() != task.a.size()) return false;
    for (std::size_t i = 0; i < task.a.size(); ++i) {
        if (result.data[i] != task.a[i] + task.b[i]) return false;
    }
    return true;
}

}  // namespace

int main() {
    Runtime runtime;
    if (runtime.initialize() != Status::Ok) {
        std::cerr << "FAIL: runtime initialize() failed\n";
        return 1;
    }

    if (!runtime.has_backend("vulkan")) {
        // Environment without a usable Vulkan device: this is a normal,
        // non-fatal condition. Report it clearly; do NOT fake success.
        std::cout << "SKIPPED (environment): Vulkan backend unavailable - "
                  << runtime.backend_unavailable_reason("vulkan") << "\n";
        std::cout << "Note: GPU execution was NOT tested on this machine. "
                  << "CPU path is covered by test_compute_cpu.\n";
        runtime.shutdown();
        return 0;
    }

    // --- Backend is available: run REAL GPU tests ---------------------------
    const vortyx::device::DeviceInfo device = runtime.backend_device("vulkan");
    std::cout << "Vulkan backend available: "
              << (device.name.empty() ? std::string("unknown device") : device.name)
              << " | vendor: " << (device.vendor.empty() ? "unknown" : device.vendor) << "\n";
    check(!device.name.empty(), "available Vulkan device must report its name");
    check(device.type == vortyx::device::DeviceType::Gpu ||
              device.type == vortyx::device::DeviceType::SoftwareGpu,
          "Vulkan device must be typed Gpu or SoftwareGpu (never Unknown)");

    // Correctness across several sizes, including sizes that are not a
    // multiple of the 64-element workgroup (guards the shader bounds check).
    const std::vector<std::size_t> sizes = {4, 16, 64, 1024, 5000};
    for (std::size_t size : sizes) {
        const VectorAddTask task = make_task(size);

        const VectorAddResult cpu_ref = runtime.execute(task, "cpu");
        check(cpu_ref.status == Status::Ok,
              "cpu reference must succeed for size " + std::to_string(size));

        const VectorAddResult gpu_result = runtime.execute(task, "vulkan");
        check(gpu_result.status == Status::Ok,
              "vulkan execution must succeed for size " + std::to_string(size) +
                  " (error: " + gpu_result.error + ")");
        check(result_matches(gpu_result, task),
              "gpu result must equal A+B for size " + std::to_string(size));
        check(gpu_result.data == cpu_ref.data,
              "gpu result must match cpu reference exactly for size " +
                  std::to_string(size));
    }

    // Repeated execution determinism (same input -> same output, 3 runs).
    {
        const VectorAddTask task = make_task(1024);
        const VectorAddResult first = runtime.execute(task, "vulkan");
        check(first.status == Status::Ok, "repeat run 1 must succeed");
        for (int run = 2; run <= 3; ++run) {
            const VectorAddResult next = runtime.execute(task, "vulkan");
            check(next.status == Status::Ok, "repeat run must succeed");
            check(next.data == first.data, "repeated GPU runs must be deterministic");
        }
    }

    // Invalid input must be rejected through the GPU path too.
    {
        VectorAddTask mismatch;
        mismatch.a = {1, 2, 3};
        mismatch.b = {1, 2};
        const VectorAddResult r = runtime.execute(mismatch, "vulkan");
        check(r.status == Status::InvalidInput,
              "mismatched sizes must return InvalidInput on the GPU path");
    }

    // Full resource cleanup + re-initialization must keep working.
    runtime.shutdown();
    check(runtime.initialize() == Status::Ok, "vulkan re-initialization must work");
    {
        const VectorAddResult r = runtime.execute(make_task(1024), "vulkan");
        check(r.status == Status::Ok, "vulkan execution after re-init must work");
    }
    runtime.shutdown();

    if (failures == 0) {
        std::cout << "GPU compute tests passed.\n";
        return 0;
    }
    return 1;
}
