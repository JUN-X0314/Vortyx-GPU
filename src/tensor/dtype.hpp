#pragma once

// Tensor data types (Phase 13).
//
// The dtype vocabulary of the Vortyx tensor layer. Explicit, closed, and
// named — dtypes are NEVER carried as arbitrary strings (a typo must not
// become a fake capability; the same rule the platform layer applies to
// backend/operation names).
//
// Supported set (Phase 13):
//   FP32  — 4 bytes, IEEE-754 single precision
//   FP16  — 2 bytes, IEEE-754 binary16 storage; the CPU reference kernels
//           define FP16 execution as PROMOTE-COMPUTE-ROUND: inputs are
//           converted to FP32 (exact), math runs in FP32, the result is
//           rounded back to binary16 with round-to-nearest-even. This is a
//           real, fully defined deterministic semantics — it is NOT a claim
//           of native FP16 hardware compute (none is claimed anywhere).
//   BF16  — 2 bytes, brain floating point storage; same documented
//           promote-compute-round semantics as FP16 (round-to-nearest-even
//           on the low 16 mantissa bits).
//   INT32 — 4 bytes, signed 32-bit two's complement
//   INT8  — 1 byte, signed 8-bit two's complement
//
// FP64 is deliberately ABSENT: nothing in Phase 13 computes in FP64, and a
// dtype that has no implementation must not exist in the vocabulary (the
// honest-capability rule). Adding FP64 later is an additive enum extension.
//
// Every dtype reports its byte width and its numeric class. Quantization
// metadata (scales / zero points) has NO representation here: Phase 13
// implements no INT8 quantized kernels, so quantization is an explicit
// future extension — its absence cannot lie.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vortyx::tensor {

enum class DataType : std::uint8_t {
    FP32 = 0,
    FP16 = 1,
    BF16 = 2,
    INT32 = 3,
    INT8 = 4,
};

// Stable lowercase names ("fp32", "fp16", "bf16", "int32", "int8").
const char* to_string(DataType dtype);

// Parses a stable name. False for anything else (including case variants —
// the names are canonical lowercase, like every project vocabulary).
bool data_type_from_string(const std::string& name, DataType& out);

// Byte width of one element. Total dispatch: all five values.
std::size_t data_type_byte_width(DataType dtype);

// Numeric class predicates (pure, total).
bool data_type_is_floating(DataType dtype);  // FP32 / FP16 / BF16
bool data_type_is_integer(DataType dtype);   // INT32 / INT8

// The full dtype set in enum order (observability / capability building).
std::vector<DataType> all_data_types();

}  // namespace vortyx::tensor
