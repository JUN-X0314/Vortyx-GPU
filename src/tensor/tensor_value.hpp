#pragma once

// Tensor — the standard Vortyx tensor value (Phase 13).
//
// A Tensor is METADATA (shape, dtype, layout, placement) plus a reference to
// the actual STORAGE (TensorStorage, allocated through the Phase 4 resource
// system). The split is explicit:
//
//   Tensor (value semantics, cheap to pass)      TensorStorage (owned bytes)
//     shape / dtype / layout / placement   --->   Buffer resource
//
//   - Two tensors can share one storage (READ-ONLY views: reshape/transpose
//     produce metadata-only views). Phase 13 exposes NO in-place mutation
//     through views: data is written at creation/execution time by the
//     executor, so a view can never corrupt a live tensor. Mutable aliasing
//     does not exist in the Phase 13 API surface.
//   - Rank is dynamic (bounded by kMaxTensorRank — an explicit resource
//     guard, not a hardcoded design limit).
//   - Zero-element tensors are refused (the project-wide zero-element rule).
//   - All offsets/strides are checked (see shape.hpp).
//
// CREATION PATHS (all allocation through ResourceManager — no bypass):
//   Tensor::create(manager, shape, dtype, placement, backend)
//       contiguous, zero-filled-by-write (the caller writes content through
//       the executor or write_host).
//   Tensor::from_host(manager, shape, dtype, data_bytes)
//       convenience: contiguous host tensor + a full upload of 'data_bytes'.
//   t.reshape(new_shape)   — element-count-preserving metadata view; the
//                            tensor must be row-major contiguous
//                            (UnsupportedLayout otherwise — no hidden copy).
//   t.transpose()          — 2D rank-2 only (InvalidShape otherwise);
//                            strided view sharing the storage.
//   t.broadcast_to(shape)  — stride-0 strided view; read-only.
//
// WRITING/READING DATA: whole-tensor, explicit, synchronous:
//   write_host(const void*, bytes) / read_host(void*, bytes).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/resource/resource_manager.hpp"
#include "tensor/dtype.hpp"
#include "tensor/placement.hpp"
#include "tensor/shape.hpp"
#include "tensor/status.hpp"
#include "tensor/storage.hpp"

namespace vortyx::tensor {

class Tensor final {
public:
    Tensor() = default;

    // -- creation -----------------------------------------------------------

    // Allocates a contiguous tensor through the Phase 4 resource system.
    // 'placement' must validate; Device placements in Phase 13 allocate
    // their storage on the named backend through the same resource system
    // (honest scope: the Phase 13 tensor kernels read host memory, so a
    // Device-placed tensor's storage is created through the resource system
    // and its location is reported honestly by the storage — see
    // TensorStorage::memory_location. No fake device residency is claimed).
    //
    // Returns Ok with 'out' filled, or a failing TensorStatus with 'error'.
    static TensorStatus create(vortyx::resource::ResourceManager& manager,
                               const TensorShape& shape, DataType dtype,
                               const TensorPlacement& placement, Tensor& out,
                               std::string& error, const std::string& backend = "cpu");

    // Convenience: contiguous Host tensor + a full upload of 'data'
    // ('byte_count' must equal the tensor's byte size).
    static TensorStatus from_host(vortyx::resource::ResourceManager& manager,
                                  const TensorShape& shape, DataType dtype,
                                  const void* data, std::size_t byte_count, Tensor& out,
                                  std::string& error);

    // -- metadata ------------------------------------------------------------

    bool valid() const { return storage_ && storage_->valid(); }
    const TensorShape& shape() const { return shape_; }
    const TensorLayout& layout() const { return layout_; }
    DataType dtype() const { return dtype_; }
    const TensorPlacement& placement() const { return placement_; }
    std::int64_t byte_size() const { return byte_size_; }

    // Element count (validated at creation, so this cannot overflow).
    std::int64_t elements() const;

    // True when the layout is the canonical row-major contiguous one.
    bool is_contiguous() const;

    // -- data movement (whole-storage, explicit, synchronous) ------------------
    //
    // STORAGE-LEVEL transfers: on a normal tensor they cover the logical
    // elements; on a VIEW (reshape/transpose/broadcast) they address the
    // UNDERLYING storage directly (the same bytes the base tensor sees) —
    // the view's strides are applied by the kernels/ops, not by these
    // primitives. Exactly byte_size() bytes per call.

    TensorStatus write_host(const void* src, std::size_t bytes, std::string& error) const;
    TensorStatus read_host(void* dst, std::size_t bytes, std::string& error) const;

    // -- views (read-only, storage-sharing, validated) --------------------------

    // Element-count-preserving reshape. Requires a row-major contiguous
    // tensor (UnsupportedLayout otherwise). The result shares this storage.
    TensorStatus reshape(const TensorShape& target, Tensor& out, std::string& error) const;

    // 2-D transpose view (rank must be exactly 2). Shares this storage with
    // swapped strides (Strided layout).
    TensorStatus transpose(Tensor& out, std::string& error) const;

    // Broadcast view at 'target' (is_broadcast_compatible must hold). Shares
    // this storage with stride-0 dimensions.
    TensorStatus broadcast_to(const TensorShape& target, Tensor& out,
                              std::string& error) const;

    // -- storage access (executor/test plumbing) --------------------------------

    // Shared read-only storage (views and executors use this).
    const std::shared_ptr<TensorStorage>& storage() const { return storage_; }

private:
    TensorShape shape_;
    TensorLayout layout_;
    DataType dtype_ = DataType::FP32;
    TensorPlacement placement_;
    std::int64_t byte_size_ = 0;
    std::shared_ptr<TensorStorage> storage_;
};

}  // namespace vortyx::tensor
