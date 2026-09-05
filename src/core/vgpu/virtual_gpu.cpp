// Virtual GPU implementation (Phase 5).
//
// The Virtual GPU is a thin, honest layer ON TOP of the existing stack: it
// owns one Compute Runtime, forwards execution to the explicitly configured
// backend, and adds lifecycle gating + state reporting. All real work
// (backend management, task -> buffer translation, resource lifecycle,
// Vulkan details) stays where Phase 3/4 put it — nothing is duplicated here.
//
// Failure philosophy: every unusable condition is reported through the
// project-wide Status vocabulary with a human-readable reason. The Virtual
// GPU never crashes, never throws, and never silently substitutes a
// different backend than the one it was configured with.

#include "core/vgpu/virtual_gpu.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include "core/compute/runtime.hpp"
#include "core/logger.hpp"
#include "core/resource/buffer.hpp"
#include "core/resource/resource_manager.hpp"

namespace vortyx::vgpu {

VirtualGpu::VirtualGpu() = default;

const char* to_string(State state) {
    switch (state) {
        case State::Uninitialized: return "Uninitialized";
        case State::Ready: return "Ready";
        case State::ShutDown: return "ShutDown";
    }
    return "Unknown";
}

VirtualGpu::~VirtualGpu() {
    shutdown();
}

VirtualGpu::VirtualGpu(VirtualGpu&& other) noexcept
    : runtime_(std::move(other.runtime_)),
      desc_(std::move(other.desc_)),
      state_(other.state_) {
    // Leave the source fully inert: no Runtime, no configuration, and a
    // state that honestly reflects that (execute() fails cleanly on it).
    other.desc_ = VirtualGpuDesc{};
    other.state_ = State::Uninitialized;
}

VirtualGpu& VirtualGpu::operator=(VirtualGpu&& other) noexcept {
    if (this != &other) {
        // Release what this object currently owns BEFORE taking over the
        // other's Runtime (shutdown() is a safe no-op when there is none).
        shutdown();
        runtime_ = std::move(other.runtime_);
        desc_ = std::move(other.desc_);
        state_ = other.state_;
        other.desc_ = VirtualGpuDesc{};
        other.state_ = State::Uninitialized;
    }
    return *this;
}

Status VirtualGpu::initialize() {
    return initialize(VirtualGpuDesc{});
}

Status VirtualGpu::initialize(const VirtualGpuDesc& desc) {
    // Already Ready: accept an identical (idempotent) re-initialization the
    // same way Runtime does; a different backend is a configuration change
    // and must go through shutdown() first.
    if (state_ == State::Ready && runtime_ != nullptr) {
        if (desc.backend == desc_.backend) {
            return Status::Ok;
        }
        // Refuse the reconfiguration explicitly; the Virtual GPU stays Ready
        // with its current backend.
        vortyx::log(vortyx::LogLevel::Error,
                    "Virtual GPU: already initialized with backend '" + desc_.backend +
                        "'; call shutdown() before reconfiguring to '" + desc.backend + "'.");
        return Status::InvalidInput;
    }

    // Fresh Runtime on every (re-)initialization: after a shutdown() the old
    // Runtime is gone, and after a failed configuration below nothing is
    // kept, so there is never a half-initialized object to reason about.
    runtime_ = std::make_unique<vortyx::compute::Runtime>();
    if (runtime_->initialize() != Status::Ok) {
        // Cannot happen with the built-in backends (Runtime::initialize is
        // designed to always succeed), but stay defensive: report honestly
        // and leave the object Uninitialized.
        vortyx::log(vortyx::LogLevel::Error, "Virtual GPU: Compute Runtime failed to initialize.");
        runtime_.reset();
        state_ = State::Uninitialized;
        return Status::NotInitialized;
    }

    // Backend names are validated against the Runtime's registered backends.
    // An unknown name is a caller configuration error ("cuda", a typo, ...)
    // and fails here, early — never silently at first execute().
    const std::vector<std::string> registered = runtime_->backend_names();
    if (std::find(registered.begin(), registered.end(), desc.backend) == registered.end()) {
        std::string known;
        for (const std::string& name : registered) {
            if (!known.empty()) known += ", ";
            known += name;
        }
        vortyx::log(vortyx::LogLevel::Error,
                    "Virtual GPU: unknown backend '" + desc.backend +
                        "' (registered backends: " + known + ").");
        runtime_.reset();
        state_ = State::Uninitialized;
        return Status::BackendUnavailable;
    }

    desc_ = desc;
    state_ = State::Ready;

    // Honest environment reporting: a known-but-unavailable backend is a
    // normal condition (no GPU / no driver / stub build). The Virtual GPU
    // stays usable, execute() will fail with a clear reason, and nothing
    // falls back automatically — choosing another backend is the caller's
    // explicit decision.
    if (!runtime_->has_backend(desc_.backend)) {
        vortyx::log(vortyx::LogLevel::Warning,
                    "Virtual GPU: backend '" + desc_.backend +
                        "' is unavailable on this system: " +
                        runtime_->backend_unavailable_reason(desc_.backend));
        return Status::Ok;
    }

    const vortyx::device::DeviceInfo device = runtime_->backend_device(desc_.backend);
    vortyx::log(vortyx::LogLevel::Info,
                "Virtual GPU initialized (backend: " + desc_.backend +
                    ", state: Ready, device: " +
                    (device.name.empty() ? std::string("unknown") : device.name) + ")");
    return Status::Ok;
}

void VirtualGpu::shutdown() {
    if (runtime_ != nullptr) {
        // Runtime::shutdown releases all live resources first (while the
        // backend devices still exist), then tears the backends down. The
        // Runtime destructor also calls shutdown(); destroying it below is
        // therefore safe and can never double-free anything.
        runtime_->shutdown();
        runtime_.reset();
        vortyx::log(vortyx::LogLevel::Info, "Virtual GPU shut down.");
    }
    state_ = State::ShutDown;
}

bool VirtualGpu::backend_available() const {
    if (state_ != State::Ready || runtime_ == nullptr) {
        return false;
    }
    return runtime_->has_backend(desc_.backend);
}

std::string VirtualGpu::backend_unavailable_reason() const {
    if (state_ != State::Ready || runtime_ == nullptr) {
        return "Virtual GPU is not initialized (state: " + std::string(to_string(state_)) + ")";
    }
    return runtime_->backend_unavailable_reason(desc_.backend);
}

vortyx::device::DeviceInfo VirtualGpu::device_info() const {
    if (state_ != State::Ready || runtime_ == nullptr) {
        return vortyx::device::DeviceInfo{};
    }
    return runtime_->backend_device(desc_.backend);
}

VectorAddResult VirtualGpu::execute(const VectorAddTask& task) {
    // ShutDown is checked first: after shutdown() the Runtime pointer is
    // null too, and the caller deserves the precise reason, not a generic
    // "not initialized".
    if (state_ == State::ShutDown) {
        return VectorAddResult{Status::NotInitialized,
                               "Virtual GPU is shut down "
                               "(call initialize() again before execute())",
                               {}};
    }
    if (state_ == State::Uninitialized || runtime_ == nullptr) {
        return VectorAddResult{Status::NotInitialized,
                               "Virtual GPU is not initialized "
                               "(call initialize() before execute())",
                               {}};
    }

    // Straight delegation to the Runtime on the configured backend. The
    // Runtime validates the task and the backend state and reports unknown
    // or unavailable backends with Status::BackendUnavailable — the Virtual
    // GPU never reroutes a task to a different backend on its own.
    return runtime_->execute(task, desc_.backend);
}

ComputeTaskResult VirtualGpu::execute(const ComputeTask& task) {
    // ShutDown is checked first: after shutdown() the Runtime pointer is
    // null too, and the caller deserves the precise reason, not a generic
    // "not initialized". Same gating as execute(VectorAddTask) above.
    if (state_ == State::ShutDown) {
        return ComputeTaskResult{Status::NotInitialized,
                                 "Virtual GPU is shut down "
                                 "(call initialize() again before execute())",
                                 {}};
    }
    if (state_ == State::Uninitialized || runtime_ == nullptr) {
        return ComputeTaskResult{Status::NotInitialized,
                                 "Virtual GPU is not initialized "
                                 "(call initialize() before execute())",
                                 {}};
    }

    // Straight delegation to the Runtime on the configured backend — the
    // generic engine path behind the unchanged lifecycle rules. No
    // automatic backend choice, no silent fallback.
    return runtime_->execute(task, desc_.backend);
}

BatchResult VirtualGpu::execute_batch(const std::vector<ComputeTask>& tasks) {
    BatchResult batch;
    if (state_ == State::ShutDown) {
        batch.status = Status::NotInitialized;
        batch.error = "Virtual GPU is shut down "
                      "(call initialize() again before execute_batch())";
        return batch;
    }
    if (state_ == State::Uninitialized || runtime_ == nullptr) {
        batch.status = Status::NotInitialized;
        batch.error = "Virtual GPU is not initialized "
                      "(call initialize() before execute_batch())";
        return batch;
    }
    return runtime_->execute_batch(tasks, desc_.backend);
}

ComputeResult VirtualGpu::execute(const vortyx::resource::Buffer& a,
                                  const vortyx::resource::Buffer& b,
                                  vortyx::resource::Buffer& c) {
    // ShutDown first: see execute(const VectorAddTask&) for the reasoning.
    if (state_ == State::ShutDown) {
        return ComputeResult{Status::NotInitialized,
                             "Virtual GPU is shut down "
                             "(call initialize() again before execute())"};
    }
    if (state_ == State::Uninitialized || runtime_ == nullptr) {
        return ComputeResult{Status::NotInitialized,
                             "Virtual GPU is not initialized "
                             "(call initialize() before execute())"};
    }

    // A Virtual GPU is ONE explicit execution target. Buffers created on any
    // other backend (or dead/moved-from/released handles) are rejected with
    // a clear error instead of being executed somewhere else silently.
    const std::string_view configured(desc_.backend);
    if (!a.valid() || std::string_view(a.backend_name()) != configured) {
        return ComputeResult{
            Status::InvalidInput,
            "buffer 'a' is not a live resource of backend '" + desc_.backend +
                "' (never created, moved-from, released, shut down, or created on a "
                "different backend)"};
    }
    if (!b.valid() || std::string_view(b.backend_name()) != configured) {
        return ComputeResult{
            Status::InvalidInput,
            "buffer 'b' is not a live resource of backend '" + desc_.backend +
                "' (never created, moved-from, released, shut down, or created on a "
                "different backend)"};
    }
    if (!c.valid() || std::string_view(c.backend_name()) != configured) {
        return ComputeResult{
            Status::InvalidInput,
            "buffer 'c' is not a live resource of backend '" + desc_.backend +
                "' (never created, moved-from, released, shut down, or created on a "
                "different backend)"};
    }

    return runtime_->execute(a, b, c);
}

vortyx::resource::ResourceManager* VirtualGpu::resources() noexcept {
    if (state_ != State::Ready || runtime_ == nullptr) {
        return nullptr;
    }
    return &runtime_->resources();
}

}  // namespace vortyx::vgpu
