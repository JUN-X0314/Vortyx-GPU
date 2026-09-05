// Tensor core tests (Phase 13) — dtype vocabulary, shape/stride/broadcast
// with checked arithmetic, placement, storage through the Phase 4 resource
// system, the tensor value type + views, and metadata serialization.
//
// Convention: plain main() + check(), like every other test in this
// project. No GPU required — the tensor core runs on the host.

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "tensor/tensor.hpp"

using namespace vortyx::tensor;
using ST = TensorStatus;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_status(ST actual, ST expected, const std::string& message) {
    check(actual == expected,
          message + " (expected " + tensor_status_code(expected) + ", got " +
              tensor_status_code(actual) + ")");
}

}  // namespace

int main() {
    // The manager MUST live in a shared_ptr (the Phase 4 contract: Buffer
    // handles observe it weakly).
    auto manager = std::make_shared<vortyx::resource::ResourceManager>();
    vortyx::resource::CpuBufferProvider cpu_provider;
    check(manager->register_provider(&cpu_provider), "cpu provider registers");

    // =====================================================================
    // 1. DataType vocabulary: stable names, byte widths, classes
    // =====================================================================
    {
        check(std::string(to_string(DataType::FP32)) == "fp32", "fp32 name");
        check(std::string(to_string(DataType::FP16)) == "fp16", "fp16 name");
        check(std::string(to_string(DataType::BF16)) == "bf16", "bf16 name");
        check(std::string(to_string(DataType::INT32)) == "int32", "int32 name");
        check(std::string(to_string(DataType::INT8)) == "int8", "int8 name");

        DataType parsed = DataType::FP32;
        check(data_type_from_string("bf16", parsed) && parsed == DataType::BF16,
              "bf16 parses");
        check(!data_type_from_string("FP32", parsed), "uppercase refused (canonical lowercase)");
        check(!data_type_from_string("float32", parsed), "non-canonical alias refused");
        check(!data_type_from_string("fp64", parsed), "fp64 does not exist (honest absence)");

        check(data_type_byte_width(DataType::FP32) == 4, "fp32 width");
        check(data_type_byte_width(DataType::FP16) == 2, "fp16 width");
        check(data_type_byte_width(DataType::BF16) == 2, "bf16 width");
        check(data_type_byte_width(DataType::INT32) == 4, "int32 width");
        check(data_type_byte_width(DataType::INT8) == 1, "int8 width");

        check(data_type_is_floating(DataType::FP16) && !data_type_is_integer(DataType::FP16),
              "fp16 is floating");
        check(data_type_is_integer(DataType::INT8) && !data_type_is_floating(DataType::INT8),
              "int8 is integer");
        check(all_data_types().size() == 5, "the dtype set has exactly five entries");
    }

    // =====================================================================
    // 2. TensorShape: rank-free N-D, negative/zero/overflow rules
    // =====================================================================
    {
        std::string error;
        const TensorShape s = TensorShape::make({2, 3, 4});
        check(s.rank() == 3, "rank from dims");
        check(s.validate(error) == ST::Ok, "valid shape accepted");

        check(TensorShape::make({}).rank() == 0, "rank-0 shape is representable");

        TensorShape negative = TensorShape::make({2, -3});
        check_status(negative.validate(error), ST::InvalidShape, "negative dim refused");

        TensorShape huge = TensorShape::make({2147483647LL, 2147483647LL, 2147483647LL});
        check_status(huge.validate(error), ST::ResourceLimitExceeded,
                     "element count overflow refused (never wrapped)");

        std::int64_t elements = 0;
        check(TensorShape::make({2, 3}).total_elements(elements) && elements == 6,
              "element count");
        check(TensorShape::make({2, 0, 3}).total_elements(elements) && elements == 0,
              "zero dim gives zero elements");

        // Rank cap: the explicit resource-exhaustion guard.
        std::vector<TensorDim> too_many(kMaxTensorRank + 1, 1);
        TensorShape over_rank(too_many);
        check_status(over_rank.validate(error), ST::ResourceLimitExceeded,
                     "rank cap enforced");

        // Equality semantics.
        check(TensorShape::make({2, 3}) == TensorShape::make({2, 3}), "shape equality");
        check(TensorShape::make({2, 3}) != TensorShape::make({3, 2}), "shape inequality");

        check(TensorShape::make({2, 3, 4}).describe() == "[2, 3, 4]", "deterministic describe");
    }

    // =====================================================================
    // 3. Strides, layouts and linear indexing (bounds + overflow)
    // =====================================================================
    {
        std::string error;
        const TensorShape shape = TensorShape::make({2, 3, 4});
        std::vector<std::int64_t> strides;
        check(contiguous_strides(shape, strides, error), "contiguous strides computed");
        check(strides == std::vector<std::int64_t>({12, 4, 1}), "canonical row-major strides");

        const TensorLayout contiguous = TensorLayout::contiguous(shape, error);
        check(contiguous.is_row_major_contiguous_for(shape), "layout reports contiguous");
        check_status(contiguous.validate(shape, 24, error), ST::Ok,
                     "contiguous layout within span");

        // Out-of-span strides refused.
        TensorLayout oversized;
        oversized.kind = LayoutKind::Strided;
        oversized.strides = {100, 4, 1};
        check_status(oversized.validate(shape, 24, error), ST::InvalidStride,
                     "strides beyond the storage span refused");

        // Negative strides refused (documented Phase 13 scope).
        TensorLayout negative;
        negative.kind = LayoutKind::Strided;
        negative.strides = {-12, 4, 1};
        check_status(negative.validate(shape, 24, error), ST::InvalidStride,
                     "negative strides refused");

        // Stride/rank mismatch refused.
        TensorLayout wrong_rank;
        wrong_rank.kind = LayoutKind::Strided;
        wrong_rank.strides = {1};
        check_status(wrong_rank.validate(shape, 24, error), ST::InvalidStride,
                     "stride count must match rank");

        // Linear offsets with bounds checking.
        std::int64_t offset = 0;
        check(linear_offset(shape, contiguous, {1, 2, 3}, offset, error) &&
                  offset == 12 + 8 + 3,
              "linear offset of (1,2,3)");
        check(!linear_offset(shape, contiguous, {2, 0, 0}, offset, error),
              "out-of-bounds index refused");
        check(!linear_offset(shape, contiguous, {0, 0, -1}, offset, error),
              "negative index refused");
        check(!linear_offset(shape, contiguous, {1, 2}, offset, error),
              "wrong index count refused");
    }

    // =====================================================================
    // 4. Broadcasting (the exact implemented semantics)
    // =====================================================================
    {
        std::string error;
        TensorShape out;

        check_status(broadcast_shapes(TensorShape::make({2, 3}), TensorShape::make({2, 3}), out,
                                      error),
                     ST::Ok, "same-shape broadcast");
        check(out == TensorShape::make({2, 3}), "same-shape result");

        check_status(broadcast_shapes(TensorShape::make({3}), TensorShape::make({2, 3}), out,
                                      error),
                     ST::Ok, "rank expansion");
        check(out == TensorShape::make({2, 3}), "rank expansion result");

        check_status(broadcast_shapes(TensorShape::make({3, 1}), TensorShape::make({1, 4}), out,
                                      error),
                     ST::Ok, "singleton stretching");
        check(out == TensorShape::make({3, 4}), "singleton stretch result");

        check_status(broadcast_shapes(TensorShape::make({1, 3}), TensorShape::make({4, 3}), out,
                                      error),
                     ST::Ok, "leading dim 1 vs N stretches");
        check(out == TensorShape::make({4, 3}), "leading stretch result");

        check_status(broadcast_shapes(TensorShape::make({2, 3}), TensorShape::make({2, 4}), out,
                                      error),
                     ST::InvalidShape, "incompatible trailing dims refused");
        check_status(broadcast_shapes(TensorShape::make({3}), TensorShape::make({4}), out, error),
                     ST::InvalidShape, "mismatched 1-D refused");

        check(!is_broadcast_compatible(TensorShape::make({2, 3}), TensorShape::make({3})),
              "target rank below source rank is incompatible");
    }

    // =====================================================================
    // 5. Placement: identity reuse + validation rules
    // =====================================================================
    {
        std::string error;
        TensorPlacement host = TensorPlacement::host();
        check_status(host.validate(error), ST::Ok, "host placement valid");

        TensorPlacement device = TensorPlacement::on_device("device-1", "cpu");
        check_status(device.validate(error), ST::Ok, "device placement valid");
        check(device.describe() == "device:device-1/cpu", "deterministic device describe");

        TensorPlacement invalid_id = TensorPlacement::on_device("bad id!", "cpu");
        check_status(invalid_id.validate(error), ST::InvalidPlacement,
                     "invalid device id refused");

        TensorPlacement unknown_backend = TensorPlacement::on_device("device-1", "cuda");
        check_status(unknown_backend.validate(error), ST::InvalidPlacement,
                     "non-canonical backend refused (no fake backends)");

        TensorPlacement host_with_id = TensorPlacement::host();
        host_with_id.device_id = "oops";
        check_status(host_with_id.validate(error), ST::InvalidPlacement,
                     "host placement carrying a device id refused");

        check(TensorPlacement::on_device("d", "cpu").same_place_as(
                  TensorPlacement::on_device("d", "cpu")),
              "same-place detection");
        check(!TensorPlacement::on_device("d", "cpu").same_place_as(
                  TensorPlacement::on_device("other", "cpu")),
              "different devices are different places");
        check(TensorPlacement::host().same_place_as(TensorPlacement::host()),
              "all host placements are the same place");
    }

    // =====================================================================
    // 6. TensorStorage: allocation through the Phase 4 resource system ONLY
    // =====================================================================
    {
        std::string error;
        TensorStorage storage;
        TensorStatus status = ST::Ok;
        TensorStorage::create(*manager, 6, DataType::FP32, vortyx::resource::ResourceAccess::Read,
                              "", storage, status, error);
        check(status == ST::Ok, "storage allocated");
        check(storage.valid(), "storage valid");
        check(storage.elements() == 6 && storage.byte_size() == 24, "storage sizing");
        check(std::string(storage.backend_name()) == "cpu", "honest backend name");
        check(storage.memory_location() == vortyx::resource::MemoryLocation::Host,
              "honest host location");

        // The Phase 4 manager accounted for it.
        check(manager->stats().live_buffers == 1 && manager->stats().live_bytes == 24,
              "resource accounting sees tensor storage");

        // Exact-size whole-tensor transfer contract.
        const float values[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
        check(storage.write_bytes(values, sizeof(values), error) == ST::Ok, "whole write ok");
        float read_back[6] = {0};
        check(storage.read_bytes(read_back, sizeof(read_back), error) == ST::Ok, "whole read ok");
        check(std::memcmp(values, read_back, sizeof(values)) == 0, "round-trip bit-exact");

        check_status(storage.write_bytes(values, 8, error), ST::InvalidInput,
                     "partial write refused");
        check_status(storage.write_bytes(nullptr, sizeof(values), error), ST::InvalidInput,
                     "null write refused");

        // Zero-element refusal (the project-wide rule).
        TensorStorage zero;
        TensorStorage::create(*manager, 0, DataType::INT32,
                              vortyx::resource::ResourceAccess::Read, "", zero, status, error);
        check_status(status, ST::InvalidShape, "zero-element storage refused");

        // Overflow beyond the per-tensor cap refused before allocation.
        TensorStorage too_big;
        TensorStorage::create(*manager, kMaxTensorBytes / 2, DataType::FP32,
                              vortyx::resource::ResourceAccess::Read, "", too_big, status,
                              error);
        check_status(status, ST::ResourceLimitExceeded, "byte cap enforced");

        storage.reset();
        check(manager->stats().live_buffers == 0, "release returns the resource");
    }

    // =====================================================================
    // 7. Tensor value type: creation, from_host, views
    // =====================================================================
    {
        std::string error;
        Tensor tensor;
        check_status(Tensor::create(*manager, TensorShape::make({2, 3}), DataType::FP32,
                                    TensorPlacement::host(), tensor, error),
                     ST::Ok, "tensor created");
        check(tensor.valid() && tensor.is_contiguous(), "fresh tensor is contiguous");
        check(tensor.elements() == 6 && tensor.byte_size() == 24, "tensor sizing");

        // from_host round trip.
        const float data[6] = {1, 2, 3, 4, 5, 6};
        Tensor filled;
        check_status(Tensor::from_host(*manager, TensorShape::make({2, 3}), DataType::FP32, data,
                                       sizeof(data), filled, error),
                     ST::Ok, "from_host");
        float out[6] = {0};
        check(filled.read_host(out, sizeof(out), error) == ST::Ok, "read back");
        check(std::memcmp(data, out, sizeof(data)) == 0, "from_host round-trip bit-exact");

        // Zero-element tensor refused.
        Tensor zero;
        check_status(Tensor::create(*manager, TensorShape::make({0, 3}), DataType::FP32,
                                    TensorPlacement::host(), zero, error),
                     ST::InvalidShape, "zero-element tensor refused");

        // Reshape (element-count preserving, contiguous-only).
        Tensor reshaped;
        check_status(filled.reshape(TensorShape::make({3, 2}), reshaped, error), ST::Ok,
                     "reshape 2x3 -> 3x2");
        check(reshaped.shape() == TensorShape::make({3, 2}), "reshape shape");
        check(reshaped.storage() == filled.storage(), "reshape shares storage");
        Tensor bad_reshape;
        check_status(filled.reshape(TensorShape::make({4, 2}), bad_reshape, error),
                     ST::InvalidShape, "element-count-changing reshape refused");

        // Transpose view (rank 2, strided, shares storage).
        Tensor transposed;
        check_status(filled.transpose(transposed, error), ST::Ok, "transpose view");
        check(transposed.shape() == TensorShape::make({3, 2}), "transpose shape");
        check(transposed.layout().kind == LayoutKind::Strided, "transpose layout is strided");
        check(transposed.storage() == filled.storage(), "transpose shares storage");
        // read_host is a STORAGE-level transfer (the same bytes the base sees;
        // this is the documented view semantics — logical element access
        // applies strides, which the kernels and the Transpose OP do).
        float t_raw[6] = {0};
        check(transposed.read_host(t_raw, sizeof(t_raw), error) == ST::Ok,
              "a view reads its underlying storage");
        check(std::memcmp(t_raw, data, sizeof(data)) == 0,
              "the view and the base share identical storage bytes");

        // Transpose of a rank-3 tensor refused.
        Tensor rank3;
        check_status(Tensor::create(*manager, TensorShape::make({2, 2, 2}), DataType::FP32,
                                    TensorPlacement::host(), rank3, error),
                     ST::Ok, "rank-3 tensor");
        Tensor t3;
        check_status(rank3.transpose(t3, error), ST::InvalidShape, "rank-3 transpose refused");

        // Reshape of a strided (transposed) view refused — no hidden copy.
        Tensor reshape_of_view;
        check_status(transposed.reshape(TensorShape::make({6}), reshape_of_view, error),
                     ST::UnsupportedLayout, "strided reshape refused (UnsupportedLayout)");

        // Broadcast view.
        Tensor broadcast;
        check_status(filled.broadcast_to(TensorShape::make({2, 2, 3}), broadcast, error), ST::Ok,
                     "broadcast view");
        check(broadcast.shape() == TensorShape::make({2, 2, 3}), "broadcast shape");
        check(broadcast.storage() == filled.storage(), "broadcast shares storage");
        Tensor invalid_broadcast;
        check_status(filled.broadcast_to(TensorShape::make({4, 3}), invalid_broadcast, error),
                     ST::InvalidShape, "incompatible broadcast refused");
    }

    // =====================================================================
    // 8. Metadata serialization: deterministic, payload-free, validated
    // =====================================================================
    {
        std::string error;
        const float data[6] = {1, 2, 3, 4, 5, 6};
        Tensor filled;
        check(Tensor::from_host(*manager, TensorShape::make({2, 3}), DataType::FP32, data,
                                sizeof(data), filled, error) == ST::Ok,
              "tensor for serialization");

        const std::string first = serialize_tensor_meta(filled);
        const std::string second = serialize_tensor_meta(filled);
        check(first == second, "serialization is deterministic");
        check(first.find("data") == std::string::npos, "no payload key exists");
        check(first.find("[2, 3]") == std::string::npos, "no accidental payload formatting");

        TensorMeta parsed;
        check_status(parse_tensor_meta(first, parsed, error), ST::Ok, "meta parses");
        check(parsed.shape == TensorShape::make({2, 3}) && parsed.dtype == DataType::FP32,
              "meta round trip");

        // Device placement serializes the device id (identity reuse) but no data.
        Tensor device_tensor;
        check(Tensor::create(*manager, TensorShape::make({4}), DataType::INT32,
                             TensorPlacement::on_device("dev-42", "cpu"), device_tensor, error) ==
                  ST::Ok,
              "device tensor");
        const std::string device_meta = serialize_tensor_meta(device_tensor);
        check(device_meta.find("dev-42") != std::string::npos, "device id in meta");
        TensorMeta device_parsed;
        check(parse_tensor_meta(device_meta, device_parsed, error) == ST::Ok &&
                  device_parsed.placement.device_id == "dev-42",
              "device placement round trip");

        // Hostile payloads refused.
        TensorMeta refused;
        check(parse_tensor_meta("{\"shape\":[2],\"dtype\":\"fp32\",\"layout\":{\"kind\":"
                                "\"row_major_contiguous\",\"strides\":[1]},\"placement\":{"
                                "\"location\":\"host\"},\"payload\":\"AAAA\"}",
                                refused, error) == ST::InvalidInput,
              "unknown field refused");
        check(parse_tensor_meta("{\"shape\":[2],\"dtype\":\"fp64\",\"layout\":{\"kind\":"
                                "\"row_major_contiguous\",\"strides\":[1]},\"placement\":{"
                                "\"location\":\"host\"}}",
                                refused, error) == ST::UnsupportedDtype,
              "fp64 refused (does not exist)");
        check(parse_tensor_meta("not json", refused, error) == ST::InvalidInput,
              "malformed JSON refused");

        // Phase 13 audit regressions: hostile arithmetic at the metadata
        // contract boundary must be REFUSED without undefined behavior (both
        // cases were UBSan-verified failures before the fixes).
        //
        // (a) A stride whose reachable offset overflows int64: the layout
        //     validation computes (dims[d]-1) * stride[d] — the product must
        //     be checked BEFORE it wraps (signed overflow was UB here).
        check(parse_tensor_meta("{\"shape\":[3],\"dtype\":\"fp32\",\"layout\":{\"kind\":"
                                "\"strided\",\"strides\":[4611686018427387904]},"
                                "\"placement\":{\"location\":\"host\"}}",
                                refused, error) == ST::InvalidStride,
              "stride-reach overflow refused (2^62 stride on [3])");
        //     Direct validate() with the same hostile strides: refused, no UB.
        TensorLayout hostile;
        hostile.kind = LayoutKind::Strided;
        hostile.strides = {std::int64_t{1} << 62};
        check(hostile.validate(TensorShape::make({3}), std::int64_t{1} << 62, error) ==
                  ST::InvalidStride,
              "layout validate refuses the overflowing product directly");
        // (b) An out-of-int64-range JSON number: the double->int64 cast is
        //     undefined behavior before the integrality check could reject.
        check(parse_tensor_meta("{\"shape\":[1e300],\"dtype\":\"fp32\",\"layout\":{\"kind\":"
                                "\"row_major_contiguous\",\"strides\":[1]},\"placement\":{"
                                "\"location\":\"host\"}}",
                                refused, error) == ST::InvalidShape,
              "out-of-int64-range dimension refused");
        // (c) The valid boundary stays valid: the largest representable
        //     stride that fits the storage is accepted.
        TensorLayout edge;
        edge.kind = LayoutKind::Strided;
        edge.strides = {1};
        check(edge.validate(TensorShape::make({3}), 3, error) == ST::Ok,
              "in-range strides still validate");
    }

    // =====================================================================
    // 9. Resource-system accounting: tensors are Phase 4 resources
    // =====================================================================
    {
        std::string error;
        const std::size_t before = manager->stats().total_allocations;
        {
            Tensor a;
            Tensor b;
            check(Tensor::create(*manager, TensorShape::make({8}), DataType::INT32,
                                 TensorPlacement::host(), a, error) == ST::Ok, "tensor a");
            check(Tensor::create(*manager, TensorShape::make({8}), DataType::INT32,
                                 TensorPlacement::host(), b, error) == ST::Ok, "tensor b");
            check(manager->stats().live_buffers == 2, "two live resources");
        }
        // RAII: the tensors released their storages.
        check(manager->stats().live_buffers == 0, "no leak after scope");
        check(manager->stats().total_allocations == before + 2, "allocation count advanced by 2");
    }

    if (failures == 0) {
        std::cout << "Tensor core tests passed.\n";
        return 0;
    }
    std::cerr << failures << " tensor core test(s) failed.\n";
    return 1;
}
