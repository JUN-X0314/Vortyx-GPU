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

ComputeResult CpuBackend::execute(const vortyx::resource::IBufferImpl& a,
                                  const vortyx::resource::IBufferImpl& b,
                                  vortyx::resource::IBufferImpl& c) {
    // The buffers must belong to THIS backend. Anything else would mean a
    // routing bug; reject it instead of reading foreign memory.
    const vortyx::resource::CpuBuffer* cpu_a =
        dynamic_cast<const vortyx::resource::CpuBuffer*>(&a);
    const vortyx::resource::CpuBuffer* cpu_b =
        dynamic_cast<const vortyx::resource::CpuBuffer*>(&b);
    vortyx::resource::CpuBuffer* cpu_c = dynamic_cast<vortyx::resource::CpuBuffer*>(&c);
    if (cpu_a == nullptr || cpu_b == nullptr || cpu_c == nullptr) {
        return ComputeResult{Status::BackendError,
                             "buffer does not belong to the cpu backend"};
    }

    // Shared task rules: int32 elements, equal non-zero counts, Read inputs,
    // Write output. Enforced here so direct backend users cannot bypass it.
    std::string error;
    const Status validation = validate_vector_add_buffers(a.desc(), b.desc(), c.desc(), error);
    if (validation != Status::Ok) {
        return ComputeResult{validation, error};
    }

    // The actual computation on plain host memory.
    const std::int32_t* pa = static_cast<const std::int32_t*>(cpu_a->data());
    const std::int32_t* pb = static_cast<const std::int32_t*>(cpu_b->data());
    std::int32_t* pc = static_cast<std::int32_t*>(cpu_c->data());
    const std::size_t count = a.desc().element_count;
    for (std::size_t i = 0; i < count; ++i) {
        pc[i] = pa[i] + pb[i];
    }

    return ComputeResult{Status::Ok, {}};
}

}  // namespace vortyx::compute
