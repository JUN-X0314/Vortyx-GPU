#pragma once

// Buffer: the logical, user-facing Vortyx resource handle (Phase 4).
//
// Ownership model (single, explicit, no double free):
//
//   - The ResourceManager REGISTRY owns the real backend storage
//     (IBufferImpl). Exactly one destruction path ever frees it: the manager
//     erasing the registry entry (via Buffer release, explicit
//     destroy_buffer, or the shutdown purge).
//   - A Buffer is a move-only RAII HANDLE into that registry. Copying is
//     deleted (copying a GPU-backed resource is exactly the kind of accident
//     Phase 4 exists to prevent). Moving transfers the handle and leaves the
//     source fully inert.
//   - Releasing a handle (destructor, reset(), move) asks the manager to
//     destroy the resource. Unknown/already-released ids are safe no-ops,
//     so a stale or moved-from handle can never double-free.
//   - The handle observes its manager with a std::weak_ptr: it never keeps
//     the manager (and therefore the Runtime's devices) alive artificially.
//     After Runtime::shutdown() the registry is empty and every remaining
//     handle becomes inert: operations fail with a clear status, destruction
//     is a no-op. A handle whose manager object is already gone behaves the
//     same way.
//
// Data movement is explicit: write() uploads host data into the buffer,
// read() downloads buffer contents to the host. Both validate pointer,
// byte count and state, and both reject zero-size and oversized transfers.

#include <cstddef>
#include <memory>
#include <string>

#include "core/compute/task.hpp"  // Status / ComputeResult (project-wide result style)
#include "core/resource/resource.hpp"

namespace vortyx::resource {

// The resource layer speaks the project-wide result vocabulary from Phase 3
// (one unified error model, no second status system).
using vortyx::compute::ComputeResult;
using vortyx::compute::Status;

class ResourceManager;

class Buffer final {
public:
    // An empty handle: valid() == false, every operation fails cleanly,
    // destruction is a no-op.
    Buffer() = default;

    // RAII release: destroys the underlying resource exactly once.
    ~Buffer();

    // Move-only. Copying is deliberately forbidden for resources whose
    // storage may live in GPU memory.
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    // True while the handle refers to a live resource in a live manager.
    bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }

    // Stable id inside the manager; kInvalidResourceId when empty.
    ResourceId id() const noexcept { return id_; }

    // Description this buffer was created with. On a moved-from handle the
    // description is reset to a default-constructed (invalid) value.
    const BufferDesc& desc() const noexcept { return desc_; }
    std::size_t element_count() const noexcept { return desc_.element_count; }
    std::size_t byte_size() const noexcept { return desc_.byte_size(); }

    // Where the storage actually lives (Host vs Device). Honest per-buffer
    // information, never fabricated.
    MemoryLocation memory_location() const noexcept { return location_; }

    // Backend that owns the storage ("cpu", "vulkan", ...).
    const char* backend_name() const noexcept { return backend_; }

    // Host -> buffer (upload). 'bytes' must be > 0 and <= byte_size().
    // Fails with Status::InvalidInput on null pointer, zero bytes, oversized
    // transfer or an invalid handle; Status::BackendError if the backend
    // transfer itself fails.
    ComputeResult write(const void* src, std::size_t bytes);

    // Buffer -> host (download). Same contract as write().
    ComputeResult read(void* dst, std::size_t bytes);

    // Releases the resource now (the destructor does the same). Safe to call
    // on empty/invalid handles and multiple times.
    void reset();

private:
    friend class ResourceManager;

    Buffer(std::weak_ptr<ResourceManager> manager, ResourceId id, const BufferDesc& desc,
           MemoryLocation location, const char* backend);

    std::weak_ptr<ResourceManager> manager_;  // never keeps the manager alive
    ResourceId id_ = kInvalidResourceId;
    BufferDesc desc_{};
    MemoryLocation location_ = MemoryLocation::Unknown;
    const char* backend_ = "";  // static string owned by the provider class
};

// Result of a resource creation request. 'buffer' is a valid handle only
// when status == Status::Ok; 'error' explains any failure.
// (Defined after Buffer because it owns one by value; move-only member.)
struct BufferResult {
    Buffer buffer;
    Status status = Status::Ok;
    std::string error;  // empty when status == Ok
};

}  // namespace vortyx::resource
