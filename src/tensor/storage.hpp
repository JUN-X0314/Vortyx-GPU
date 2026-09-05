#pragma once

// TensorStorage — the provider-neutral storage of one tensor (Phase 13).
//
// THE MEMORY RULE (the one this module exists to enforce): tensor storage is
// allocated EXCLUSIVELY through the existing Phase 4 memory system
// (ResourceManager -> IBufferProvider -> IBufferImpl). There is NO second
// tensor allocator, NO bypass path, NO separate memory world: every byte a
// tensor holds is a tracked Phase 4 Buffer resource with honest accounting
// (ResourceManager stats), the 1 GiB per-buffer safety cap and overflow
// validation.
//
//   TensorStorage  ---owns--->  vortyx::resource::Buffer  (Phase 4 handle)
//                                     | (weak)
//                                     v
//                              ResourceManager -> IBufferProvider (backend)
//
// What this adds on top of a plain Buffer: the ELEMENT view (dtype width),
// the byte-size computation with checked arithmetic, and an explicit
// synchronous transfer contract. Data movement is EXPLICIT and SYNCHRONOUS
// (write_bytes/read_bytes through Buffer::write/read). Phase 13 has no async
// copy engine — nothing here pretends otherwise.
//
// OWNERSHIP: one TensorStorage owns exactly one Buffer (move-only). Tensors
// share storage through std::shared_ptr<const TensorStorage> for read-only
// views (transpose/reshape) — sharing is documented READ-ONLY at the tensor
// layer (Phase 13 exposes no in-place mutation through views), which is what
// makes sharing safe.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/resource/buffer.hpp"
#include "core/resource/resource_manager.hpp"
#include "tensor/dtype.hpp"
#include "tensor/status.hpp"

namespace vortyx::tensor {

// Checked byte size of 'elements' elements of 'dtype'. False on overflow.
bool tensor_byte_size(std::int64_t elements, DataType dtype, std::int64_t& out);

class TensorStorage final {
public:
    TensorStorage() = default;

    // Move-only (exactly one owner of one Buffer).
    TensorStorage(TensorStorage&& other) noexcept = default;
    TensorStorage& operator=(TensorStorage&& other) noexcept = default;
    TensorStorage(const TensorStorage&) = delete;
    TensorStorage& operator=(const TensorStorage&) = delete;

    // Allocates through the Phase 4 resource system: 'manager' creates a
    // buffer of the requested element count / dtype width / access role on
    // the named backend ("cpu" for tensor storage in Phase 13 — see the
    // module header of tensor.hpp for the honest device-storage scope).
    //
    //   elements < 0                      -> InvalidInput
    //   elements == 0                     -> InvalidShape (the project's
    //                                        zero-element refusal)
    //   byte size overflows / > 1 GiB cap -> ResourceLimitExceeded
    //   allocation refused by the manager -> MemoryAllocationFailure
    //
    // 'backend' must be a canonical backend name; empty = "cpu".
    static void create(vortyx::resource::ResourceManager& manager, std::int64_t elements,
                       DataType dtype, vortyx::resource::ResourceAccess access,
                       const std::string& backend, TensorStorage& out_storage,
                       TensorStatus& out_status, std::string& error);

    bool valid() const { return buffer_.valid(); }

    std::int64_t elements() const { return elements_; }
    DataType dtype() const { return dtype_; }
    std::int64_t byte_size() const { return byte_size_; }
    DataType element_dtype() const { return dtype_; }

    // Honest location/backend reporting straight from the Phase 4 buffer.
    vortyx::resource::MemoryLocation memory_location() const {
        return buffer_.memory_location();
    }
    const char* backend_name() const { return buffer_.backend_name(); }
    vortyx::resource::ResourceAccess access() const { return buffer_.desc().access; }

    // Host -> storage (synchronous, explicit). 'bytes' must be exactly the
    // storage's byte size (the whole-tensor contract keeps partial writes —
    // and the aliasing questions they raise — out of Phase 13).
    TensorStatus write_bytes(const void* src, std::size_t bytes, std::string& error);

    // Storage -> host (synchronous, explicit). Same exact-size contract.
    TensorStatus read_bytes(void* dst, std::size_t bytes, std::string& error);

    // Releases the underlying buffer now (RAII does the same). Safe on
    // invalid storages.
    void reset() { buffer_.reset(); }

    // The underlying Phase 4 resource id (observability / tests). Invalid
    // when empty.
    vortyx::resource::ResourceId resource_id() const { return buffer_.id(); }

private:
    vortyx::resource::Buffer buffer_;
    std::int64_t elements_ = 0;
    DataType dtype_ = DataType::FP32;
    std::int64_t byte_size_ = 0;
};

}  // namespace vortyx::tensor
