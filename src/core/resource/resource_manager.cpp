// Resource Manager implementation (Phase 4).
//
// Lifecycle guarantee: shutdown() releases every live resource BEFORE the
// Runtime tears down backend devices, so real storage (including Vulkan
// buffers and device memory) is always freed through its owning backend
// while that backend is still valid. After shutdown the registry is empty,
// provider references are dropped, and every operation fails explicitly.

#include "core/resource/resource_manager.hpp"

#include "core/logger.hpp"

namespace vortyx::resource {

ResourceManager::~ResourceManager() {
    shutdown();
}

bool ResourceManager::register_provider(IBufferProvider* provider) {
    if (provider == nullptr) return false;
    if (shut_down_) return false;
    if (find_provider(provider->name()) != nullptr) {
        // Duplicate backend names would make routing ambiguous.
        vortyx::log(vortyx::LogLevel::Warning,
                    std::string("Resource Manager: provider '") + provider->name() +
                        "' is already registered; ignoring duplicate registration");
        return false;
    }
    providers_.push_back(provider);
    return true;
}

std::vector<std::string> ResourceManager::provider_names() const {
    std::vector<std::string> names;
    names.reserve(providers_.size());
    for (const IBufferProvider* provider : providers_) {
        names.emplace_back(provider->name());
    }
    return names;
}

IBufferProvider* ResourceManager::find_provider(const std::string& name) const {
    for (IBufferProvider* provider : providers_) {
        if (name == provider->name()) return provider;
    }
    return nullptr;
}

BufferResult ResourceManager::create_buffer(const BufferDesc& desc,
                                            const std::string& backend_name) {
    if (shut_down_) {
        return BufferResult{Buffer{}, Status::NotInitialized,
                            "resource manager is shut down; no buffers can be created"};
    }

    const std::string desc_error = validate_buffer_desc(desc);
    if (!desc_error.empty()) {
        return BufferResult{Buffer{}, Status::InvalidInput, desc_error};
    }

    IBufferProvider* provider = find_provider(backend_name);
    if (provider == nullptr) {
        std::string registered;
        for (const std::string& entry : provider_names()) {
            if (!registered.empty()) registered += ", ";
            registered += entry;
        }
        return BufferResult{Buffer{}, Status::BackendUnavailable,
                            "no buffer provider registered for backend '" + backend_name +
                                "' (registered providers: " + registered + ")"};
    }
    if (!provider->available()) {
        return BufferResult{Buffer{}, Status::BackendUnavailable,
                            "backend '" + backend_name +
                                "' cannot provide buffers: " + provider->unavailable_reason()};
    }

    std::string error;
    std::unique_ptr<IBufferImpl> impl = provider->create_buffer(desc, error);
    if (impl == nullptr) {
        return BufferResult{Buffer{}, Status::BackendError,
                            "backend '" + backend_name + "' failed to allocate a buffer of " +
                                std::to_string(desc.byte_size()) + " bytes" +
                                (error.empty() ? "" : ": " + error)};
    }

    const ResourceId id = next_id_++;
    const std::size_t bytes = impl->byte_size();
    resources_[id] = std::move(impl);

    ++stats_.total_allocations;
    ++stats_.live_buffers;
    stats_.live_bytes += bytes;

    return BufferResult{
        Buffer(weak_from_this(), id, desc, resources_[id]->memory_location(),
               resources_[id]->backend_name()),
        Status::Ok, {}};
}

void ResourceManager::destroy_buffer(ResourceId id) {
    if (id == kInvalidResourceId) return;  // safe no-op
    const auto it = resources_.find(id);
    if (it == resources_.end()) return;    // stale / foreign handle: safe no-op

    const std::size_t bytes = it->second->byte_size();
    resources_.erase(it);  // frees the real backend storage exactly once

    --stats_.live_buffers;
    stats_.live_bytes -= bytes;
}

IBufferImpl* ResourceManager::resource(ResourceId id) {
    if (id == kInvalidResourceId) return nullptr;
    const auto it = resources_.find(id);
    return it == resources_.end() ? nullptr : it->second.get();
}

const IBufferImpl* ResourceManager::resource(ResourceId id) const {
    if (id == kInvalidResourceId) return nullptr;
    const auto it = resources_.find(id);
    return it == resources_.end() ? nullptr : it->second.get();
}

ComputeResult ResourceManager::write_resource(ResourceId id, const void* src,
                                              std::size_t bytes) {
    if (shut_down_) {
        return ComputeResult{Status::NotInitialized,
                             "resource manager is shut down; buffer handles are no longer usable"};
    }
    IBufferImpl* impl = resource(id);
    if (impl == nullptr) {
        return ComputeResult{Status::InvalidInput,
                             "buffer handle is not valid (already released, moved-from, or unknown)"};
    }
    if (src == nullptr) {
        return ComputeResult{Status::InvalidInput, "null data pointer passed to buffer write"};
    }
    if (bytes == 0) {
        return ComputeResult{Status::InvalidInput,
                             "zero-byte write is rejected by policy (nothing to upload)"};
    }
    if (bytes > impl->byte_size()) {
        return ComputeResult{Status::InvalidInput,
                             "write of " + std::to_string(bytes) + " bytes exceeds buffer size of " +
                                 std::to_string(impl->byte_size()) + " bytes"};
    }

    std::string error;
    if (!impl->upload(src, bytes, error)) {
        return ComputeResult{Status::BackendError,
                             "buffer upload failed" + (error.empty() ? "" : ": " + error)};
    }
    return ComputeResult{Status::Ok, {}};
}

ComputeResult ResourceManager::read_resource(ResourceId id, void* dst, std::size_t bytes) {
    if (shut_down_) {
        return ComputeResult{Status::NotInitialized,
                             "resource manager is shut down; buffer handles are no longer usable"};
    }
    IBufferImpl* impl = resource(id);
    if (impl == nullptr) {
        return ComputeResult{Status::InvalidInput,
                             "buffer handle is not valid (already released, moved-from, or unknown)"};
    }
    if (dst == nullptr) {
        return ComputeResult{Status::InvalidInput, "null data pointer passed to buffer read"};
    }
    if (bytes == 0) {
        return ComputeResult{Status::InvalidInput,
                             "zero-byte read is rejected by policy (nothing to download)"};
    }
    if (bytes > impl->byte_size()) {
        return ComputeResult{Status::InvalidInput,
                             "read of " + std::to_string(bytes) + " bytes exceeds buffer size of " +
                                 std::to_string(impl->byte_size()) + " bytes"};
    }

    std::string error;
    if (!impl->download(dst, bytes, error)) {
        return ComputeResult{Status::BackendError,
                             "buffer download failed" + (error.empty() ? "" : ": " + error)};
    }
    return ComputeResult{Status::Ok, {}};
}

void ResourceManager::reset() {
    const std::size_t released = resources_.size();
    std::size_t released_bytes = 0;
    for (const auto& entry : resources_) {
        released_bytes += entry.second->byte_size();
    }
    // Erasing frees the real backend storage through the implementations'
    // destructors. This runs while the backend devices are still alive
    // (Runtime::shutdown order guarantees it).
    resources_.clear();
    providers_.clear();  // non-owning references only
    shut_down_ = false;  // reset() returns the manager to a usable state

    stats_.live_buffers = 0;
    stats_.live_bytes = 0;

    if (released > 0) {
        vortyx::log(vortyx::LogLevel::Info,
                    "Resource Manager: released " + std::to_string(released) +
                        " live buffer(s) (" + std::to_string(released_bytes) + " bytes)");
    }
}

void ResourceManager::shutdown() {
    const std::size_t released = resources_.size();
    reset();
    shut_down_ = true;

    if (released > 0) {
        vortyx::log(vortyx::LogLevel::Info,
                    "Resource Manager: released " + std::to_string(released) +
                        " live buffer(s) at shutdown (state is now terminal)");
    }
}

}  // namespace vortyx::resource
