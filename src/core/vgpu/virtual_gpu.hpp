#pragma once

// Virtual GPU Interface (Phase 5).
//
// The application-facing logical compute device. A Virtual GPU is NOT a
// physical GPU and does not create any new compute capability: it wraps ONE
// explicitly chosen execution backend of the Vortyx stack and presents it as
// a single logical object, so applications never touch backends, Vulkan,
// DXGI or resource internals directly:
//
//   Application -> Virtual GPU -> Compute Runtime -> Resource Manager
//                            -> Backend -> Physical Device
//
// Responsibilities (exactly these, nothing more):
//   - Own and drive one Compute Runtime (initialize / shutdown lifecycle).
//   - Bind itself to ONE explicitly configured backend ("cpu", "vulkan").
//   - Execute compute tasks on that backend and return results.
//   - Report state, backend availability, and the physical device it
//     executes on — honestly, never fabricated.
//   - Expose the Runtime's Resource Manager so buffer resources can be
//     created when an application wants explicit resource control.
//
// Explicitly NOT its job (later phases):
//   - Task Queue / async execution (Phase 6): execute() is synchronous.
//   - Scheduler (Phase 7): there is NO automatic backend/device choice and
//     NO silent fallback. If "vulkan" is configured and the system has no
//     usable Vulkan device, execute() fails with a descriptive error while
//     the CPU path remains available through a separate, explicitly created
//     CPU Virtual GPU. The decision "which backend" is always the caller's.
//   - Multi-GPU aggregation, distributed computing, memory pooling.
//
// Backend independence:
//   This header and the Virtual GPU implementation contain no Vulkan, DXGI
//   or platform types. Everything below this layer (VkInstance, queues,
//   memory types, adapter LUIDs, host buffers) stays inside the Runtime,
//   Resource Manager and backends. An application using only this interface
//   can run the same code on any current or future Vortyx backend.
//
// Ownership and lifecycle:
//   - A Virtual GPU exclusively owns its Runtime (std::unique_ptr). It is
//     created by initialize() and destroyed by shutdown(); double shutdown
//     is impossible and shutdown() is safe to call multiple times.
//   - The Virtual GPU is move-only (copying a live execution context is the
//     same class of accident as copying a GPU resource handle). Moving
//     transfers the Runtime and leaves the source Uninitialized.
//   - shutdown() only affects this Virtual GPU and the resources its own
//     Runtime owns. Device discovery (Phase 2) is a separate subsystem and
//     is never touched.
//   - Re-initializing after shutdown() is supported: it builds a fresh
//     Runtime from scratch (matching Runtime's own re-initialization
//     behavior tested since Phase 3).
//
// Error handling follows the project-wide result style (Phase 3): no
// exceptions, explicit Status values with human-readable 'error' strings.

#include <memory>
#include <string>

#include "core/compute/task.hpp"
#include "core/device/device.hpp"

// Forward declarations: the Virtual GPU API surface only references these
// through pointers/references, keeping header dependencies one-directional
// (vgpu -> compute/resource; nothing includes vgpu from below).
namespace vortyx::compute {
class Runtime;
}

namespace vortyx::resource {
class Buffer;
class ResourceManager;
}

namespace vortyx::vgpu {

// The Virtual GPU layer speaks the project-wide result vocabulary from
// Phase 3 (one unified error model, no second status system). Using
// declarations (same pattern as core/resource/buffer.hpp) because sibling
// namespace members are not found by unqualified lookup.
using vortyx::compute::Status;
using vortyx::compute::ComputeResult;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;

// ---------------------------------------------------------------------------
// Lifecycle state
// ---------------------------------------------------------------------------

// The Virtual GPU's lifecycle state.
//  - Uninitialized: constructed (or moved-from) but initialize() has not
//    succeeded yet; execute() fails with Status::NotInitialized.
//  - Ready: initialized and usable; execute() and resources() are available.
//    Note: Ready does NOT promise the configured backend is usable on this
//    system — check backend_available() for the honest answer.
//  - ShutDown: shutdown() was called; execute() fails with
//    Status::NotInitialized until initialize() is called again.
enum class State {
    Uninitialized,
    Ready,
    ShutDown,
};

const char* to_string(State state);

// ---------------------------------------------------------------------------
// Virtual GPU description
// ---------------------------------------------------------------------------

// Configuration fixed before initialize(). Phase 5 keeps this minimal:
// exactly one field, the explicit backend choice. Scheduler options, queue
// priorities, network endpoints etc. belong to later phases and are
// deliberately absent.
struct VirtualGpuDesc {
    // Explicit execution backend, using the Runtime's stable backend names
    // ("cpu", "vulkan"). Default "cpu": the same default the Runtime uses,
    // and the only backend guaranteed to exist on every system. There is no
    // automatic selection anywhere in Phase 5.
    std::string backend = "cpu";
};

// ---------------------------------------------------------------------------
// Virtual GPU
// ---------------------------------------------------------------------------

class VirtualGpu {
public:
    // Creates an Uninitialized Virtual GPU with a default description
    // (backend "cpu"). Initialize before any use.
    // Defined out-of-line (like the destructor) because the Runtime type is
    // deliberately incomplete in this header.
    VirtualGpu();

    // Releases the owned Runtime (which releases all resources first, then
    // all backend/device objects). Safe on Uninitialized/moved-from objects.
    ~VirtualGpu();

    VirtualGpu(const VirtualGpu&) = delete;
    VirtualGpu& operator=(const VirtualGpu&) = delete;

    // Move-only: transferring a live execution context must be explicit,
    // exactly like the resource handles below this layer. After the move the
    // source is Uninitialized with no Runtime; every operation on it fails
    // cleanly.
    VirtualGpu(VirtualGpu&& other) noexcept;
    VirtualGpu& operator=(VirtualGpu&& other) noexcept;

    // Initializes the Virtual GPU with the default description.
    // Equivalent to initialize(VirtualGpuDesc{}).
    Status initialize();

    // Initializes the Virtual GPU with an explicit description:
    //   1. Creates and initializes a fresh Compute Runtime (this also
    //      initializes all built-in backends and registers their buffer
    //      providers with the Resource Manager).
    //   2. Validates the requested backend name. An unknown name is a
    //      configuration error and fails immediately with
    //      Status::BackendUnavailable (the object stays Uninitialized, no
    //      half-initialized state is kept).
    //   3. A KNOWN backend that is unavailable on this system (e.g. "vulkan"
    //      without a Vulkan device) is a normal environment condition, not
    //      an initialization failure: initialize() returns Status::Ok, the
    //      state becomes Ready, and the truth is available through
    //      backend_available() / backend_unavailable_reason(). execute()
    //      will honestly fail with Status::BackendUnavailable until a
    //      usable backend is configured. Nothing ever falls back silently.
    //
    // Calling initialize() while Ready is accepted (Status::Ok) only when
    // the description is unchanged (idempotent, mirroring Runtime); a
    // different backend while Ready returns Status::InvalidInput — call
    // shutdown() first to reconfigure.
    Status initialize(const VirtualGpuDesc& desc);

    // Shuts down the Virtual GPU: the owned Runtime releases all live
    // resources (while backend devices still exist), tears down the
    // backends, and is then destroyed. Every state becomes ShutDown;
    // execute() fails until initialize() is called again. Safe to call
    // multiple times and on Uninitialized/moved-from objects.
    void shutdown();

    // Current lifecycle state (never fabricated).
    State state() const noexcept { return state_; }
    bool is_ready() const noexcept { return state_ == State::Ready; }

    // The backend this Virtual GPU executes on, exactly as configured.
    // Because there is no silent fallback, this is also the backend that
    // actually runs every task.
    const std::string& backend_name() const noexcept { return desc_.backend; }

    // True when the configured backend exists AND is usable on this system.
    // Always false before initialize(); empty of meaning for an
    // Uninitialized object — initialize first, then ask.
    bool backend_available() const;

    // Reason the configured backend is unavailable; empty when it is
    // available (mirrors Runtime::backend_unavailable_reason).
    std::string backend_unavailable_reason() const;

    // The concrete physical device the configured backend executes on
    // (Phase 2 DeviceInfo). A default-constructed (Unknown) DeviceInfo is
    // returned when the Virtual GPU is not Ready or the backend has no
    // device to report (e.g. stub/unavailable Vulkan backend).
    vortyx::device::DeviceInfo device_info() const;

    // Executes vector addition on the configured backend (synchronous).
    // The Runtime performs all task validation and reports unknown or
    // unavailable backends with Status::BackendUnavailable; uninitialized
    // and shut-down Virtual GPUs fail with Status::NotInitialized.
    VectorAddResult execute(const VectorAddTask& task);

    // Resource-based execution (Phase 4 style) through this Virtual GPU:
    // vector addition over three Buffer resources. All three buffers must
    // be live resources of THIS Virtual GPU's configured backend — a
    // Virtual GPU is one explicit execution target, so handing it another
    // backend's buffers is rejected with Status::InvalidInput instead of
    // being silently executed somewhere else.
    ComputeResult execute(const vortyx::resource::Buffer& a,
                          const vortyx::resource::Buffer& b,
                          vortyx::resource::Buffer& c);

    // The Resource Manager of the owned Runtime, for creating buffer
    // resources explicitly. Returns nullptr while the Virtual GPU is not
    // Ready (there is nothing to point at — the manager lives inside the
    // Runtime, which only exists between initialize() and shutdown()).
    // After shutdown(), previously created Buffer handles stay safe: they
    // fail with a clear status and release as a no-op (Phase 4 contract).
    vortyx::resource::ResourceManager* resources() noexcept;

private:
    // The Compute Runtime this Virtual GPU owns. Non-null exactly while the
    // Virtual GPU is initialized (Ready or ShutDown-before-destroy); the
    // Runtime itself owns the backends and the Resource Manager.
    std::unique_ptr<vortyx::compute::Runtime> runtime_;

    // Description the Virtual GPU was (last) initialized with.
    VirtualGpuDesc desc_{};

    State state_ = State::Uninitialized;
};

}  // namespace vortyx::vgpu
