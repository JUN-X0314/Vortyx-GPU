#pragma once

// Resource Manager (Phase 4).
//
// The layer between the Runtime and the backends for resource LIFECYCLE:
//
//   Runtime -> ResourceManager -> IBufferProvider (backend) -> real storage
//
// Responsibilities (exactly these, nothing more):
//   - Route buffer creation to the provider of the explicitly requested
//     backend and validate descriptions (sizes, overflow, safety cap).
//   - Track every live resource in a registry with monotonic, never-reused
//     ids (a stale handle can never alias a new resource).
//   - Validate host-side transfers (write/read) and forward them to the
//     owning implementation.
//   - Expose honest stats (live buffers / bytes, allocation count).
//   - Guarantee that shutdown() releases ALL live resources while the
//     owning backend devices still exist, and that after shutdown no
//     resource operation can touch a dead device.
//
// Explicitly NOT its job (later phases): device selection/scheduling, task
// queueing, memory pooling or suballocation, multi-GPU aggregation.
//
// Ownership:
//   - The manager must be owned via std::shared_ptr (the Runtime holds one
//     shared_ptr; Buffer handles observe it weakly). Its registry owns the
//     real backend storage (unique_ptr<IBufferImpl>).
//   - Providers are registered NON-OWNING; the Runtime keeps them alive and
//     the manager drops the references in shutdown().
//
// Threading: Phase 4 is single-threaded by design (no async execution yet).
// All operations assume external serialization.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/resource/backend_buffer.hpp"
#include "core/resource/buffer.hpp"
#include "core/resource/resource.hpp"

namespace vortyx::resource {

// Honest accounting of the manager's allocations. live_* reflect the current
// registry; total_allocations counts every successful create_buffer since
// construction (useful for leak detection in tests and CI logs).
struct ResourceStats {
    std::size_t live_buffers = 0;
    std::size_t live_bytes = 0;
    std::uint64_t total_allocations = 0;
};

class ResourceManager final : public std::enable_shared_from_this<ResourceManager> {
public:
    // Must be created via std::make_shared<ResourceManager>() so Buffer
    // handles can observe it weakly.
    ResourceManager() = default;
    ~ResourceManager();

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    // Registers a NON-OWNING provider. The caller guarantees the provider
    // stays alive until the next shutdown(). Registering a provider whose
    // name is already present is rejected (returns false) to keep routing
    // unambiguous.
    bool register_provider(IBufferProvider* provider);

    std::vector<std::string> provider_names() const;

    // Creates a buffer on the explicitly named backend. This is the ONLY way
    // to obtain a Buffer; there is no implicit device choice anywhere.
    BufferResult create_buffer(const BufferDesc& desc, const std::string& backend_name);

    // Destroys the resource. Unknown / already-released / invalid ids are
    // safe no-ops (this is what makes stale Buffer handles harmless).
    void destroy_buffer(ResourceId id);

    // Registry lookup for execution paths (Runtime + backends). Returns
    // nullptr when the id is unknown. The pointer stays valid until the next
    // resource mutation (create/destroy/shutdown); Phase 4 is single-threaded.
    IBufferImpl* resource(ResourceId id);
    const IBufferImpl* resource(ResourceId id) const;

    // True when 'buffer' is a handle that was created through THIS manager.
    // Resource ids are unique per manager, not globally: a valid handle from
    // a DIFFERENT manager (another Runtime) can carry an id that also exists
    // in this registry, so resolving a foreign handle by id alone could bind
    // the wrong storage. Execution paths verify ownership with this predicate
    // before resolving handles, so a foreign handle is rejected with an
    // explicit error instead of being silently executed on (Phase 9 stability
    // fix, enforcing the "foreign buffers are rejected, never accessed"
    // backend contract).
    bool owns_handle(const Buffer& buffer) const;

    // Validated host -> buffer transfer (null / zero / oversized rejected).
    ComputeResult write_resource(ResourceId id, const void* src, std::size_t bytes);

    // Validated buffer -> host transfer.
    ComputeResult read_resource(ResourceId id, void* dst, std::size_t bytes);

    ResourceStats stats() const { return stats_; }

    // Releases ALL live resources (real storage is freed while the backend
    // devices still exist) and drops all provider references. Every Buffer
    // handle created before shutdown becomes inert afterwards: operations
    // return Status::NotInitialized, destruction is a safe no-op.
    // Idempotent; also called by the destructor.
    void shutdown();

    bool is_shut_down() const { return shut_down_; }

    // Like shutdown(), but the manager returns to a fresh, usable state
    // afterwards. Used by Runtime::initialize() to start (or re-start) from
    // a clean slate; shutdown() remains the terminal operation.
    void reset();

private:
    IBufferProvider* find_provider(const std::string& name) const;

    std::unordered_map<ResourceId, std::unique_ptr<IBufferImpl>> resources_;
    std::vector<IBufferProvider*> providers_;  // non-owning, cleared at shutdown
    ResourceStats stats_{};
    ResourceId next_id_ = 1;  // monotonic; ids are never reused
    bool shut_down_ = false;
};

}  // namespace vortyx::resource
