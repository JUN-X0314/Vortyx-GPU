#include "core/device/device.hpp"

#include <iomanip>
#include <sstream>

namespace vortyx::device {
namespace {

std::string format_bytes(std::uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;
    if (static_cast<double>(bytes) >= kGiB) {
        oss << (static_cast<double>(bytes) / kGiB) << " GiB";
    } else {
        oss << (static_cast<double>(bytes) / kMiB) << " MiB";
    }
    return oss.str();
}

const char* device_type_name(DeviceType type) {
    switch (type) {
        case DeviceType::Cpu: return "CPU";
        case DeviceType::Gpu: return "GPU";
        default: return "Unknown device";
    }
}

}  // namespace

std::string describe(const DeviceInfo& device) {
    std::ostringstream oss;
    oss << device_type_name(device.type);
    oss << ": " << (device.name.empty() ? std::string("unknown") : device.name);
    oss << " | vendor: " << (device.vendor.empty() ? std::string("unknown") : device.vendor);

    if (device.type == DeviceType::Cpu) {
        if (device.logical_processors.has_value()) {
            oss << " | " << *device.logical_processors << " logical processors";
        }
        if (device.physical_cores.has_value()) {
            oss << " | " << *device.physical_cores << " physical cores";
        }
        if (device.memory_bytes.has_value()) {
            oss << " | RAM " << format_bytes(*device.memory_bytes);
        }
    } else if (device.type == DeviceType::Gpu) {
        if (device.memory_bytes.has_value()) {
            oss << " | VRAM " << format_bytes(*device.memory_bytes);
        }
        if (device.shared_memory_bytes.has_value()) {
            oss << " | shared " << format_bytes(*device.shared_memory_bytes);
        }
    }

    if (!device.id.empty()) {
        oss << " | id: " << device.id;
    }
    return oss.str();
}

}  // namespace vortyx::device
