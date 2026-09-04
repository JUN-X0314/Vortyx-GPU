#include "core/compute/cpu_backend.hpp"

#include "core/device/discovery.hpp"

namespace vortyx::compute {

vortyx::device::DeviceInfo CpuBackend::device_info() const {
    // Reuses the Phase 2 CPU discovery; the machine always has at least
    // one CPU entry. If discovery unexpectedly fails, an unknown device is
    // reported instead of fabricated data.
    const vortyx::device::DiscoveryResult cpus = vortyx::device::discover_cpus();
    if (cpus.ok && !cpus.devices.empty()) {
        return cpus.devices.front();
    }
    vortyx::device::DeviceInfo unknown;
    unknown.type = vortyx::device::DeviceType::Cpu;
    unknown.backend = "cpu";
    return unknown;
}

VectorAddResult CpuBackend::execute(const VectorAddTask& task) {
    VectorAddResult result;

    const Status validation = validate_vector_add(task);
    if (validation != Status::Ok) {
        result.status = validation;
        result.error = "invalid vector addition task (a.size=" +
                       std::to_string(task.a.size()) + ", b.size=" +
                       std::to_string(task.b.size()) + "); arrays must be non-empty and equal size";
        return result;
    }

    const std::size_t count = task.a.size();
    result.data.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        result.data[i] = task.a[i] + task.b[i];
    }

    result.status = Status::Ok;
    result.error.clear();
    return result;
}

}  // namespace vortyx::compute
