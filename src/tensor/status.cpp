// Tensor layer status vocabulary (Phase 13) — implementation.

#include "tensor/status.hpp"

namespace vortyx::tensor {

const char* to_string(TensorStatus status) {
    switch (status) {
        case TensorStatus::Ok: return "Ok";
        case TensorStatus::InvalidInput: return "InvalidInput";
        case TensorStatus::InvalidShape: return "InvalidShape";
        case TensorStatus::InvalidStride: return "InvalidStride";
        case TensorStatus::DtypeMismatch: return "DtypeMismatch";
        case TensorStatus::UnsupportedDtype: return "UnsupportedDtype";
        case TensorStatus::UnsupportedOperation: return "UnsupportedOperation";
        case TensorStatus::UnsupportedLayout: return "UnsupportedLayout";
        case TensorStatus::InvalidPlacement: return "InvalidPlacement";
        case TensorStatus::ResourceLimitExceeded: return "ResourceLimitExceeded";
        case TensorStatus::MemoryAllocationFailure: return "MemoryAllocationFailure";
        case TensorStatus::InvalidState: return "InvalidState";
        case TensorStatus::DeviceCapabilityMismatch: return "DeviceCapabilityMismatch";
        case TensorStatus::TransferUnsupported: return "TransferUnsupported";
        case TensorStatus::ExecutionFailure: return "ExecutionFailure";
        case TensorStatus::NumericalValidationFailure: return "NumericalValidationFailure";
        case TensorStatus::NotInitialized: return "NotInitialized";
        case TensorStatus::Internal: return "Internal";
    }
    return "Unknown";
}

const char* tensor_status_code(TensorStatus status) {
    switch (status) {
        case TensorStatus::Ok: return "ok";
        case TensorStatus::InvalidInput: return "invalid_input";
        case TensorStatus::InvalidShape: return "invalid_shape";
        case TensorStatus::InvalidStride: return "invalid_stride";
        case TensorStatus::DtypeMismatch: return "dtype_mismatch";
        case TensorStatus::UnsupportedDtype: return "unsupported_dtype";
        case TensorStatus::UnsupportedOperation: return "unsupported_operation";
        case TensorStatus::UnsupportedLayout: return "unsupported_layout";
        case TensorStatus::InvalidPlacement: return "invalid_placement";
        case TensorStatus::ResourceLimitExceeded: return "resource_limit_exceeded";
        case TensorStatus::MemoryAllocationFailure: return "memory_allocation_failure";
        case TensorStatus::InvalidState: return "invalid_state";
        case TensorStatus::DeviceCapabilityMismatch: return "device_capability_mismatch";
        case TensorStatus::TransferUnsupported: return "transfer_unsupported";
        case TensorStatus::ExecutionFailure: return "execution_failure";
        case TensorStatus::NumericalValidationFailure: return "numerical_validation_failure";
        case TensorStatus::NotInitialized: return "not_initialized";
        case TensorStatus::Internal: return "internal";
    }
    return "unknown";
}

bool tensor_status_from_code(const std::string& code, TensorStatus& out) {
    // Linear scan over the full vocabulary: the table is small, the scan is
    // deterministic, and an unknown code is refused (never guessed).
    static const struct {
        TensorStatus status;
        const char* code;
    } kTable[] = {
        {TensorStatus::Ok, "ok"},
        {TensorStatus::InvalidInput, "invalid_input"},
        {TensorStatus::InvalidShape, "invalid_shape"},
        {TensorStatus::InvalidStride, "invalid_stride"},
        {TensorStatus::DtypeMismatch, "dtype_mismatch"},
        {TensorStatus::UnsupportedDtype, "unsupported_dtype"},
        {TensorStatus::UnsupportedOperation, "unsupported_operation"},
        {TensorStatus::UnsupportedLayout, "unsupported_layout"},
        {TensorStatus::InvalidPlacement, "invalid_placement"},
        {TensorStatus::ResourceLimitExceeded, "resource_limit_exceeded"},
        {TensorStatus::MemoryAllocationFailure, "memory_allocation_failure"},
        {TensorStatus::InvalidState, "invalid_state"},
        {TensorStatus::DeviceCapabilityMismatch, "device_capability_mismatch"},
        {TensorStatus::TransferUnsupported, "transfer_unsupported"},
        {TensorStatus::ExecutionFailure, "execution_failure"},
        {TensorStatus::NumericalValidationFailure, "numerical_validation_failure"},
        {TensorStatus::NotInitialized, "not_initialized"},
        {TensorStatus::Internal, "internal"},
    };
    for (const auto& entry : kTable) {
        if (code == entry.code) {
            out = entry.status;
            return true;
        }
    }
    return false;
}

vortyx::compute::Status tensor_status_to_compute_status(TensorStatus status) {
    switch (status) {
        case TensorStatus::Ok: return vortyx::compute::Status::Ok;
        case TensorStatus::InvalidInput: return vortyx::compute::Status::InvalidInput;
        case TensorStatus::InvalidShape: return vortyx::compute::Status::InvalidInput;
        case TensorStatus::InvalidStride: return vortyx::compute::Status::InvalidInput;
        case TensorStatus::DtypeMismatch: return vortyx::compute::Status::InvalidInput;
        case TensorStatus::UnsupportedDtype: return vortyx::compute::Status::InvalidInput;
        case TensorStatus::UnsupportedOperation: return vortyx::compute::Status::InvalidInput;
        case TensorStatus::UnsupportedLayout: return vortyx::compute::Status::InvalidInput;
        case TensorStatus::InvalidPlacement: return vortyx::compute::Status::InvalidInput;
        case TensorStatus::ResourceLimitExceeded: return vortyx::compute::Status::InvalidInput;
        case TensorStatus::MemoryAllocationFailure: return vortyx::compute::Status::BackendError;
        case TensorStatus::InvalidState: return vortyx::compute::Status::InvalidInput;
        case TensorStatus::DeviceCapabilityMismatch:
            return vortyx::compute::Status::BackendUnavailable;
        case TensorStatus::TransferUnsupported:
            return vortyx::compute::Status::BackendUnavailable;
        case TensorStatus::ExecutionFailure: return vortyx::compute::Status::BackendError;
        case TensorStatus::NumericalValidationFailure:
            return vortyx::compute::Status::BackendError;
        case TensorStatus::NotInitialized: return vortyx::compute::Status::NotInitialized;
        case TensorStatus::Internal: return vortyx::compute::Status::BackendError;
    }
    return vortyx::compute::Status::BackendError;
}

TensorStatus tensor_status_from_compute_status(vortyx::compute::Status status) {
    switch (status) {
        case vortyx::compute::Status::Ok: return TensorStatus::Ok;
        case vortyx::compute::Status::InvalidInput: return TensorStatus::InvalidInput;
        case vortyx::compute::Status::NotInitialized: return TensorStatus::NotInitialized;
        case vortyx::compute::Status::BackendUnavailable:
            return TensorStatus::DeviceCapabilityMismatch;
        case vortyx::compute::Status::BackendError: return TensorStatus::ExecutionFailure;
    }
    return TensorStatus::ExecutionFailure;
}

}  // namespace vortyx::tensor
