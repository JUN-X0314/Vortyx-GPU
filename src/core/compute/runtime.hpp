#pragma once

// Compute Runtime (Phase 3).
//
// Central execution layer: it owns the available backends and executes
// compute tasks on an explicitly chosen backend. There is NO automatic
// scheduling in Phase 3 (Scheduler is a later phase); the default backend
// is "cpu" and must be overridden explicitly.
//
// Lifecycle:
//   Runtime runtime;
//   runtime.initialize();       // registers and initializes backends
//   ... execute(...) ...        // run tasks, check results
//   runtime.shutdown();         // releases all backend resources
//
// GPU backend unavailability is a normal, non-fatal condition: if Vulkan
// cannot initialize, initialize() still succeeds and CPU execution keeps
// working; the reason is exposed via backend_unavailable_reason().

#include <memory>
#include <string>
#include <vector>

#include "core/compute/backend.hpp"
#include "core/compute/task.hpp"
#include "core/device/device.hpp"

namespace vortyx::compute {

class Runtime {
public:
    Runtime() = default;
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Registers the built-in backends ("cpu", "vulkan" when compiled in)
    // and initializes them. Returns Status::Ok even when the GPU backend
    // cannot initialize (that condition is a normal environment state, not
    // a runtime failure).
    Status initialize();

    // Releases all backend resources. Safe to call multiple times.
    void shutdown();

    bool is_initialized() const { return initialized_; }

    // Names of all registered backends (e.g. {"cpu", "vulkan"}).
    std::vector<std::string> backend_names() const;

    // True when the backend exists AND is usable on this system.
    bool has_backend(const std::string& name) const;

    // Reason why the backend is unavailable; empty for available backends.
    std::string backend_unavailable_reason(const std::string& name) const;

    // The concrete device the backend executes on (Phase 2 DeviceInfo).
    vortyx::device::DeviceInfo backend_device(const std::string& name) const;

    // Executes the task on the default backend ("cpu"; Phase 3 performs no
    // automatic device selection).
    VectorAddResult execute(const VectorAddTask& task);

    // Executes the task on an explicitly named backend. Unknown names and
    // unavailable backends return Status::BackendUnavailable with a
    // descriptive error message.
    VectorAddResult execute(const VectorAddTask& task, const std::string& backend_name);

private:
    IComputeBackend* find_backend(const std::string& name) const;

    std::vector<std::unique_ptr<IComputeBackend>> backends_;
    bool initialized_ = false;
};

}  // namespace vortyx::compute
