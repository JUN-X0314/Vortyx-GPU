// TensorStorage (Phase 13) — implementation. All allocation flows through
// the Phase 4 ResourceManager; there is no other allocation path.

#include "tensor/storage.hpp"

#include <cstring>

#include "platform/metadata.hpp"  // is_known_backend
#include "tensor/shape.hpp"       // kMaxTensorBytes (the shared per-tensor cap)

namespace vortyx::tensor {

bool tensor_byte_size(std::int64_t elements, DataType dtype, std::int64_t& out) {
    if (elements < 0) return false;
    const std::int64_t width = static_cast<std::int64_t>(data_type_byte_width(dtype));
    if (width == 0) return false;
    if (elements != 0 && elements > INT64_MAX / width) return false;
    out = elements * width;
    return true;
}

void TensorStorage::create(vortyx::resource::ResourceManager& manager, std::int64_t elements,
                           DataType dtype, vortyx::resource::ResourceAccess access,
                           const std::string& backend, TensorStorage& out_storage,
                           TensorStatus& out_status, std::string& error) {
    out_status = TensorStatus::Ok;
    error.clear();

    if (elements < 0) {
        error = "negative element count";
        out_status = TensorStatus::InvalidInput;
        return;
    }
    if (elements == 0) {
        error = "zero-element tensors are refused (project-wide zero-element rule)";
        out_status = TensorStatus::InvalidShape;
        return;
    }
    const std::string backend_name = backend.empty() ? std::string("cpu") : backend;
    if (!vortyx::platform::is_known_backend(backend_name)) {
        error = "storage backend '" + backend_name + "' is not a canonical backend name";
        out_status = TensorStatus::InvalidPlacement;
        return;
    }
    std::int64_t bytes = 0;
    if (!tensor_byte_size(elements, dtype, bytes)) {
        error = "tensor byte size overflows int64 — refused, never wrapped";
        out_status = TensorStatus::ResourceLimitExceeded;
        return;
    }
    if (bytes > kMaxTensorBytes) {
        error = "tensor byte size (" + std::to_string(bytes) +
                ") exceeds the per-tensor limit (" + std::to_string(kMaxTensorBytes) + ")";
        out_status = TensorStatus::ResourceLimitExceeded;
        return;
    }

    vortyx::resource::BufferDesc desc;
    desc.element_count = static_cast<std::size_t>(elements);
    desc.element_size = data_type_byte_width(dtype);
    desc.access = access;

    vortyx::resource::BufferResult result = manager.create_buffer(desc, backend_name);
    if (result.status != vortyx::compute::Status::Ok) {
        error = "resource system refused the tensor allocation: " + result.error;
        out_status = TensorStatus::MemoryAllocationFailure;
        return;
    }

    out_storage.buffer_ = std::move(result.buffer);
    out_storage.elements_ = elements;
    out_storage.dtype_ = dtype;
    out_storage.byte_size_ = bytes;
}

TensorStatus TensorStorage::write_bytes(const void* src, std::size_t bytes,
                                        std::string& error) {
    if (!valid()) {
        error = "tensor storage is not allocated";
        return TensorStatus::NotInitialized;
    }
    if (bytes == 0 || src == nullptr) {
        error = "null or zero-size tensor write";
        return TensorStatus::InvalidInput;
    }
    if (static_cast<std::int64_t>(bytes) != byte_size_) {
        error = "tensor write must cover the whole storage (" +
                std::to_string(byte_size_) + " bytes), got " + std::to_string(bytes);
        return TensorStatus::InvalidInput;
    }
    const vortyx::compute::ComputeResult result = buffer_.write(src, bytes);
    if (result.status != vortyx::compute::Status::Ok) {
        error = "tensor storage write failed: " + result.error;
        return TensorStatus::ExecutionFailure;
    }
    return TensorStatus::Ok;
}

TensorStatus TensorStorage::read_bytes(void* dst, std::size_t bytes, std::string& error) {
    if (!valid()) {
        error = "tensor storage is not allocated";
        return TensorStatus::NotInitialized;
    }
    if (bytes == 0 || dst == nullptr) {
        error = "null or zero-size tensor read";
        return TensorStatus::InvalidInput;
    }
    if (static_cast<std::int64_t>(bytes) != byte_size_) {
        error = "tensor read must cover the whole storage (" +
                std::to_string(byte_size_) + " bytes), got " + std::to_string(bytes);
        return TensorStatus::InvalidInput;
    }
    const vortyx::compute::ComputeResult result = buffer_.read(dst, bytes);
    if (result.status != vortyx::compute::Status::Ok) {
        error = "tensor storage read failed: " + result.error;
        return TensorStatus::ExecutionFailure;
    }
    return TensorStatus::Ok;
}

}  // namespace vortyx::tensor
