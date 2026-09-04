#include <algorithm>
#include <iostream>
#include <string>
#include "core/version.hpp"
#include "core/logger.hpp"
#include "core/device/discovery.hpp"
#include "core/device/device.hpp"
#include "core/compute/task.hpp"
#include "core/compute/runtime.hpp"

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

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Vortyx GPU" << std::endl;
    std::cout << "  Version: " << VORTYX_VERSION_STRING << std::endl;
    std::cout << "  Phase:   3 (Compute Runtime)" << std::endl;
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

    // Compute Runtime (Phase 3): initialize, run a small vector addition on
    // the CPU backend, then try the GPU backend when it is available.
    vortyx::compute::Runtime runtime;
    if (runtime.initialize() != vortyx::compute::Status::Ok) {
        vortyx::log(vortyx::LogLevel::Error, "Compute Runtime failed to initialize.");
        return 1;
    }

    vortyx::compute::VectorAddTask demo;
    demo.a = {1, 2, 3, 4, 5, 6, 7, 8};
    demo.b = {10, 20, 30, 40, 50, 60, 70, 80};

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

    runtime.shutdown();

    vortyx::log(vortyx::LogLevel::Info, "Hardware discovery: implemented (Phase 2).");
    vortyx::log(vortyx::LogLevel::Info, "Compute Runtime: implemented (Phase 3) - CPU backend always available, Vulkan GPU backend when a Vulkan device is present.");
    vortyx::log(vortyx::LogLevel::Info, "Not implemented yet: Scheduler, Virtual GPU, Task Queue, Multi-GPU, Distributed Computing.");

    return 0;
}
