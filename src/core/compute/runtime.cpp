#include "core/compute/runtime.hpp"

#include "core/compute/cpu_backend.hpp"
#include "core/compute/vulkan_backend.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <string_view>

namespace vortyx::compute {

Runtime::~Runtime() {
    shutdown();
}

Status Runtime::initialize() {
    if (initialized_) return Status::Ok;

    // Fresh resource state. For a re-initialization after shutdown() this
    // also drops stale provider references; after this point providers are
    // registered again below. reset() (not shutdown()) keeps the manager
    // usable — shutdown() remains the terminal operation.
    resources_->reset();
    resources_->register_provider(&cpu_provider_);

    backends_.clear();
    backends_.push_back(std::make_unique<CpuBackend>());

    // Vulkan GPU backend: compiled in when the Vulkan loader was found by
    // CMake. Its initialize() returning false is a normal environment
    // condition (no driver / no device), never a program failure.
    backends_.push_back(std::make_unique<VulkanBackend>());

    // The Vulkan backend performs its full device initialization here. A
    // failure is a normal environment condition (no driver / no device).
    for (const auto& backend : backends_) {
        if (backend->name() == std::string("vulkan")) {
            auto* vulkan = static_cast<VulkanBackend*>(backend.get());
            vulkan->initialize();  // result is reported via available()/reason

            // Phase 4: expose the backend's GPU buffer provider to the
            // Resource Manager (only exists when initialization succeeded).
            if (vortyx::resource::IBufferProvider* provider = vulkan->resource_provider()) {
                resources_->register_provider(provider);
            }
        }
    }

    // Report unavailable backends AFTER initialization attempts, so reasons
    // reflect the final state.
    for (const auto& backend : backends_) {
        if (backend->available()) continue;
        if (!backend->unavailable_reason().empty()) {
            vortyx::log(vortyx::LogLevel::Warning,
                        "Backend '" + std::string(backend->name()) + "' unavailable: " +
                            backend->unavailable_reason());
        }
    }

    initialized_ = true;

    std::string available;
    for (const auto& backend : backends_) {
        if (backend->available()) {
            if (!available.empty()) available += ", ";
            available += backend->name();
        }
    }
    std::string providers;
    for (const std::string& entry : resources_->provider_names()) {
        if (!providers.empty()) providers += ", ";
        providers += entry;
    }
    vortyx::log(vortyx::LogLevel::Info,
                "Compute Runtime initialized. Available backends: " + available);
    vortyx::log(vortyx::LogLevel::Info,
                "Resource Manager ready. Buffer providers: " + providers);

    return Status::Ok;
}

void Runtime::shutdown() {
    // Phase 4 ordering: the Resource Manager releases every live buffer
    // FIRST, while the backend devices still exist. Only then are the
    // backends (and their Vulkan device) torn down.
    resources_->shutdown();

    for (const auto& backend : backends_) {
        if (backend->name() == std::string("vulkan")) {
            static_cast<VulkanBackend*>(backend.get())->shutdown();
        }
    }
    backends_.clear();
    initialized_ = false;
}

std::vector<std::string> Runtime::backend_names() const {
    std::vector<std::string> names;
    names.reserve(backends_.size());
    for (const auto& backend : backends_) {
        names.emplace_back(backend->name());
    }
    return names;
}

IComputeBackend* Runtime::find_backend(const std::string& name) const {
    for (const auto& backend : backends_) {
        if (name == backend->name()) return backend.get();
    }
    return nullptr;
}

bool Runtime::has_backend(const std::string& name) const {
    const IComputeBackend* backend = find_backend(name);
    return backend != nullptr && backend->available();
}

std::string Runtime::backend_unavailable_reason(const std::string& name) const {
    const IComputeBackend* backend = find_backend(name);
    if (backend == nullptr) {
        std::string registered;
        for (const auto& entry : backend_names()) {
            if (!registered.empty()) registered += ", ";
            registered += entry;
        }
        return "unknown backend '" + name + "' (registered backends: " + registered + ")";
    }
    return backend->unavailable_reason();
}

vortyx::device::DeviceInfo Runtime::backend_device(const std::string& name) const {
    const IComputeBackend* backend = find_backend(name);
    if (backend == nullptr) return vortyx::device::DeviceInfo{};
    return backend->device_info();
}

VectorAddResult Runtime::execute(const VectorAddTask& task) {
    return execute(task, "cpu");
}

VectorAddResult Runtime::execute(const VectorAddTask& task, const std::string& backend_name) {
    VectorAddResult result;

    if (!initialized_) {
        result.status = Status::NotInitialized;
        result.error = "Runtime is not initialized (call initialize() before execute())";
        return result;
    }

    IComputeBackend* backend = find_backend(backend_name);
    if (backend == nullptr) {
        result.status = Status::BackendUnavailable;
        result.error = "backend '" + backend_name + "' is not supported; " +
                       backend_unavailable_reason(backend_name);
        return result;
    }
    if (!backend->available()) {
        result.status = Status::BackendUnavailable;
        result.error = "backend '" + backend_name + "' is unavailable on this system: " +
                       backend->unavailable_reason();
        return result;
    }

    const Status validation = validate_vector_add(task);
    if (validation != Status::Ok) {
        result.status = validation;
        result.error = "invalid vector addition task (a.size=" +
                       std::to_string(task.a.size()) + ", b.size=" +
                       std::to_string(task.b.size()) + "); arrays must be non-empty and equal size";
        return result;
    }

    // Phase 4: the task's inputs/outputs are expressed as Buffer resources on
    // the requested backend. RAII guarantees they are released on EVERY path
    // below, so no scratch memory (host or GPU) ever leaks.
    const std::size_t count = task.a.size();
    const std::size_t bytes = count * sizeof(std::int32_t);

    const vortyx::resource::BufferDesc desc_a =
        vortyx::resource::BufferDesc::of<std::int32_t>(count, vortyx::resource::ResourceAccess::Read);
    const vortyx::resource::BufferDesc desc_b = desc_a;
    const vortyx::resource::BufferDesc desc_c =
        vortyx::resource::BufferDesc::of<std::int32_t>(count, vortyx::resource::ResourceAccess::Write);

    vortyx::resource::BufferResult ra = resources_->create_buffer(desc_a, backend_name);
    if (ra.status != Status::Ok) {
        result.status = Status::BackendError;
        result.error = "failed to allocate input buffer A on backend '" + backend_name +
                       "': " + ra.error;
        return result;
    }
    vortyx::resource::BufferResult rb = resources_->create_buffer(desc_b, backend_name);
    if (rb.status != Status::Ok) {
        result.status = Status::BackendError;
        result.error = "failed to allocate input buffer B on backend '" + backend_name +
                       "': " + rb.error;
        return result;
    }
    vortyx::resource::BufferResult rc = resources_->create_buffer(desc_c, backend_name);
    if (rc.status != Status::Ok) {
        result.status = Status::BackendError;
        result.error = "failed to allocate output buffer C on backend '" + backend_name +
                       "': " + rc.error;
        return result;
    }

    // Upload the inputs into the resources.
    const ComputeResult wa = ra.buffer.write(task.a.data(), bytes);
    if (wa.status != Status::Ok) {
        result.status = Status::BackendError;
        result.error = "failed to upload input buffer A: " + wa.error;
        return result;
    }
    const ComputeResult wb = rb.buffer.write(task.b.data(), bytes);
    if (wb.status != Status::Ok) {
        result.status = Status::BackendError;
        result.error = "failed to upload input buffer B: " + wb.error;
        return result;
    }

    // Execute directly on the buffers.
    const ComputeResult exec = backend->execute(*resources_->resource(ra.buffer.id()),
                                                *resources_->resource(rb.buffer.id()),
                                                *resources_->resource(rc.buffer.id()));
    if (exec.status != Status::Ok) {
        result.status = exec.status;
        result.error = exec.error;
        return result;
    }

    // Download the result from the output resource.
    result.data.resize(count);
    const ComputeResult rd = rc.buffer.read(result.data.data(), bytes);
    if (rd.status != Status::Ok) {
        result.status = Status::BackendError;
        result.error = "failed to download output buffer: " + rd.error;
        return result;
    }

    result.status = Status::Ok;
    result.error.clear();
    return result;
}

ComputeResult Runtime::execute(const vortyx::resource::Buffer& a,
                               const vortyx::resource::Buffer& b,
                               vortyx::resource::Buffer& c) {
    if (!initialized_) {
        return ComputeResult{Status::NotInitialized,
                             "Runtime is not initialized (call initialize() before execute())"};
    }
    if (resources_->is_shut_down()) {
        return ComputeResult{Status::NotInitialized,
                             "resource manager is shut down (runtime was shut down)"};
    }

    // Resolve the live resources behind the handles. Anything that is not a
    // live resource is rejected with a clear error, never dereferenced.
    const vortyx::resource::IBufferImpl* impl_a = resources_->resource(a.id());
    const vortyx::resource::IBufferImpl* impl_b = resources_->resource(b.id());
    vortyx::resource::IBufferImpl* impl_c = resources_->resource(c.id());
    if (impl_a == nullptr) {
        return ComputeResult{Status::InvalidInput,
                             "buffer 'a' is not a valid live resource (never created, "
                             "moved-from, released, or runtime shut down)"};
    }
    if (impl_b == nullptr) {
        return ComputeResult{Status::InvalidInput,
                             "buffer 'b' is not a valid live resource (never created, "
                             "moved-from, released, or runtime shut down)"};
    }
    if (impl_c == nullptr) {
        return ComputeResult{Status::InvalidInput,
                             "buffer 'c' is not a valid live resource (never created, "
                             "moved-from, released, or runtime shut down)"};
    }

    // All three buffers must belong to the same backend: vector addition is
    // executed by ONE device. Mixing backends is an explicit error (Phase 4
    // never moves data between devices silently).
    if (std::string_view(impl_a->backend_name()) != std::string_view(impl_b->backend_name()) ||
        std::string_view(impl_a->backend_name()) != std::string_view(impl_c->backend_name())) {
        return ComputeResult{
            Status::InvalidInput,
            std::string("buffers belong to different backends (a=") + impl_a->backend_name() +
                ", b=" + impl_b->backend_name() + ", c=" + impl_c->backend_name() +
                "); all three must belong to the same backend"};
    }

    IComputeBackend* backend = find_backend(impl_a->backend_name());
    if (backend == nullptr || !backend->available()) {
        return ComputeResult{
            Status::BackendUnavailable,
            std::string("backend '") + impl_a->backend_name() +
                "' cannot execute (not registered or unavailable on this system)"};
    }

    return backend->execute(*impl_a, *impl_b, *impl_c);
}

}  // namespace vortyx::compute
