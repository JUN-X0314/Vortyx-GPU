#pragma once

// Backend-owned buffer implementation and provider interfaces (Phase 4).
//
// The separation this header expresses:
//
//   Buffer (logical Vortyx resource, RAII handle)
//     -> ResourceManager (lifecycle: creation, tracking, release)
//       -> IBufferProvider (a backend able to CREATE real storage)
//         -> IBufferImpl (the REAL storage: host memory or a Vulkan buffer)
//
// IBufferImpl is what a backend actually computes on. It is deliberately
// minimal and does NOT try to hide API specifics: the Vulkan implementation
// keeps its VkBuffer, and the Vulkan backend downcasts to it — no fake
// "generic handle" abstraction over API-specific objects.
//
// Upload/download are the ONLY data-movement primitives. Whether a backend
// maps host-visible memory, uses staging copies, or something else is an
// implementation detail behind these two calls.

#include <cstddef>
#include <memory>
#include <string>

#include "core/resource/resource.hpp"

namespace vortyx::resource {

// A real buffer allocation owned by a backend (CPU host memory, Vulkan
// VkBuffer + VkDeviceMemory, ...). Non-copyable by nature: storage has
// exactly one owner. Destroying the object releases the real storage.
class IBufferImpl {
public:
    explicit IBufferImpl(const BufferDesc& desc) : desc_(desc) {}
    virtual ~IBufferImpl() = default;

    IBufferImpl(const IBufferImpl&) = delete;
    IBufferImpl& operator=(const IBufferImpl&) = delete;

    // Description the resource was created with (logical size, access role).
    const BufferDesc& desc() const { return desc_; }

    // Usable logical size in bytes (== desc().byte_size()). Backends may
    // allocate more internally (e.g. Vulkan alignment requirements); the
    // logical size is what validation and stats use.
    std::size_t byte_size() const { return desc_.byte_size(); }
    std::size_t element_count() const { return desc_.element_count; }

    // Backend that owns this storage ("cpu", "vulkan", ...). Same string as
    // the provider's name(); used to route execution and reject foreign
    // buffers with a clear error instead of undefined behavior.
    virtual const char* backend_name() const = 0;

    // Where the storage physically lives. Never faked: a host-memory buffer
    // reports Host, a Vulkan VkDeviceMemory buffer reports Device.
    virtual MemoryLocation memory_location() const = 0;

    // Host -> storage. 'bytes' must be > 0 and <= byte_size(); implementations
    // re-check defensively. Returns false and fills 'error' on failure.
    virtual bool upload(const void* src, std::size_t bytes, std::string& error) = 0;

    // Storage -> host. Same validation contract as upload().
    virtual bool download(void* dst, std::size_t bytes, std::string& error) = 0;

protected:
    BufferDesc desc_;
};

// A backend's buffer factory. The ResourceManager routes creation requests
// to the provider matching the requested backend name; the provider owns
// whatever device context it needs (e.g. the Vulkan logical device).
// Providers are registered NON-OWNING: the Runtime keeps the provider alive
// (the Vulkan provider lives inside the Vulkan backend, the CPU provider
// inside the Runtime) and guarantees they outlive the manager's active
// period. The manager drops all provider references at shutdown().
class IBufferProvider {
public:
    virtual ~IBufferProvider() = default;

    // Stable backend name ("cpu", "vulkan"), matching IComputeBackend::name().
    virtual const char* name() const = 0;

    // True when this provider can currently create buffers.
    virtual bool available() const = 0;

    // Reason when unavailable(); empty otherwise.
    virtual std::string unavailable_reason() const = 0;

    // Creates the real storage for 'desc'. 'desc' is already validated by the
    // ResourceManager, but implementations must stay defensive. Returns
    // nullptr and fills 'error' on failure (allocation failure, no usable
    // memory type, ...). Must never throw.
    virtual std::unique_ptr<IBufferImpl> create_buffer(const BufferDesc& desc,
                                                       std::string& error) = 0;
};

}  // namespace vortyx::resource
