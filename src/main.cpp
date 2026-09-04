#include <algorithm>
#include <iostream>
#include <string>
#include "core/version.hpp"
#include "core/logger.hpp"
#include "core/device/discovery.hpp"
#include "core/device/device.hpp"
#include "core/compute/task.hpp"
#include "core/resource/buffer.hpp"
#include "core/resource/resource.hpp"
#include "core/resource/resource_manager.hpp"
#include "core/vgpu/virtual_gpu.hpp"

#ifndef VORTYX_BUILD_CONFIG
#define VORTYX_BUILD_CONFIG "Unknown"
#endif

namespace {

std::string join_values(const std::vector<std::int32_t>& values, std::size_t max_count) {
    std::string out;
    const std::size_t limit = std::min(values.size(), max_count);
    for (std::size_t i = 0; i < limit; ++i) {
        if (i > 0) out += " ";
        out += std::to_string(values[i]);
    }
    if (values.size() > limit) out += " ...";
    return out;
}

// Phase 5: runs one vector addition entirely through the Virtual GPU API,
// using explicit Buffer resources underneath:
//   create -> write (upload) -> execute -> read (download) -> release.
// Returns false and fills 'error' on any failure. Buffers are released
// explicitly with reset() at the end (RAII would release them at scope exit
// even on the error paths above).
bool run_vgpu_resource_vector_add(vortyx::vgpu::VirtualGpu& gpu,
                                  const std::vector<std::int32_t>& input_a,
                                  const std::vector<std::int32_t>& input_b,
                                  std::vector<std::int32_t>& output, std::string& error) {
    const std::size_t count = input_a.size();
    const std::size_t bytes = count * sizeof(std::int32_t);

    const vortyx::resource::BufferDesc desc_in =
        vortyx::resource::BufferDesc::of<std::int32_t>(count, vortyx::resource::ResourceAccess::Read);
    const vortyx::resource::BufferDesc desc_out =
        vortyx::resource::BufferDesc::of<std::int32_t>(count, vortyx::resource::ResourceAccess::Write);

    // 1. Resource creation through the Virtual GPU's Resource Manager, on the
    //    backend this Virtual GPU was explicitly configured for.
    vortyx::resource::ResourceManager* manager = gpu.resources();
    if (manager == nullptr) {
        error = "Virtual GPU has no Resource Manager (not initialized)";
        return false;
    }
    vortyx::resource::BufferResult ra = manager->create_buffer(desc_in, gpu.backend_name());
    if (ra.status != vortyx::compute::Status::Ok) {
        error = "buffer A: " + ra.error;
        return false;
    }
    vortyx::resource::BufferResult rb = manager->create_buffer(desc_in, gpu.backend_name());
    if (rb.status != vortyx::compute::Status::Ok) {
        error = "buffer B: " + rb.error;
        return false;
    }
    vortyx::resource::BufferResult rc = manager->create_buffer(desc_out, gpu.backend_name());
    if (rc.status != vortyx::compute::Status::Ok) {
        error = "buffer C: " + rc.error;
        return false;
    }

    bool ok = false;
    do {
        // 2. Write (upload) input data into the resources.
        const vortyx::compute::ComputeResult wa = ra.buffer.write(input_a.data(), bytes);
        if (wa.status != vortyx::compute::Status::Ok) {
            error = "write A: " + wa.error;
            break;
        }
        const vortyx::compute::ComputeResult wb = rb.buffer.write(input_b.data(), bytes);
        if (wb.status != vortyx::compute::Status::Ok) {
            error = "write B: " + wb.error;
            break;
        }

        // 3. Compute through the Virtual GPU: the backend reads/writes the
        //    resources directly.
        const vortyx::compute::ComputeResult exec = gpu.execute(ra.buffer, rb.buffer, rc.buffer);
        if (exec.status != vortyx::compute::Status::Ok) {
            error = "execute: " + exec.error;
            break;
        }

        // 4. Read (download) the result out of the output resource.
        output.assign(count, 0);
        const vortyx::compute::ComputeResult rd = rc.buffer.read(output.data(), bytes);
        if (rd.status != vortyx::compute::Status::Ok) {
            error = "read C: " + rd.error;
            break;
        }
        ok = true;
    } while (false);

    // 5. Explicit release. (The destructor would do the same on scope exit;
    // reset() just makes the release point visible here.)
    ra.buffer.reset();
    rb.buffer.reset();
    rc.buffer.reset();

    return ok;
}

// Honest one-line description of the device a Virtual GPU executes on.
std::string vgpu_device_text(const vortyx::vgpu::VirtualGpu& gpu) {
    const vortyx::device::DeviceInfo device = gpu.device_info();
    if (device.type == vortyx::device::DeviceType::Unknown) {
        return "no device reported";
    }
    return vortyx::device::describe(device);
}

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Vortyx GPU" << std::endl;
    std::cout << "  Version: " << VORTYX_VERSION_STRING << std::endl;
    std::cout << "  Phase:   5 (Virtual GPU Interface)" << std::endl;
    std::cout << "  Build:   " << VORTYX_BUILD_CONFIG << std::endl;
    std::cout << "========================================" << std::endl;

    vortyx::log(vortyx::LogLevel::Info, "Vortyx started.");

    // Hardware Discovery (Phase 2): find CPUs and GPUs on this machine.
    // GPU discovery failure is logged inside discover_devices() and must not
    // stop the program or CPU discovery.
    const std::vector<vortyx::device::DeviceInfo> devices = vortyx::device::discover_devices();

    const std::size_t cpu_count =
        static_cast<std::size_t>(std::count_if(devices.begin(), devices.end(),
            [](const vortyx::device::DeviceInfo& d) {
                return d.type == vortyx::device::DeviceType::Cpu;
            }));
    const std::size_t gpu_count =
        static_cast<std::size_t>(std::count_if(devices.begin(), devices.end(),
            [](const vortyx::device::DeviceInfo& d) {
                return d.type == vortyx::device::DeviceType::Gpu ||
                       d.type == vortyx::device::DeviceType::SoftwareGpu;
            }));

    vortyx::log(vortyx::LogLevel::Info,
                "Discovered " + std::to_string(devices.size()) + " device(s): " +
                    std::to_string(cpu_count) + " CPU, " +
                    std::to_string(gpu_count) + " GPU.");

    for (std::size_t i = 0; i < devices.size(); ++i) {
        vortyx::log(vortyx::LogLevel::Info,
                    "  Device " + std::to_string(i) + ": " +
                        vortyx::device::describe(devices[i]) +
                        " (via " +
                        (devices[i].backend.empty() ? std::string("unknown") : devices[i].backend) +
                        ")");
    }

    const std::vector<std::int32_t> demo_a = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<std::int32_t> demo_b = {10, 20, 30, 40, 50, 60, 70, 80};

    std::vector<std::int32_t> cpu_task_result;
    std::vector<std::int32_t> cpu_resource_result;

    // =====================================================================
    // 1. CPU Virtual GPU (Phase 5): the always-available logical device.
    // =====================================================================
    {
        vortyx::vgpu::VirtualGpuDesc desc;  // backend defaults to "cpu"
        vortyx::vgpu::VirtualGpu gpu;
        if (gpu.initialize(desc) != vortyx::compute::Status::Ok) {
            vortyx::log(vortyx::LogLevel::Error, "CPU Virtual GPU failed to initialize.");
            return 1;
        }
        vortyx::log(vortyx::LogLevel::Info,
                    "CPU Virtual GPU ready: backend='" + gpu.backend_name() + "', state=" +
                        vortyx::vgpu::to_string(gpu.state()) + ", device: " +
                        vgpu_device_text(gpu));

        // --- 1a. Task-based execution through the Virtual GPU ------------
        vortyx::compute::VectorAddTask demo;
        demo.a = demo_a;
        demo.b = demo_b;

        const vortyx::compute::VectorAddResult result = gpu.execute(demo);
        if (result.status == vortyx::compute::Status::Ok) {
            cpu_task_result = result.data;
            vortyx::log(vortyx::LogLevel::Info,
                        "Virtual GPU (cpu) task execution success: C = A + B (" +
                            join_values(result.data, 8) + ")");
        } else {
            vortyx::log(vortyx::LogLevel::Error,
                        std::string("Virtual GPU (cpu) task execution failed: ") +
                            to_string(result.status) + " - " + result.error);
        }

        // --- 1b. Resource-based execution through the Virtual GPU --------
        std::string resource_error;
        if (run_vgpu_resource_vector_add(gpu, demo_a, demo_b, cpu_resource_result,
                                         resource_error)) {
            vortyx::log(vortyx::LogLevel::Info,
                        "Virtual GPU (cpu) resource execution success: C = A + B (" +
                            join_values(cpu_resource_result, 8) + ")");
        } else {
            vortyx::log(vortyx::LogLevel::Error,
                        "Virtual GPU (cpu) resource execution failed: " + resource_error);
        }

        // --- 1c. Honest resource accounting ------------------------------
        const vortyx::resource::ResourceStats stats = gpu.resources()->stats();
        vortyx::log(vortyx::LogLevel::Info,
                    "CPU Virtual GPU resource stats: " + std::to_string(stats.live_buffers) +
                        " live buffer(s), " + std::to_string(stats.live_bytes) +
                        " live byte(s), " + std::to_string(stats.total_allocations) +
                        " total allocation(s).");

        gpu.shutdown();
    }

    // =====================================================================
    // 2. Vulkan Virtual GPU (Phase 5): created always, reported honestly.
    //    A known-but-unavailable backend is a normal environment state, so
    //    initialization succeeds; the truth is in backend_available().
    //    There is NO automatic fallback: the CPU path above was already an
    //    explicit CPU Virtual GPU.
    // =====================================================================
    {
        vortyx::vgpu::VirtualGpuDesc desc;
        desc.backend = "vulkan";
        vortyx::vgpu::VirtualGpu gpu;
        if (gpu.initialize(desc) != vortyx::compute::Status::Ok) {
            vortyx::log(vortyx::LogLevel::Error,
                        "Vulkan Virtual GPU initialization returned an unexpected failure.");
            return 1;
        }

        if (gpu.backend_available()) {
            const vortyx::device::DeviceInfo device = gpu.device_info();
            const bool software = (device.type == vortyx::device::DeviceType::SoftwareGpu);
            vortyx::log(vortyx::LogLevel::Info,
                        std::string("Vulkan Virtual GPU ready: backend='vulkan', device: ") +
                            (device.name.empty() ? std::string("unknown") : device.name) +
                            (software ? " (software/CPU implementation - not a hardware GPU)"
                                      : ""));
            vortyx::log(vortyx::LogLevel::Info,
                        "Device details: " + vgpu_device_text(gpu));

            // --- 2a. Task-based execution -------------------------------
            vortyx::compute::VectorAddTask demo;
            demo.a = demo_a;
            demo.b = demo_b;

            const vortyx::compute::VectorAddResult result = gpu.execute(demo);
            if (result.status == vortyx::compute::Status::Ok) {
                vortyx::log(vortyx::LogLevel::Info,
                            "Virtual GPU (vulkan) task execution success: C = A + B (" +
                                join_values(result.data, 8) + ")");
                const bool match = (result.data == cpu_task_result);
                vortyx::log(vortyx::LogLevel::Info,
                            match ? "Result verification: Virtual GPU (vulkan) output matches "
                                    "Virtual GPU (cpu) output."
                                  : "Result verification: MISMATCH between vulkan and cpu "
                                    "Virtual GPU outputs!");
            } else {
                vortyx::log(vortyx::LogLevel::Warning,
                            std::string("Virtual GPU (vulkan) task execution failed: ") +
                                to_string(result.status) + " - " + result.error);
            }

            // --- 2b. Resource-based execution ---------------------------
            std::vector<std::int32_t> vulkan_resource_result;
            std::string resource_error;
            if (run_vgpu_resource_vector_add(gpu, demo_a, demo_b, vulkan_resource_result,
                                             resource_error)) {
                vortyx::log(vortyx::LogLevel::Info,
                            "Virtual GPU (vulkan) resource execution success: C = A + B (" +
                                join_values(vulkan_resource_result, 8) + ")");
                const bool match = (vulkan_resource_result == cpu_resource_result);
                vortyx::log(vortyx::LogLevel::Info,
                            match ? "Resource verification: vulkan buffer output matches cpu "
                                    "buffer output."
                                  : "Resource verification: MISMATCH between vulkan and cpu "
                                    "buffer outputs!");
            } else {
                vortyx::log(vortyx::LogLevel::Warning,
                            "Virtual GPU (vulkan) resource execution failed: " + resource_error);
            }
        } else {
            vortyx::log(vortyx::LogLevel::Info,
                        "Vulkan Virtual GPU is not usable on this system: " +
                            gpu.backend_unavailable_reason());
            vortyx::log(vortyx::LogLevel::Info,
                        "No fallback was attempted: the CPU Virtual GPU above ran on the "
                        "explicitly configured cpu backend.");
        }

        gpu.shutdown();

        // --- 2c. Honest error behavior after shutdown (no crash) ---------
        vortyx::compute::VectorAddTask demo;
        demo.a = demo_a;
        demo.b = demo_b;
        const vortyx::compute::VectorAddResult refused = gpu.execute(demo);
        vortyx::log(vortyx::LogLevel::Info,
                    std::string("Post-shutdown execute() refused as expected: ") +
                        to_string(refused.status) + " - " + refused.error);
    }

    vortyx::log(vortyx::LogLevel::Info, "Hardware discovery: implemented (Phase 2).");
    vortyx::log(vortyx::LogLevel::Info, "Compute Runtime: implemented (Phase 3) - CPU backend always available, Vulkan GPU backend when a Vulkan device is present.");
    vortyx::log(vortyx::LogLevel::Info, "Compute Resource Manager: implemented (Phase 4) - Buffer resources with explicit host/device memory, upload/download, RAII ownership and safe shutdown.");
    vortyx::log(vortyx::LogLevel::Info, "Virtual GPU: implemented (Phase 5) - one logical compute device per explicitly chosen backend; no automatic backend choice, no silent fallback.");
    vortyx::log(vortyx::LogLevel::Info, "Not implemented yet: Task Queue (Phase 6), Scheduler (Phase 7), Multi-GPU, Distributed Computing.");

    return 0;
}
