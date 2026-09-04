// Virtual GPU GPU-path tests (Phase 5).
//
// Design rules (per project requirements):
//  - No hardcoded hardware expectations (no "there must be exactly 1 GPU",
//    no vendor names). Tests pass on AMD, Intel, NVIDIA, software Vulkan
//    implementations (lavapipe/llvmpipe) and on machines with NO GPU at all.
//  - When the Vulkan backend is unavailable, the test exits successfully
//    with an explicit, visible note saying it did NOT run. It is never
//    reported as a GPU execution success.
//  - When the backend IS available, real vector additions run through the
//    Virtual GPU and must match the CPU reference bit-exactly.
//  - A Virtual GPU configured for one backend must never execute another
//    backend's buffers silently.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "core/compute/task.hpp"
#include "core/device/device.hpp"
#include "core/resource/buffer.hpp"
#include "core/resource/resource.hpp"
#include "core/resource/resource_manager.hpp"
#include "core/vgpu/virtual_gpu.hpp"

using vortyx::compute::ComputeResult;
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
    // The reference CPU Virtual GPU exists in every environment (created
    // first so the vulkan path below is always compared against a real,
    // independently executed CPU result).
    VirtualGpu cpu_gpu;
    VirtualGpuDesc cpu_desc;
    cpu_desc.backend = "cpu";
    if (cpu_gpu.initialize(cpu_desc) != Status::Ok) {
        std::cerr << "FAIL: cpu VirtualGpu initialize() failed\n";
        return 1;
    }

    // The Vulkan Virtual GPU under test.
    VirtualGpu vulkan_gpu;
    VirtualGpuDesc vulkan_desc;
    vulkan_desc.backend = "vulkan";
    if (vulkan_gpu.initialize(vulkan_desc) != Status::Ok) {
        std::cerr << "FAIL: vulkan VirtualGpu initialize() must not fail for a known backend\n";
        return 1;
    }

    if (!vulkan_gpu.backend_available()) {
        // Environment without a usable Vulkan device: a normal, non-fatal
        // condition. Report it clearly; do NOT fake success.
        std::cout << "SKIPPED (environment): Vulkan backend unavailable - "
                  << vulkan_gpu.backend_unavailable_reason() << "\n";
        std::cout << "Note: GPU execution was NOT tested on this machine. "
                  << "The Virtual GPU CPU path is covered by VirtualGpuTest.\n";
        vulkan_gpu.shutdown();
        cpu_gpu.shutdown();
        return 0;
    }

    // --- Backend is available: run REAL Virtual GPU tests -------------------
    const vortyx::device::DeviceInfo device = vulkan_gpu.device_info();
    std::cout << "Vulkan Virtual GPU available on: "
              << (device.name.empty() ? std::string("unknown device") : device.name)
              << " | vendor: " << (device.vendor.empty() ? "unknown" : device.vendor) << "\n";
    check(!device.name.empty(), "an available Vulkan device must report its name");
    check(device.type == vortyx::device::DeviceType::Gpu ||
              device.type == vortyx::device::DeviceType::SoftwareGpu,
          "the Vulkan device must be typed Gpu or SoftwareGpu (never Unknown)");

    // Correctness across several sizes, including sizes that are not a
    // multiple of the 64-element workgroup (guards the shader bounds check).
    for (const std::size_t size : {std::size_t{4}, std::size_t{16}, std::size_t{64},
                                   std::size_t{1024}, std::size_t{5000}}) {
        const VectorAddTask task = make_task(size);

        const VectorAddResult cpu_ref = cpu_gpu.execute(task);
        check(cpu_ref.status == Status::Ok,
              "cpu VirtualGpu reference must succeed for size " + std::to_string(size));

        const VectorAddResult gpu_result = vulkan_gpu.execute(task);
        check(gpu_result.status == Status::Ok,
              "vulkan VirtualGpu execution must succeed for size " + std::to_string(size) +
                  " (error: " + gpu_result.error + ")");
        check(result_matches(gpu_result, task),
              "vulkan VirtualGpu result must equal A+B for size " + std::to_string(size));
        check(gpu_result.data == cpu_ref.data,
              "vulkan VirtualGpu result must match the cpu VirtualGpu result exactly for size " +
                  std::to_string(size));
    }

    // Repeated execution determinism (same input -> same output, 3 runs).
    {
        const VectorAddTask task = make_task(1024);
        const VectorAddResult first = vulkan_gpu.execute(task);
        check(first.status == Status::Ok, "repeat run 1 must succeed");
        for (int run = 2; run <= 3; ++run) {
            const VectorAddResult next = vulkan_gpu.execute(task);
            check(next.status == Status::Ok, "repeat run must succeed");
            check(next.data == first.data, "repeated Virtual GPU runs must be deterministic");
        }
    }

    // Resource-based execution through the Virtual GPU on real GPU memory.
    {
        vortyx::resource::ResourceManager* manager = vulkan_gpu.resources();
        check(manager != nullptr, "the vulkan VirtualGpu must expose its Resource Manager");

        const auto desc_in = vortyx::resource::BufferDesc::of<std::int32_t>(
            128, vortyx::resource::ResourceAccess::Read);
        const auto desc_out = vortyx::resource::BufferDesc::of<std::int32_t>(
            128, vortyx::resource::ResourceAccess::Write);

        vortyx::resource::BufferResult ra = manager->create_buffer(desc_in, vulkan_gpu.backend_name());
        vortyx::resource::BufferResult rb = manager->create_buffer(desc_in, vulkan_gpu.backend_name());
        vortyx::resource::BufferResult rc =
            manager->create_buffer(desc_out, vulkan_gpu.backend_name());
        check(ra.status == Status::Ok && rb.status == Status::Ok && rc.status == Status::Ok,
              "real Vulkan buffer creation through the Virtual GPU must succeed");
        check(std::string(ra.buffer.backend_name()) == "vulkan",
              "vulkan buffers must report their backend");
        check(ra.buffer.memory_location() == vortyx::resource::MemoryLocation::Device,
              "vulkan buffers must honestly report Device memory (never Host)");

        const VectorAddTask task = make_task(128);
        const std::size_t bytes = task.a.size() * sizeof(std::int32_t);
        check(ra.buffer.write(task.a.data(), bytes).status == Status::Ok, "upload A must succeed");
        check(rb.buffer.write(task.b.data(), bytes).status == Status::Ok, "upload B must succeed");

        const ComputeResult exec = vulkan_gpu.execute(ra.buffer, rb.buffer, rc.buffer);
        check(exec.status == Status::Ok,
              "resource-based GPU execution through the Virtual GPU must succeed (error: " +
                  exec.error + ")");

        std::vector<std::int32_t> out(128, 0);
        check(rc.buffer.read(out.data(), bytes).status == Status::Ok, "download C must succeed");
        bool match = true;
        for (std::size_t i = 0; i < task.a.size(); ++i) {
            if (out[i] != task.a[i] + task.b[i]) match = false;
        }
        check(match, "resource-based GPU result must equal A+B");
    }

    // Cross-backend rejection: a Virtual GPU is ONE explicit execution
    // target and must refuse another backend's buffers instead of running
    // them silently.
    {
        vortyx::resource::ResourceManager* gpu_manager = vulkan_gpu.resources();
        vortyx::resource::ResourceManager* cpu_manager = cpu_gpu.resources();

        const auto desc_in = vortyx::resource::BufferDesc::of<std::int32_t>(
            8, vortyx::resource::ResourceAccess::Read);
        const auto desc_out = vortyx::resource::BufferDesc::of<std::int32_t>(
            8, vortyx::resource::ResourceAccess::Write);

        vortyx::resource::BufferResult va = gpu_manager->create_buffer(desc_in, "vulkan");
        vortyx::resource::BufferResult vb = gpu_manager->create_buffer(desc_in, "vulkan");
        vortyx::resource::BufferResult vc = gpu_manager->create_buffer(desc_out, "vulkan");
        check(va.status == Status::Ok && vb.status == Status::Ok && vc.status == Status::Ok,
              "vulkan buffer creation for the cross test");

        vortyx::resource::BufferResult ca = cpu_manager->create_buffer(desc_in, "cpu");
        vortyx::resource::BufferResult cb = cpu_manager->create_buffer(desc_in, "cpu");
        vortyx::resource::BufferResult cc = cpu_manager->create_buffer(desc_out, "cpu");
        check(ca.status == Status::Ok && cb.status == Status::Ok && cc.status == Status::Ok,
              "cpu buffer creation for the cross test");

        const ComputeResult cpu_gpu_with_gpu_buffers = cpu_gpu.execute(va.buffer, vb.buffer, vc.buffer);
        check(cpu_gpu_with_gpu_buffers.status == Status::InvalidInput,
              "the cpu VirtualGpu must reject vulkan buffers with InvalidInput");
        const ComputeResult vulkan_gpu_with_cpu_buffers =
            vulkan_gpu.execute(ca.buffer, cb.buffer, cc.buffer);
        check(vulkan_gpu_with_cpu_buffers.status == Status::InvalidInput,
              "the vulkan VirtualGpu must reject cpu buffers with InvalidInput");
    }

    // Shutdown with live resources, then re-initialization.
    vulkan_gpu.shutdown();
    check(vulkan_gpu.state() == vortyx::vgpu::State::ShutDown,
          "shutdown after GPU work must reach ShutDown");
    check(vulkan_gpu.initialize(vulkan_desc) == Status::Ok, "vulkan re-initialization must work");
    check(vulkan_gpu.backend_available(), "re-initialized vulkan VirtualGpu must be available");
    {
        const VectorAddResult r = vulkan_gpu.execute(make_task(1024));
        check(r.status == Status::Ok && result_matches(r, make_task(1024)),
              "vulkan VirtualGpu execution after re-init must work");
    }
    vulkan_gpu.shutdown();

    cpu_gpu.shutdown();

    if (failures == 0) {
        std::cout << "Virtual GPU GPU tests passed.\n";
        return 0;
    }
    return 1;
}
