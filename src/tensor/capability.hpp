#pragma once

// Tensor capabilities and requirements (Phase 13).
//
// The capability model answers ONE question honestly: what tensor work can
// this execution target actually do? A capability entry exists ONLY for
// something a backend really implements — the project's honesty rules
// forbid declaring "Tensor Core support", "BF16 support" or any hardware
// acceleration that no code path in this repository executes.
//
//   - The CPU REFERENCE backend's capabilities are exactly the reference
//     kernels' implemented set (see backend.hpp).
//   - The Runtime ADAPTER backend's capabilities are exactly the int32
//     elementwise ops the existing Phase 10 engine executes (through the
//     real CPU/Vulkan backends).
//   - matrix_acceleration is a SELF-REPORTED claim enum, NotClaimed by
//     default. NOTHING in Phase 13 sets it to Claimed (no hardware tensor
//     path exists) — the field's default is the honest value.
//   - max_rank / max_elements / max_bytes are real policy limits enforced
//     by the same constants the tensor layer enforces (shape.hpp).
//
// TensorRequirements is the request-side mirror: which ops, which dtypes,
// which rank/size bounds a workload needs. satisfies() is the pure
// compatibility decision used by dispatch and by the Phase 12 placement
// integration (a device whose capabilities do not satisfy the requirements
// is never selected — capability mismatch is detected, never guessed away).

#include <cstdint>
#include <string>
#include <vector>

#include "tensor/dtype.hpp"
#include "tensor/op.hpp"
#include "tensor/status.hpp"

namespace vortyx::tensor {

// Self-reported acceleration claim. NotClaimed is the honest default; a
// backend that cannot verify hardware acceleration NEVER sets Claimed.
enum class MatrixAcceleration : std::uint8_t {
    NotClaimed = 0,
    Claimed = 1,  // reserved: no Phase 13 backend reports this (documented)
};

const char* to_string(MatrixAcceleration acceleration);

struct TensorCapabilities {
    // The ops this target can execute (validated against the op vocabulary).
    std::vector<TensorOp> supported_ops;

    // The dtypes this target can compute in.
    std::vector<DataType> supported_dtypes;

    // Layout support: Phase 13 kernels require row-major contiguous inputs
    // and produce contiguous outputs; strided inputs are materialized by
    // the EXECUTOR (an explicit, documented copy — never hidden inside a
    // kernel). This flag is therefore true for every real Phase 13 target
    // and exists so the capability model can express the constraint.
    bool supports_strided_input = false;

    // True when the backend can execute BROADCAST elementwise cases (input
    // shapes that differ from the output shape). The runtime adapter
    // executes same-shape int32 elementwise only (the existing ComputeTask
    // contract requires equal sizes), so it reports false — a broadcast
    // request dispatches to the reference backend instead. Capability-based
    // dispatch, not a silent fallback.
    bool supports_broadcast = true;

    // Real policy limits (mirroring the tensor layer's own guards).
    std::size_t max_rank = 0;
    std::int64_t max_elements = 0;
    std::int64_t max_bytes = 0;

    // Preferred tile for matrix kernels (configuration, not a measurement;
    // the reference kernel uses it only as an iteration structure hint —
    // results are accumulation-order-deterministic regardless).
    std::int64_t preferred_tile_m = 0;
    std::int64_t preferred_tile_n = 0;
    std::int64_t preferred_tile_k = 0;

    // Self-reported matrix acceleration (see the enum comment).
    MatrixAcceleration matrix_acceleration = MatrixAcceleration::NotClaimed;

    bool supports_op(TensorOp op) const;
    bool supports_dtype(DataType dtype) const;

    // Validation: supported_ops holds known ops (no duplicates), dtypes are
    // known (no duplicates), limits are positive. Ok or InvalidInput.
    TensorStatus validate(std::string& error) const;
};

struct TensorRequirements {
    std::vector<TensorOp> required_ops;
    std::vector<DataType> required_dtypes;
    std::size_t max_input_rank = 0;
    std::int64_t max_tensor_bytes = 0;

    // Pure compatibility decision: every required op and dtype supported,
    // and the target's limits cover the requirement's bounds. An EMPTY
    // requirement list is satisfied by any valid capability set (an empty
    // requirement asks for nothing — but an INVALID capability set is
    // rejected before this is ever consulted).
    bool satisfied_by(const TensorCapabilities& capabilities) const;
};

}  // namespace vortyx::tensor
