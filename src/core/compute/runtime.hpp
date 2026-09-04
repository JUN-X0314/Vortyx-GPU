#pragma once

// Compute Runtime (Phase 4).
//
// Central execution layer. It owns the available backends AND the Resource
// Manager, and it is the single place where a compute task is translated
// into resources:
//
//   Application -> Runtime -> ResourceManager -> Backend -> Device
//
// Lifecycle:
//   Runtime runtime;
//   runtime.initialize();       // registers backends + buffer providers
//   ... execute(...) ...        // run tasks, check results
//   runtime.shutdown();         // releases all resources, then all backends
//
// Two execution styles coexist (same code path underneath):
//   1. Task-based (Phase 3 API, unchanged): execute(VectorAddTask) — the
//      Runtime allocates the task's input/output buffers through the
//      Resource Manager, uploads the inputs, dispatches, downloads the
//      result, and releases everything (RAII).
//   2. Resource-based (Phase 4 API): create_buffer() + execute(a, b, c) —
//      the caller owns the buffer lifecycle explicitly: create -> write ->
//      execute -> read -> release. Results stay inside the output buffer
//      until the caller reads them.
//
// GPU backend unavailability is a normal, non-fatal condition: if Vulkan
// cannot initialize, initialize() still succeeds and CPU execution keeps
// working; the reason is exposed via backend_unavailable_reason().
//
// Phase 4 adds NO scheduling: the default backend is "cpu" and device choice
// is always explicit. Scheduler / Task Queue / Virtual GPU / Multi-GPU are
// later phases.

#include <memory>
#include <string>
#include <vector>

#include "core/compute/backend.hpp"
#include "core/compute/task.hpp"
#include "core/device/device.hpp"
#include "core/resource/buffer.hpp"
#include "core/resource/cpu_buffer.hpp"
#include "core/resource/resource_manager.hpp"

namespace vortyx::compute {

class Runtime {
public:
    Runtime() = default;
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Registers the built-in backends ("cpu", "vulkan" when compiled in),
    // initializes them, and registers the backends' buffer providers with
    // the Resource Manager. Returns Status::Ok even when the GPU backend
    // cannot initialize (that condition is a normal environment state, not
    // a runtime failure).
    Status initialize();

    // Releases all resources (Resource Manager first — buffer storage is
    // freed while devices still exist), then all backend resources.
    // Safe to call multiple times.
    void shutdown();

    bool is_initialized() const { return initialized_; }

    // The Resource Manager. Its lifetime equals the Runtime's; after
    // shutdown() it rejects all operations with Status::NotInitialized and
    // every previously created Buffer handle becomes inert.
    vortyx::resource::ResourceManager& resources() { return *resources_; }

    // Names of all registered backends (e.g. {"cpu", "vulkan"}).
    std::vector<std::string> backend_names() const;

    // True when the backend exists AND is usable on this system.
    bool has_backend(const std::string& name) const;

    // Reason why the backend is unavailable; empty for available backends.
    std::string backend_unavailable_reason(const std::string& name) const;

    // The concrete device the backend executes on (Phase 2 DeviceInfo).
    vortyx::device::DeviceInfo backend_device(const std::string& name) const;

    // Executes the task on the default backend ("cpu"; Phase 4 still performs
    // no automatic device selection).
    VectorAddResult execute(const VectorAddTask& task);

    // Executes the task on an explicitly named backend. The Runtime turns
    // the task into buffer resources on that backend (allocate -> upload ->
    // dispatch -> download -> release). Unknown names and unavailable
    // backends return Status::BackendUnavailable with a descriptive error.
    VectorAddResult execute(const VectorAddTask& task, const std::string& backend_name);

    // Resource-based execution (Phase 4): vector addition over three Buffer
    // resources that the caller created through resources().create_buffer().
    // Reads a and b, writes c; the result stays inside c until the caller
    // downloads it with c.read(). All three buffers must belong to the same
    // backend; access roles and sizes are validated.
    ComputeResult execute(const vortyx::resource::Buffer& a, const vortyx::resource::Buffer& b,
                          vortyx::resource::Buffer& c);

private:
    IComputeBackend* find_backend(const std::string& name) const;

    std::vector<std::unique_ptr<IComputeBackend>> backends_;

    // Resource management (Phase 4). The manager is shared-owned so Buffer
    // handles can observe it weakly without keeping the Runtime alive.
    std::shared_ptr<vortyx::resource::ResourceManager> resources_ =
        std::make_shared<vortyx::resource::ResourceManager>();

    // Always-available host-memory buffer provider ("cpu"). Registered with
    // the manager on initialize(); outlives the manager's active period.
    vortyx::resource::CpuBufferProvider cpu_provider_;

    bool initialized_ = false;
};

}  // namespace vortyx::compute
