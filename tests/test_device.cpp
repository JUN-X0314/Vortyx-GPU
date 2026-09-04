// Hardware discovery tests.
//
// These tests are deliberately hardware-independent:
//  - No assertion requires a specific number of GPUs (systems may have
//    zero, one, or many GPUs; all are valid results).
//  - No assertion compares against hardcoded hardware values.
// They verify the Device abstraction and the discovery machinery itself.

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "core/device/device.hpp"
#include "core/device/discovery.hpp"

using vortyx::device::describe;
using vortyx::device::DeviceType;
using vortyx::device::DeviceInfo;
using vortyx::device::discover_cpus;
using vortyx::device::discover_devices;
using vortyx::device::discover_gpus;
using vortyx::device::DiscoveryResult;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

}  // namespace

int main() {
    // --- 1. DeviceInfo is safely constructible and copyable/movable --------
    DeviceInfo info;
    check(info.type == DeviceType::Unknown, "default DeviceType must be Unknown");
    check(info.name.empty(), "default name must be empty (unknown)");
    check(info.vendor.empty(), "default vendor must be empty (unknown)");
    check(!info.logical_processors.has_value(), "default logical_processors must be unset");
    check(!info.physical_cores.has_value(), "default physical_cores must be unset");
    check(!info.memory_bytes.has_value(), "default memory_bytes must be unset");
    check(!info.shared_memory_bytes.has_value(), "default shared_memory_bytes must be unset");

    DeviceInfo copy = info;
    copy.name = "copied";
    check(info.name.empty(), "copying must not share state between objects");
    DeviceInfo moved = std::move(copy);
    check(moved.name == "copied", "moving must preserve data");

    // --- 2. DeviceType distinguishes CPU and GPU ---------------------------
    check(DeviceType::Cpu != DeviceType::Gpu, "Cpu and Gpu must be distinct types");
    check(DeviceType::Unknown != DeviceType::Cpu, "Unknown and Cpu must be distinct types");

    // --- 3. describe() renders known and unknown fields honestly -----------
    DeviceInfo gpu;
    gpu.type = DeviceType::Gpu;
    gpu.name = "Test GPU";
    gpu.vendor = "TestVendor";
    gpu.memory_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    gpu.shared_memory_bytes = 512ull * 1024ull * 1024ull;

    const std::string gpu_text = describe(gpu);
    check(gpu_text.find("GPU") != std::string::npos, "GPU type must appear in description");
    check(gpu_text.find("Test GPU") != std::string::npos, "device name must appear in description");
    check(gpu_text.find("TestVendor") != std::string::npos, "vendor must appear in description");
    check(gpu_text.find("8.0 GiB") != std::string::npos, "VRAM must be rendered human-readable");
    check(gpu_text.find("512.0 MiB") != std::string::npos, "shared memory must be rendered");

    DeviceInfo unnamed;
    unnamed.type = DeviceType::Gpu;
    const std::string unknown_text = describe(unnamed);
    check(unknown_text.find("unknown") != std::string::npos,
          "missing name/vendor must be shown as unknown, never fabricated");

    // --- 4. CPU discovery ---------------------------------------------------
    const DiscoveryResult cpus = discover_cpus();
    check(cpus.ok, "CPU discovery mechanism must succeed on supported platforms");
    check(cpus.error.empty(), "successful CPU discovery must not carry an error");
    check(!cpus.devices.empty(), "at least one CPU entry must exist on any running system");
    for (const auto& cpu : cpus.devices) {
        check(cpu.type == DeviceType::Cpu, "CPU discovery results must be typed Cpu");
        check(!cpu.backend.empty(), "CPU devices must record their discovery backend");
        if (cpu.logical_processors.has_value()) {
            check(*cpu.logical_processors > 0, "logical processor count must be positive when present");
        }
        if (cpu.physical_cores.has_value()) {
            check(*cpu.physical_cores > 0, "physical core count must be positive when present");
        }
        if (cpu.physical_cores.has_value() && cpu.logical_processors.has_value()) {
            check(*cpu.physical_cores <= *cpu.logical_processors,
                  "physical cores must not exceed logical processors when both are known");
        }
        if (cpu.memory_bytes.has_value()) {
            check(*cpu.memory_bytes > 0, "system RAM must be positive when present");
        }
    }

    // --- 5. GPU discovery: zero-GPU systems are a normal, valid outcome ----
    const DiscoveryResult gpus = discover_gpus();
    for (const auto& gpu_dev : gpus.devices) {
        check(gpu_dev.type == DeviceType::Gpu, "GPU discovery results must be typed Gpu");
        check(!gpu_dev.backend.empty(), "GPU devices must record their discovery backend");
        if (gpu_dev.memory_bytes.has_value()) {
            // Dedicated VRAM of 0 bytes can legitimately appear on software
            // adapters; any value is accepted, only signedness matters here.
        }
    }
    if (!gpus.ok) {
        check(!gpus.error.empty(), "failed/unavailable GPU discovery must provide a reason");
        std::cout << "Note: GPU discovery unavailable on this machine: " << gpus.error << "\n";
    } else {
        // ok == true with zero devices is valid (machine without GPUs).
        std::cout << "Note: GPU discovery found " << gpus.devices.size() << " GPU(s).\n";
    }

    // --- 6. discover_devices(): one consistent collection ------------------
    const std::vector<DeviceInfo> all = discover_devices();
    const std::size_t expected_total =
        cpus.devices.size() + (gpus.ok ? gpus.devices.size() : 0);
    check(all.size() == expected_total,
          "discover_devices() must return the union of CPU and GPU results");
    for (const auto& device : all) {
        check(device.type == DeviceType::Cpu || device.type == DeviceType::Gpu,
              "discovery collection must never contain Unknown-typed devices");
    }

    // Calling it twice must yield the same device count (deterministic).
    const std::vector<DeviceInfo> again = discover_devices();
    check(again.size() == all.size(), "repeated discovery must be deterministic in device count");

    if (failures == 0) {
        std::cout << "Device discovery tests passed.\n";
        return 0;
    }
    return 1;
}
