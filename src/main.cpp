#include <algorithm>
#include <iostream>
#include <string>
#include "core/version.hpp"
#include "core/logger.hpp"
#include "core/device/discovery.hpp"
#include "core/device/device.hpp"
#include "core/compute/task.hpp"
#include "core/compute/runtime.hpp"
#include "core/resource/buffer.hpp"
#include "core/resource/resource.hpp"
#include "core/resource/resource_manager.hpp"

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

// Phase 4: runs one vector addition entirely through explicit Buffer
// resources, demonstrating the full resource lifecycle:
//   create -> write (upload) -> execute -> read (download) -> release.
// Returns false and fills 'error' on any failure. Buffers are released
// explicitly with reset() at the end (RAII would release them at scope exit
// even on the error paths above).
bool run_resource_vector_add(vortyx::compute::Runtime& runtime, const std::string& backend_name,
                             const std::vector<std::int32_t>& input_a,
                             const std::vector<std::int32_t>& input_b,
                             std::vector<std::int32_t>& output, std::string& error) {
    const std::size_t count = input_a.size();
    const std::size_t bytes = count * sizeof(std::int32_t);

    const vortyx::resource::BufferDesc desc_in =
        vortyx::resource::BufferDesc::of<std::int32_t>(count, vortyx::resource::ResourceAccess::Read);
    const vortyx::resource::BufferDesc desc_out =
        vortyx::resource::BufferDesc::of<std::int32_t>(count, vortyx::resource::ResourceAccess::Write);

    // 1. Resource creation through the Runtime's Resource Manager.
    vortyx::resource::BufferResult ra = runtime.resources().create_buffer(desc_in, backend_name);
    if (ra.status != vortyx::compute::Status::Ok) {
        error = "buffer A: " + ra.error;
        return false;
    }
    vortyx::resource::BufferResult rb = runtime.resources().create_buffer(desc_in, backend_name);
    if (rb.status != vortyx::compute::Status::Ok) {
        error = "buffer B: " + rb.error;
        return false;
    }
    vortyx::resource::BufferResult rc = runtime.resources().create_buffer(desc_out, backend_name);
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

        // 3. Compute: the backend reads/writes the resources directly.
        const vortyx::compute::ComputeResult exec = runtime.execute(ra.buffer, rb.buffer, rc.buffer);
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

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Vortyx GPU" << std::endl;
    std::cout << "  Version: " << VORTYX_VERSION_STRING << std::endl;
    std::cout << "  Phase:   4 (Compute Resource & Memory Management)" << std::endl;
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

    // Compute Runtime (Phase 3) + Resource Manager (Phase 4).
    vortyx::compute::Runtime runtime;
    if (runtime.initialize() != vortyx::compute::Status::Ok) {
        vortyx::log(vortyx::LogLevel::Error, "Compute Runtime failed to initialize.");
        return 1;
    }

    const std::vector<std::int32_t> demo_a = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<std::int32_t> demo_b = {10, 20, 30, 40, 50, 60, 70, 80};

    // --- 1. Task-based execution (Phase 3 API, now routed through the
    //        Resource Manager internally) -----------------------------------
    vortyx::compute::VectorAddTask demo;
    demo.a = demo_a;
    demo.b = demo_b;

    const vortyx::compute::VectorAddResult cpu_result = runtime.execute(demo, "cpu");
    if (cpu_result.status == vortyx::compute::Status::Ok) {
        vortyx::log(vortyx::LogLevel::Info,
                    "CPU execution success: C = A + B (" + join_values(cpu_result.data, 8) + ")");
    } else {
        vortyx::log(vortyx::LogLevel::Error,
                    std::string("CPU execution failed: ") + to_string(cpu_result.status) +
                        " - " + cpu_result.error);
    }

    if (runtime.has_backend("vulkan")) {
        const vortyx::compute::VectorAddResult gpu_result = runtime.execute(demo, "vulkan");
        if (gpu_result.status == vortyx::compute::Status::Ok) {
            const bool match = (gpu_result.data == cpu_result.data);
            const vortyx::device::DeviceInfo gpu_device = runtime.backend_device("vulkan");
            vortyx::log(vortyx::LogLevel::Info,
                        "GPU (Vulkan) execution success on '" +
                            (gpu_device.name.empty() ? std::string("unknown device")
                                                     : gpu_device.name) +
                            "': C = A + B (" + join_values(gpu_result.data, 8) + ")");
            vortyx::log(vortyx::LogLevel::Info,
                        match ? "Result verification: GPU output matches CPU reference."
                              : "Result verification: MISMATCH between GPU and CPU results!");
        } else {
            vortyx::log(vortyx::LogLevel::Warning,
                        std::string("GPU (Vulkan) execution failed: ") +
                            to_string(gpu_result.status) + " - " + gpu_result.error);
        }
    } else {
        vortyx::log(vortyx::LogLevel::Info,
                    "GPU (Vulkan) backend unavailable on this system: " +
                        runtime.backend_unavailable_reason("vulkan"));
    }

    // --- 2. Resource-based execution (Phase 4 API): the same calculation
    //        with explicitly managed Buffer resources ------------------------
    std::vector<std::int32_t> resource_cpu;
    std::string resource_error;
    if (run_resource_vector_add(runtime, "cpu", demo_a, demo_b, resource_cpu, resource_error)) {
        vortyx::log(vortyx::LogLevel::Info,
                    "Resource-based CPU execution success: C = A + B (" +
                        join_values(resource_cpu, 8) + ")");
    } else {
        vortyx::log(vortyx::LogLevel::Error,
                    "Resource-based CPU execution failed: " + resource_error);
    }

    if (runtime.has_backend("vulkan")) {
        std::vector<std::int32_t> resource_gpu;
        if (run_resource_vector_add(runtime, "vulkan", demo_a, demo_b, resource_gpu,
                                    resource_error)) {
            const vortyx::device::DeviceInfo gpu_device = runtime.backend_device("vulkan");
            vortyx::log(vortyx::LogLevel::Info,
                        "Resource-based GPU (Vulkan) execution success on '" +
                            (gpu_device.name.empty() ? std::string("unknown device")
                                                     : gpu_device.name) +
                            "': C = A + B (" + join_values(resource_gpu, 8) + ")");
            const bool match = (resource_gpu == resource_cpu) && (resource_gpu == cpu_result.data);
            vortyx::log(vortyx::LogLevel::Info,
                        match ? "Resource verification: GPU buffer output matches CPU output."
                              : "Resource verification: MISMATCH between GPU and CPU buffer outputs!");
        } else {
            vortyx::log(vortyx::LogLevel::Warning,
                        "Resource-based GPU (Vulkan) execution failed: " + resource_error);
        }
    }

    // Honest accounting: after the demos everything was released through
    // RAII / reset(); the manager reports zero live buffers.
    const vortyx::resource::ResourceStats stats = runtime.resources().stats();
    vortyx::log(vortyx::LogLevel::Info,
                "Resource stats: " + std::to_string(stats.live_buffers) +
                    " live buffer(s), " + std::to_string(stats.live_bytes) +
                    " live byte(s), " + std::to_string(stats.total_allocations) +
                    " total allocation(s) this session.");

    runtime.shutdown();

    vortyx::log(vortyx::LogLevel::Info, "Hardware discovery: implemented (Phase 2).");
    vortyx::log(vortyx::LogLevel::Info, "Compute Runtime: implemented (Phase 3) - CPU backend always available, Vulkan GPU backend when a Vulkan device is present.");
    vortyx::log(vortyx::LogLevel::Info, "Compute Resource Manager: implemented (Phase 4) - Buffer resources with explicit host/device memory, upload/download, RAII ownership and safe shutdown.");
    vortyx::log(vortyx::LogLevel::Info, "Not implemented yet: Virtual GPU (Phase 5), Task Queue (Phase 6), Scheduler (Phase 7), Multi-GPU, Distributed Computing.");

    return 0;
}
