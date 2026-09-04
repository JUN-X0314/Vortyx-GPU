#pragma once

// Compute Backend interface (Phase 3).
//
// The Runtime manages compute tasks; a Backend performs the actual
// calculation on a concrete device (CPU cores, a Vulkan GPU, ...).
// Keeping this boundary clean is what allows future backends
// (CUDA, DirectX, Vortyx hardware, ...) without changing the Runtime.

#include <string>

#include "core/compute/task.hpp"
#include "core/device/device.hpp"

namespace vortyx::compute {

class IComputeBackend {
public:
    virtual ~IComputeBackend() = default;

    IComputeBackend(const IComputeBackend&) = delete;
    IComputeBackend& operator=(const IComputeBackend&) = delete;

    // Stable backend name used for explicit selection ("cpu", "vulkan").
    virtual const char* name() const = 0;

    // True when this backend can run on the current system.
    // available() == false must never be reported as a working backend.
    virtual bool available() const = 0;

    // Human-readable reason when available() == false; empty otherwise.
    virtual std::string unavailable_reason() const = 0;

    // The concrete device this backend executes on, expressed with the
    // Phase 2 DeviceInfo abstraction. Cross-API device matching (e.g.
    // DXGI adapter <-> Vulkan physical device) is NOT attempted; this
    // reports only what the backend itself actually knows.
    virtual vortyx::device::DeviceInfo device_info() const = 0;

    // Executes a vector addition task. Must not throw; failures are
    // reported through VectorAddResult::status / error.
    virtual VectorAddResult execute(const VectorAddTask& task) = 0;

protected:
    IComputeBackend() = default;
};

}  // namespace vortyx::compute
