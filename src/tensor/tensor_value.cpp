// Tensor value type (Phase 13) — implementation.

#include "tensor/tensor_value.hpp"

#include <cstring>

namespace vortyx::tensor {

TensorStatus Tensor::create(vortyx::resource::ResourceManager& manager,
                            const TensorShape& shape, DataType dtype,
                            const TensorPlacement& placement, Tensor& out,
                            std::string& error, const std::string& backend) {
    // Metadata validation first (shape rules, placement rules).
    const TensorStatus shape_status = shape.validate(error);
    if (shape_status != TensorStatus::Ok) return shape_status;
    const TensorStatus placement_status = placement.validate(error);
    if (placement_status != TensorStatus::Ok) return placement_status;

    std::int64_t elements = 0;
    if (!shape.total_elements(elements)) {
        error = "tensor element count overflows int64";
        return TensorStatus::ResourceLimitExceeded;
    }
    if (elements == 0) {
        error = "zero-element tensors are refused (shape " + shape.describe() +
                " has a zero dimension; the project refuses zero-element work explicitly)";
        return TensorStatus::InvalidShape;
    }

    std::int64_t bytes = 0;
    if (!tensor_byte_size(elements, dtype, bytes)) {
        error = "tensor byte size overflows int64";
        return TensorStatus::ResourceLimitExceeded;
    }
    if (bytes > kMaxTensorBytes) {
        error = "tensor byte size (" + std::to_string(bytes) +
                ") exceeds the per-tensor limit (" + std::to_string(kMaxTensorBytes) + ")";
        return TensorStatus::ResourceLimitExceeded;
    }

    // Canonical contiguous layout.
    TensorLayout layout = TensorLayout::contiguous(shape, error);
    if (layout.strides.empty() && shape.rank() != 0) {
        return TensorStatus::InvalidStride;  // stride computation overflowed (error filled)
    }

    // Storage through the Phase 4 resource system (the only allocation path).
    // A tensor's storage carries ReadWrite: the same tensor is an input of
    // one op and the output of another (the resource role is the tensor's
    // lifetime role, not a single op's).
    auto storage = std::make_shared<TensorStorage>();
    TensorStatus status = TensorStatus::Ok;
    TensorStorage::create(manager, elements, dtype,
                          vortyx::resource::ResourceAccess::Read |
                              vortyx::resource::ResourceAccess::Write,
                          backend, *storage, status, error);
    if (status != TensorStatus::Ok) return status;

    out.shape_ = shape;
    out.layout_ = std::move(layout);
    out.dtype_ = dtype;
    out.placement_ = placement;
    out.byte_size_ = bytes;
    out.storage_ = std::move(storage);
    return TensorStatus::Ok;
}

TensorStatus Tensor::from_host(vortyx::resource::ResourceManager& manager,
                               const TensorShape& shape, DataType dtype, const void* data,
                               std::size_t byte_count, Tensor& out, std::string& error) {
    TensorStatus status = Tensor::create(manager, shape, dtype, TensorPlacement::host(), out,
                                         error);
    if (status != TensorStatus::Ok) return status;
    return out.write_host(data, byte_count, error);
}

std::int64_t Tensor::elements() const {
    std::int64_t count = 0;
    // The shape was validated at creation; the overflow path cannot occur.
    if (!shape_.total_elements(count)) return 0;
    return count;
}

bool Tensor::is_contiguous() const { return layout_.is_row_major_contiguous_for(shape_); }

TensorStatus Tensor::write_host(const void* src, std::size_t bytes, std::string& error) const {
    if (!storage_) {
        error = "tensor has no storage";
        return TensorStatus::NotInitialized;
    }
    return storage_->write_bytes(src, bytes, error);
}

TensorStatus Tensor::read_host(void* dst, std::size_t bytes, std::string& error) const {
    if (!storage_) {
        error = "tensor has no storage";
        return TensorStatus::NotInitialized;
    }
    return storage_->read_bytes(dst, bytes, error);
}

TensorStatus Tensor::reshape(const TensorShape& target, Tensor& out, std::string& error) const {
    if (!valid()) {
        error = "tensor has no live storage";
        return TensorStatus::NotInitialized;
    }
    const TensorStatus target_status = target.validate(error);
    if (target_status != TensorStatus::Ok) return target_status;
    if (!is_contiguous()) {
        error = "reshape requires a row-major contiguous tensor (this tensor is strided); "
                "copy first via an explicit op — no hidden copy happens here";
        return TensorStatus::UnsupportedLayout;
    }
    std::int64_t source_elements = 0;
    std::int64_t target_elements = 0;
    if (!shape_.total_elements(source_elements) || !target.total_elements(target_elements)) {
        error = "reshape element count computation overflowed";
        return TensorStatus::ResourceLimitExceeded;
    }
    if (source_elements != target_elements) {
        error = "reshape must preserve the element count (" +
                std::to_string(source_elements) + " -> " + std::to_string(target_elements) + ")";
        return TensorStatus::InvalidShape;
    }
    TensorLayout target_layout = TensorLayout::contiguous(target, error);
    if (target_layout.strides.empty() && target.rank() != 0) {
        return TensorStatus::InvalidStride;
    }
    out.shape_ = target;
    out.layout_ = std::move(target_layout);
    out.dtype_ = dtype_;
    out.placement_ = placement_;
    out.byte_size_ = byte_size_;
    out.storage_ = storage_;  // shared READ-ONLY view (no in-place mutation exists)
    return TensorStatus::Ok;
}

TensorStatus Tensor::transpose(Tensor& out, std::string& error) const {
    if (!valid()) {
        error = "tensor has no live storage";
        return TensorStatus::NotInitialized;
    }
    if (shape_.rank() != 2) {
        error = "Phase 13 transpose is defined for rank-2 tensors only (got rank " +
                std::to_string(shape_.rank()) + ")";
        return TensorStatus::InvalidShape;
    }
    Tensor transposed;
    transposed.shape_ = TensorShape::make({shape_.dims[1], shape_.dims[0]});
    transposed.layout_.kind = LayoutKind::Strided;
    transposed.layout_.strides = {layout_.strides[1], layout_.strides[0]};
    transposed.dtype_ = dtype_;
    transposed.placement_ = placement_;
    transposed.byte_size_ = byte_size_;
    transposed.storage_ = storage_;  // shared READ-ONLY view
    // Validate the swapped strides against the SAME storage span.
    const TensorStatus status = transposed.layout_.validate(
        transposed.shape_, storage_->elements(), error);
    if (status != TensorStatus::Ok) return status;
    out = std::move(transposed);
    return TensorStatus::Ok;
}

TensorStatus Tensor::broadcast_to(const TensorShape& target, Tensor& out,
                                  std::string& error) const {
    if (!valid()) {
        error = "tensor has no live storage";
        return TensorStatus::NotInitialized;
    }
    const TensorStatus target_status = target.validate(error);
    if (target_status != TensorStatus::Ok) return target_status;
    if (!is_broadcast_compatible(shape_, target)) {
        error = "shape " + shape_.describe() + " cannot be broadcast to " + target.describe();
        return TensorStatus::InvalidShape;
    }
    Tensor broadcast;
    broadcast.shape_ = target;
    const TensorStatus stride_status =
        broadcast_strides(shape_, layout_, target, broadcast.layout_, error);
    if (stride_status != TensorStatus::Ok) return stride_status;
    broadcast.dtype_ = dtype_;
    broadcast.placement_ = placement_;
    std::int64_t target_elements = 0;
    if (!target.total_elements(target_elements)) {
        error = "broadcast target element count overflows";
        return TensorStatus::ResourceLimitExceeded;
    }
    if (!tensor_byte_size(target_elements, dtype_, broadcast.byte_size_)) {
        error = "broadcast byte size overflows";
        return TensorStatus::ResourceLimitExceeded;
    }
    broadcast.storage_ = storage_;  // shared READ-ONLY view (stride-0 broadcast)
    // Broadcast strides reach a SUBSET of the storage (stride-0 dims repeat) —
    // validation uses the storage span as the bound.
    const TensorStatus layout_status =
        broadcast.layout_.validate(target, storage_->elements(), error);
    if (layout_status != TensorStatus::Ok) return layout_status;
    out = std::move(broadcast);
    return TensorStatus::Ok;
}

}  // namespace vortyx::tensor
