#pragma once

// CPU compute backend (Phase 4).
// Reference implementation for vector addition: deterministic, always
// available, and the correctness baseline the GPU backend is checked against.
//
// Phase 4: the backend computes directly on Buffer resources created through
// the Resource Manager (host-memory buffers). Task->buffer translation lives
// in the Runtime; the backend itself never allocates scratch memory.

#include "core/compute/backend.hpp"
#include "core/resource/cpu_buffer.hpp"

namespace vortyx::compute {

class CpuBackend final : public IComputeBackend {
public:
    CpuBackend() = default;

    const char* name() const override { return "cpu"; }
    bool available() const override { return true; }
    std::string unavailable_reason() const override { return {}; }

    vortyx::device::DeviceInfo device_info() const override;

    // Buffer-based execution: a, b, c must be CpuBuffer resources. Reads the
    // inputs' host storage, writes the sum into the output's host storage.
    ComputeResult execute(const vortyx::resource::IBufferImpl& a,
                          const vortyx::resource::IBufferImpl& b,
                          vortyx::resource::IBufferImpl& c) override;
};

}  // namespace vortyx::compute
