#pragma once

// Compute Backend interface (Phase 4).
//
// The Runtime manages compute tasks and resources; a Backend performs the
// actual calculation on a concrete device (CPU cores, a Vulkan GPU, ...).
// Keeping this boundary clean is what allows future backends
// (CUDA, DirectX, Vortyx hardware, ...) without changing the Runtime.
//
// Phase 4 change — tasks and resources are now separated:
//   - Backends no longer allocate their own per-call scratch buffers.
//     They compute DIRECTLY ON Buffer resources created through the
//     Resource Manager (Runtime -> Resource Manager -> Backend -> Device).
//   - The Phase 3 execute(VectorAddTask) was removed from the backend
//     interface on purpose: translating a task into buffers (allocate ->
//     upload -> execute -> download -> release) is the RUNTIME's job now and
//     lives in exactly one place instead of being duplicated per backend.
//     The public Runtime::execute(VectorAddTask) API is unchanged for users.
//   - Backends may expose their buffer provider (resource_provider()) so the
//     Runtime can register it with the Resource Manager. A backend without
//     device memory (or a stub build) exposes none.

#include <string>

#include "core/compute/task.hpp"
#include "core/device/device.hpp"
#include "core/resource/backend_buffer.hpp"

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

    // Executes vector addition directly on backend-owned buffer resources:
    // reads a and b, writes c. The buffers MUST have been created through
    // this backend's provider (foreign buffers are rejected with an error,
    // never accessed). Must not throw; failures are reported through
    // ComputeResult::status / error.
    virtual ComputeResult execute(const vortyx::resource::IBufferImpl& a,
                                  const vortyx::resource::IBufferImpl& b,
                                  vortyx::resource::IBufferImpl& c) = 0;

    // The backend's buffer factory for the Resource Manager, or nullptr when
    // this backend does not provide device storage (stub build, not
    // initialized, no device memory of its own). The Runtime registers the
    // returned provider; the backend keeps ownership.
    virtual vortyx::resource::IBufferProvider* resource_provider() { return nullptr; }

protected:
    IComputeBackend() = default;
};

}  // namespace vortyx::compute
