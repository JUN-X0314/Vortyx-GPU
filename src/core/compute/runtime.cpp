#include "core/compute/runtime.hpp"

#include "core/compute/cpu_backend.hpp"
#include "core/compute/vulkan_backend.hpp"
#include "core/logger.hpp"

#include <algorithm>

namespace vortyx::compute {

Runtime::~Runtime() {
    shutdown();
}

Status Runtime::initialize() {
    if (initialized_) return Status::Ok;

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
    vortyx::log(vortyx::LogLevel::Info, "Compute Runtime initialized. Available backends: " + available);

    return Status::Ok;
}

void Runtime::shutdown() {
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
    if (!initialized_) {
        VectorAddResult result;
        result.status = Status::NotInitialized;
        result.error = "Runtime is not initialized (call initialize() before execute())";
        return result;
    }

    IComputeBackend* backend = find_backend(backend_name);
    if (backend == nullptr) {
        VectorAddResult result;
        result.status = Status::BackendUnavailable;
        result.error = "backend '" + backend_name + "' is not supported; " +
                       backend_unavailable_reason(backend_name);
        return result;
    }
    if (!backend->available()) {
        VectorAddResult result;
        result.status = Status::BackendUnavailable;
        result.error = "backend '" + backend_name + "' is unavailable on this system: " +
                       backend->unavailable_reason();
        return result;
    }

    return backend->execute(task);
}

}  // namespace vortyx::compute
