// Tensor capabilities and requirements (Phase 13) — implementation.

#include "tensor/capability.hpp"

#include <algorithm>

namespace vortyx::tensor {

const char* to_string(MatrixAcceleration acceleration) {
    switch (acceleration) {
        case MatrixAcceleration::NotClaimed: return "not_claimed";
        case MatrixAcceleration::Claimed: return "claimed";
    }
    return "unknown";
}

bool TensorCapabilities::supports_op(TensorOp op) const {
    return std::find(supported_ops.begin(), supported_ops.end(), op) != supported_ops.end();
}

bool TensorCapabilities::supports_dtype(DataType dtype) const {
    return std::find(supported_dtypes.begin(), supported_dtypes.end(), dtype) !=
           supported_dtypes.end();
}

TensorStatus TensorCapabilities::validate(std::string& error) const {
    for (std::size_t i = 0; i < supported_ops.size(); ++i) {
        for (std::size_t j = i + 1; j < supported_ops.size(); ++j) {
            if (supported_ops[i] == supported_ops[j]) {
                error = std::string("duplicate op in capabilities: ") + to_string(supported_ops[i]);
                return TensorStatus::InvalidInput;
            }
        }
    }
    for (std::size_t i = 0; i < supported_dtypes.size(); ++i) {
        for (std::size_t j = i + 1; j < supported_dtypes.size(); ++j) {
            if (supported_dtypes[i] == supported_dtypes[j]) {
                error = std::string("duplicate dtype in capabilities: ") +
                        to_string(supported_dtypes[i]);
                return TensorStatus::InvalidInput;
            }
        }
    }
    if (max_rank == 0 || max_elements <= 0 || max_bytes <= 0) {
        error = "capability limits must be positive (max_rank/max_elements/max_bytes)";
        return TensorStatus::InvalidInput;
    }
    if (max_elements > kMaxTensorBytes) {
        // The narrowest dtype (INT8, 1 byte) bounds element counts by the
        // byte budget; anything above it could never be backed by storage.
        // (Per-dtype enforcement happens at creation time — this is the
        // coarse sanity bound.)
        error = "capability max_elements exceeds the per-tensor byte budget";
        return TensorStatus::InvalidInput;
    }
    return TensorStatus::Ok;
}

bool TensorRequirements::satisfied_by(const TensorCapabilities& capabilities) const {
    for (const TensorOp op : required_ops) {
        if (!capabilities.supports_op(op)) return false;
    }
    for (const DataType dtype : required_dtypes) {
        if (!capabilities.supports_dtype(dtype)) return false;
    }
    if (max_input_rank > capabilities.max_rank) return false;
    if (max_tensor_bytes > capabilities.max_bytes) return false;
    return true;
}

}  // namespace vortyx::tensor
