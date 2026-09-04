#include "core/device/discovery.hpp"

#include "core/device/platform_discovery.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <string>

namespace vortyx::device {

DiscoveryResult discover_cpus() {
    return detail::discover_cpus_platform();
}

DiscoveryResult discover_gpus() {
    return detail::discover_gpus_platform();
}

std::vector<DeviceInfo> discover_devices() {
    std::vector<DeviceInfo> devices;

    const DiscoveryResult cpus = discover_cpus();
    if (cpus.ok) {
        devices.insert(devices.end(), cpus.devices.begin(), cpus.devices.end());
    } else {
        vortyx::log(vortyx::LogLevel::Warning, "CPU discovery failed: " + cpus.error);
    }

    const DiscoveryResult gpus = discover_gpus();
    if (gpus.ok) {
        vortyx::log(vortyx::LogLevel::Info,
                    "GPU discovery ran successfully, found " +
                        std::to_string(gpus.devices.size()) + " GPU device(s).");
        devices.insert(devices.end(), gpus.devices.begin(), gpus.devices.end());
    } else {
        vortyx::log(vortyx::LogLevel::Warning,
                    "GPU discovery is unavailable/failed: " + gpus.error);
    }

    return devices;
}

}  // namespace vortyx::device
