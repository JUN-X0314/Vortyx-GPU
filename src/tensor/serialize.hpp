#pragma once

// Tensor metadata serialization (Phase 13) — provider-neutral METADATA only.
//
// WHAT TRAVELS: shape, dtype, layout (kind + strides) and placement — the
// descriptive contract of a tensor, serialized with the Phase 11 strict
// JSON module (compact, deterministic field order, no duplicate-key tricks,
// unknown fields refused on parse). The same conventions the platform and
// distributed contracts use.
//
// WHAT NEVER TRAVELS: tensor PAYLOAD. No base64 dumps, no element arrays, no
// "just this once" data smuggling — a JSON tensor payload would violate the
// project's contract rules (the payload path is the resource system, not a
// text wire). The serializer's input is a Tensor; the output carries zero
// bytes of its data.
//
// Schema (deterministic field order):
//   {
//     "shape": [int64...],
//     "dtype": "fp32" | "fp16" | "bf16" | "int32" | "int8",
//     "layout": { "kind": "row_major_contiguous" | "strided",
//                 "strides": [int64...] },
//     "placement": { "location": "host" | "device",
//                    "device_id": string?,      (device only)
//                    "backend": string? }       (when set)
//   }
//
// Parsed metas are VALIDATED (rank cap, stride sanity, canonical names) — a
// hostile payload is rejected with the precise TensorStatus, never trusted.

#include <string>

#include "platform/json.hpp"  // the Phase 11 strict JSON module (adapter boundary)
#include "tensor/status.hpp"
#include "tensor/tensor_value.hpp"

namespace vortyx::tensor {

// A parsed tensor description (metadata only — no storage attached).
struct TensorMeta {
    TensorShape shape;
    DataType dtype = DataType::FP32;
    TensorLayout layout;
    TensorPlacement placement;
};

// Serializes 'tensor's metadata (never its data). Deterministic bytes.
std::string serialize_tensor_meta(const Tensor& tensor);

// Serializes a standalone meta (same schema; used for graph input contracts).
std::string serialize_tensor_meta(const TensorMeta& meta);

// Parses + validates. Returns Ok with 'out' filled, or the precise failure
// (InvalidInput for malformed JSON, InvalidShape/InvalidStride/
// InvalidPlacement/UnsupportedDtype for schema violations, and unknown-field
// rejection like every project contract).
TensorStatus parse_tensor_meta(const std::string& text, TensorMeta& out, std::string& error);

}  // namespace vortyx::tensor
