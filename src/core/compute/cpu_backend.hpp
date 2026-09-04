#pragma once

// CPU compute backend (Phase 3).
// Reference implementation for vector addition: deterministic, always
// available, and the correctness baseline the GPU backend is checked against.

#include "core/compute/backend.hpp"

namespace vortyx::compute {

class CpuBackend final : public IComputeBackend {
public:
    CpuBackend() = default;

    const char* name() const override { return "cpu"; }
    bool available() const override { return true; }
    std::string unavailable_reason() const override { return {}; }

    vortyx::device::DeviceInfo device_info() const override;
    VectorAddResult execute(const VectorAddTask& task) override;
};

}  // namespace vortyx::compute
