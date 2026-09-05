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
    // Phase 10: the legacy Phase 3/4 API is the VectorAdd specialization of
    // the generic compute engine. Adapting and delegating keeps validation,
    // task->buffer translation, dispatch and error reporting in exactly ONE
    // place, so the legacy API and the generic engine cannot drift apart.
    ComputeTask generic;
    generic.op = ComputeOp::VectorAdd;
    generic.a = task.a;
    generic.b = task.b;
    const ComputeTaskResult result = execute(generic, backend_name);
    return VectorAddResult{result.status, std::move(result.error), std::move(result.data)};
}

ComputeTaskResult Runtime::execute(const ComputeTask& task) {
    return execute(task, "cpu");
}

ComputeTaskResult Runtime::execute(const ComputeTask& task, const std::string& backend_name) {
    ComputeTaskResult result;

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

    // Strict task validation before anything executes (same policy as the
    // legacy path: an invalid task never reaches a backend).
    std::string validation_error;
    const Status validation = validate_compute_task(task, validation_error);
    if (validation != Status::Ok) {
        result.status = validation;
        result.error = validation_error;
        return result;
    }

    // Translate the task into Buffer resources on the requested backend
    // (allocate -> upload -> dispatch -> download -> release). RAII
    // guarantees they are released on EVERY path below, so no scratch
    // memory (host or GPU) ever leaks.
    const std::size_t count = task.a.size();
    const std::size_t bytes = count * sizeof(std::int32_t);
    const bool two_input = (task.op != ComputeOp::VectorScale);

    const vortyx::resource::BufferDesc desc_a =
        vortyx::resource::BufferDesc::of<std::int32_t>(count, vortyx::resource::ResourceAccess::Read);
    const vortyx::resource::BufferDesc desc_c = vortyx::resource::BufferDesc::of<std::int32_t>(
        count, vortyx::resource::ResourceAccess::Write);

    vortyx::resource::BufferResult ra = resources_->create_buffer(desc_a, backend_name);
    if (ra.status != Status::Ok) {
        result.status = Status::BackendError;
        result.error = "failed to allocate input buffer A on backend '" + backend_name +
                       "': " + ra.error;
        return result;
    }
    vortyx::resource::BufferResult rb;
    if (two_input) {
        rb = resources_->create_buffer(desc_a, backend_name);
        if (rb.status != Status::Ok) {
            result.status = Status::BackendError;
            result.error = "failed to allocate input buffer B on backend '" + backend_name +
                           "': " + rb.error;
            return result;
        }
    }
    vortyx::resource::BufferResult rc = resources_->create_buffer(desc_c, backend_name);
    if (rc.status != Status::Ok) {
        result.status = Status::BackendError;
        result.error = "failed to allocate output buffer C on backend '" + backend_name +
                       "': " + rc.error;
        return result;
    }

    // Upload the input(s) into the resources.
    const ComputeResult wa = ra.buffer.write(task.a.data(), bytes);
    if (wa.status != Status::Ok) {
        result.status = Status::BackendError;
        result.error = "failed to upload input buffer A: " + wa.error;
        return result;
    }
    if (two_input) {
        const ComputeResult wb = rb.buffer.write(task.b.data(), bytes);
        if (wb.status != Status::Ok) {
            result.status = Status::BackendError;
            result.error = "failed to upload input buffer B: " + wb.error;
            return result;
        }
    }

    // Dispatch through the ONE buffer-level compute shape. The buffers were
    // created through THIS manager above, so the resolved pointers are the
    // resources this execution owns (foreign handles cannot enter here).
    ComputeDispatch dispatch;
    dispatch.op = task.op;
    dispatch.scalar = task.scalar;
    dispatch.input_a = resources_->resource(ra.buffer.id());
    dispatch.input_b = two_input ? resources_->resource(rb.buffer.id()) : nullptr;
    dispatch.output = resources_->resource(rc.buffer.id());
    const ComputeResult exec = backend->execute(dispatch);
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

BatchResult Runtime::execute_batch(const std::vector<ComputeTask>& tasks,
                                   const std::string& backend_name) {
    BatchResult batch;

    // Wholesale refusals happen BEFORE any task runs; their reason is the
    // batch's own status/error and 'results' stays empty.
    if (!initialized_) {
        batch.status = Status::NotInitialized;
        batch.error = "Runtime is not initialized (call initialize() before execute_batch())";
        return batch;
    }
    IComputeBackend* backend = find_backend(backend_name);
    if (backend == nullptr) {
        batch.status = Status::BackendUnavailable;
        batch.error = "backend '" + backend_name + "' is not supported; " +
                      backend_unavailable_reason(backend_name);
        return batch;
    }
    if (!backend->available()) {
        batch.status = Status::BackendUnavailable;
        batch.error = "backend '" + backend_name + "' is unavailable on this system: " +
                      backend->unavailable_reason();
        return batch;
    }
    if (tasks.empty()) {
        batch.status = Status::InvalidInput;
        batch.error = "batch execution called with zero tasks (an empty batch has "
                      "nothing to execute)";
        return batch;
    }

    // Every task is attempted, in submission order. Invalid tasks fail as
    // their own item (without executing); earlier failures never stop later
    // tasks; successful results are never discarded.
    batch.results.resize(tasks.size());
    bool any_failed = false;
    std::size_t first_failure = 0;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        batch.results[i] = execute(tasks[i], backend_name);
        if (batch.results[i].status == Status::Ok) {
            ++batch.succeeded;
        } else {
            ++batch.failed;
            if (!any_failed) {
                any_failed = true;
                first_failure = i;
            }
        }
    }

    if (any_failed) {
        // Honest aggregate: the FIRST failing item's own status (no new
        // status vocabulary), with counts and the first failure's reason.
        batch.status = batch.results[first_failure].status;
        batch.error = "batch finished with " + std::to_string(batch.failed) + " of " +
                      std::to_string(tasks.size()) + " task(s) failed; first failure at index " +
                      std::to_string(first_failure) + ": " + batch.results[first_failure].error;
    }
    return batch;
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

    // Phase 9 stability fix — foreign handles must never reach a backend.
    // Resource ids are unique PER MANAGER, not globally: a valid handle from
    // a different Runtime can carry an id that also exists in THIS registry,
    // so resolving it by id alone would silently bind the WRONG storage and
    // return Status::Ok for a computation the caller never asked for. Verify
    // all three handles were created through THIS Runtime's manager first
    // (the backend contract "foreign buffers are rejected with an error,
    // never accessed" is enforced here, in exactly one place).
    const bool foreign_a = !resources_->owns_handle(a);
    const bool foreign_b = !resources_->owns_handle(b);
    const bool foreign_c = !resources_->owns_handle(c);
    if (foreign_a || foreign_b || foreign_c) {
        std::string which;
        if (foreign_a) which += "'a'";
        if (foreign_b) which += std::string(foreign_a ? ", " : "") + "'b'";
        if (foreign_c) which += std::string((foreign_a || foreign_b) ? ", " : "") + "'c'";
        const bool multiple = foreign_a + foreign_b + foreign_c > 1;
        return ComputeResult{
            Status::InvalidInput,
            std::string(multiple ? "buffers " : "buffer ") + which +
                (multiple ? " were" : " was") +
                " not created through this Runtime's Resource Manager (foreign handles "
                "are never executed; create all buffers via the same Runtime's "
                "resources())"};
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
