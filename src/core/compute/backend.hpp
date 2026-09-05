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

// ---------------------------------------------------------------------------
// Buffer-level dispatch (Phase 10 — Compute Engine)
// ---------------------------------------------------------------------------

// ONE explicit buffer-level compute request: which operation, its scalar
// parameter, and the live backend-owned buffers it reads/writes. This is the
// single dispatch shape every elementwise operation goes through — adding an
// operation extends ComputeOp and the validation table, not the interface.
//
// Buffer rules (enforced by the Runtime AND defensively re-checked by every
// backend, like the Phase 4 contract):
//   - input_a/output must be non-null for every op; input_b must be non-null
//     for VectorAdd/VectorMultiply and null for VectorScale.
//   - the buffers must have been created through THIS backend's provider
//     (foreign buffers are rejected with an error, never accessed) — the
//     Runtime verifies ownership BEFORE resolving handles; the backend
//     re-checks the concrete type and device identity.
//   - shapes/access roles are validated with
//     validate_compute_dispatch_buffers(op, ...).
struct ComputeDispatch {
    ComputeOp op = ComputeOp::VectorAdd;

    // Operation parameter. VectorScale: the scale factor (int32). All other
    // current ops ignore it (and their task-level validation refuses a
    // non-zero scalar so it can never silently matter here).
    std::int32_t scalar = 0;

    const vortyx::resource::IBufferImpl* input_a = nullptr;
    const vortyx::resource::IBufferImpl* input_b = nullptr;  // op-dependent, see above
    vortyx::resource::IBufferImpl* output = nullptr;
};

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
    //
    // Phase 10: this remains the exact Phase 4 vector-add contract. It is
    // now implemented as the VectorAdd specialization of the generic
    // dispatch below (every backend funnels it through execute(dispatch),
    // so the compute loops/binding code exist exactly once per backend).
    virtual ComputeResult execute(const vortyx::resource::IBufferImpl& a,
                                  const vortyx::resource::IBufferImpl& b,
                                  vortyx::resource::IBufferImpl& c) = 0;

    // Generic buffer-level dispatch (Phase 10): executes the operation named
    // by 'dispatch' over the given backend-owned buffers. Same ownership,
    // validation and no-throw rules as execute(a, b, c) above, generalized
    // to the op in the dispatch. The output is left inside 'output' (the
    // caller downloads it through Buffer::read).
    virtual ComputeResult execute(const ComputeDispatch& dispatch) = 0;

    // The backend's buffer factory for the Resource Manager, or nullptr when
    // this backend does not provide device storage (stub build, not
    // initialized, no device memory of its own). The Runtime registers the
    // returned provider; the backend keeps ownership.
    virtual vortyx::resource::IBufferProvider* resource_provider() { return nullptr; }

protected:
    IComputeBackend() = default;
};

}  // namespace vortyx::compute
