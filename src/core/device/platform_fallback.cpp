// Fallback backend for platforms without a dedicated implementation
// (e.g. macOS). CPU discovery uses only the standard C++ runtime;
// GPU discovery is reported as unavailable, never faked.

#if !defined(_WIN32) && !defined(__linux__)

#include "core/device/platform_discovery.hpp"

#include <thread>

namespace vortyx::device::detail {

DiscoveryResult discover_cpus_platform() {
    DiscoveryResult result;
    result.ok = true;

    DeviceInfo cpu;
    cpu.type = DeviceType::Cpu;
    cpu.backend = "generic";

    const unsigned int hardware_concurrency = std::thread::hardware_concurrency();
    if (hardware_concurrency > 0) {
        cpu.logical_processors = hardware_concurrency;
    }

    result.devices.push_back(std::move(cpu));
    return result;
}

DiscoveryResult discover_gpus_platform() {
    DiscoveryResult result;
    result.ok = false;
    result.error = "GPU discovery is not implemented for this platform yet";
    return result;
}

}  // namespace vortyx::device::detail

#endif  // !_WIN32 && !__linux__
