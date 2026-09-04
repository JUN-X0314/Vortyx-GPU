// Buffer handle implementation (Phase 4).
//
// The Buffer methods live next to the ResourceManager implementation because
// they delegate every real operation to it. All failure paths return an
// explicit status with a human-readable message; none of them throw, and
// none of them can double-free: release always goes through
// ResourceManager::destroy_buffer, which erases the registry entry first.

#include "core/resource/buffer.hpp"

#include "core/resource/resource_manager.hpp"

namespace vortyx::resource {

Buffer::Buffer(std::weak_ptr<ResourceManager> manager, ResourceId id, const BufferDesc& desc,
               MemoryLocation location, const char* backend)
    : manager_(std::move(manager)), id_(id), desc_(desc), location_(location), backend_(backend) {}

Buffer::~Buffer() {
    reset();
}

Buffer::Buffer(Buffer&& other) noexcept
    : manager_(std::move(other.manager_)),
      id_(other.id_),
      desc_(other.desc_),
      location_(other.location_),
      backend_(other.backend_) {
    // Leave the source fully inert: invalid, empty description, no identity.
    other.id_ = kInvalidResourceId;
    other.desc_ = BufferDesc{};
    other.location_ = MemoryLocation::Unknown;
    other.backend_ = "";
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        reset();  // release the resource this handle currently owns (safe no-op if none)
        manager_ = std::move(other.manager_);
        id_ = other.id_;
        desc_ = other.desc_;
        location_ = other.location_;
        backend_ = other.backend_;
        other.id_ = kInvalidResourceId;
        other.desc_ = BufferDesc{};
        other.location_ = MemoryLocation::Unknown;
        other.backend_ = "";
    }
    return *this;
}

bool Buffer::valid() const noexcept {
    return id_ != kInvalidResourceId && !manager_.expired();
}

ComputeResult Buffer::write(const void* src, std::size_t bytes) {
    if (id_ == kInvalidResourceId) {
        return ComputeResult{Status::InvalidInput,
                             "buffer handle is empty (never created, moved-from, or already "
                             "released)"};
    }
    const std::shared_ptr<ResourceManager> manager = manager_.lock();
    if (!manager) {
        return ComputeResult{Status::NotInitialized,
                             "buffer's resource manager no longer exists (runtime was destroyed)"};
    }
    return manager->write_resource(id_, src, bytes);
}

ComputeResult Buffer::read(void* dst, std::size_t bytes) {
    if (id_ == kInvalidResourceId) {
        return ComputeResult{Status::InvalidInput,
                             "buffer handle is empty (never created, moved-from, or already "
                             "released)"};
    }
    const std::shared_ptr<ResourceManager> manager = manager_.lock();
    if (!manager) {
        return ComputeResult{Status::NotInitialized,
                             "buffer's resource manager no longer exists (runtime was destroyed)"};
    }
    return manager->read_resource(id_, dst, bytes);
}

void Buffer::reset() {
    if (id_ == kInvalidResourceId) return;  // already empty; safe to call again
    const std::shared_ptr<ResourceManager> manager = manager_.lock();
    if (manager) {
        manager->destroy_buffer(id_);  // unknown id would be a safe no-op anyway
    }
    id_ = kInvalidResourceId;
    desc_ = BufferDesc{};
    location_ = MemoryLocation::Unknown;
    backend_ = "";
}

}  // namespace vortyx::resource
