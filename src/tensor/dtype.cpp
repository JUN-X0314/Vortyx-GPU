// Tensor data types (Phase 13) — implementation.

#include "tensor/dtype.hpp"

namespace vortyx::tensor {

const char* to_string(DataType dtype) {
    switch (dtype) {
        case DataType::FP32: return "fp32";
        case DataType::FP16: return "fp16";
        case DataType::BF16: return "bf16";
        case DataType::INT32: return "int32";
        case DataType::INT8: return "int8";
    }
    return "unknown";
}

bool data_type_from_string(const std::string& name, DataType& out) {
    if (name == "fp32") { out = DataType::FP32; return true; }
    if (name == "fp16") { out = DataType::FP16; return true; }
    if (name == "bf16") { out = DataType::BF16; return true; }
    if (name == "int32") { out = DataType::INT32; return true; }
    if (name == "int8") { out = DataType::INT8; return true; }
    return false;
}

std::size_t data_type_byte_width(DataType dtype) {
    switch (dtype) {
        case DataType::FP32: return 4;
        case DataType::FP16: return 2;
        case DataType::BF16: return 2;
        case DataType::INT32: return 4;
        case DataType::INT8: return 1;
    }
    return 0;
}

bool data_type_is_floating(DataType dtype) {
    return dtype == DataType::FP32 || dtype == DataType::FP16 || dtype == DataType::BF16;
}

bool data_type_is_integer(DataType dtype) {
    return dtype == DataType::INT32 || dtype == DataType::INT8;
}

std::vector<DataType> all_data_types() {
    return {DataType::FP32, DataType::FP16, DataType::BF16, DataType::INT32, DataType::INT8};
}

}  // namespace vortyx::tensor
