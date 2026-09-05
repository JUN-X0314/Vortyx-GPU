#pragma once

// Tensor shape, layout and broadcasting (Phase 13).
//
// TensorShape is an N-D shape with DYNAMIC RANK: dimensions live in a
// vector, there is no hardcoded rank limit in the type itself. The project
// limit that DOES exist is an explicit resource-exhaustion guard
// (kMaxTensorRank below) enforced at tensor creation and op validation —
// an input that absurd cannot be refused by allocation failure alone.
//
// SEMANTICS CONTRACT (pinned by tests):
//   - Dimensions are non-negative. A negative dimension is refused
//     (InvalidShape) — never reinterpreted as a sentinel.
//   - ZERO DIMENSIONS: a shape with any zero dimension has an element
//     count of 0. Consistent with the whole project's zero-element rule
//     (Phase 3 ComputeTask, Phase 4 BufferDesc: "a zero-element task has
//     nothing to compute; it is rejected explicitly instead of being
//     treated as a silent success"), TENSORS with an element count of 0
//     are REJECTED at creation. The shape type itself can represent such
//     a shape (op shape inference may pass through it before rejection);
//     the refusal happens where storage or execution would begin.
//   - total_elements() is checked arithmetic: an int64 overflow is an
//     explicit failure (out parameter + false), never a wrapped value.
//
// LAYOUT (TensorLayout):
//   - RowMajorContiguous: strides are the canonical row-major strides
//     (computed by contiguous_strides). The common case; reshape requires it.
//   - Strided: explicit strides, element i_d of dimension d contributes
//     i_d * stride_d to the linear element offset. Used for transpose
//     views and broadcast views (stride 0 = broadcast along that axis).
//     Strides are validated: the maximum reachable offset must stay within
//     the storage element span and must not overflow (checked arithmetic).
//   - The layout is DATA, never decoration: kernels read the strides.
//
// BROADCASTING (the exact implemented semantics, deliberately documented as
// what it is — NumPy-style right-aligned broadcasting, verified by tests;
// no claim beyond what is tested):
//   - Shapes are compared from the trailing dimension backwards.
//   - Dimensions are compatible when equal, or one of them is 1.
//   - Missing leading dimensions of the shorter shape behave as 1.
//   - The result dimension is the maximum of the two (a 1 stretches).
//   - Incompatible dimensions are an explicit error, never a guess.

#include <cstdint>
#include <string>
#include <vector>

#include "tensor/status.hpp"

namespace vortyx::tensor {

// Resource-exhaustion guards (explicit project limits — the values are
// policy, the existence of the limits is the contract).
inline constexpr std::size_t kMaxTensorRank = 8;
// Per-tensor byte budget. Matches the Phase 4 per-buffer safety cap: tensor
// storage lives in the resource system, so a tensor cannot ask for more
// than one buffer may hold.
inline constexpr std::int64_t kMaxTensorBytes = std::int64_t{1} << 30;  // 1 GiB

// One dimension value. Non-negative by contract (validated on entry).
using TensorDim = std::int64_t;

// ---------------------------------------------------------------------------
// TensorShape
// ---------------------------------------------------------------------------

struct TensorShape {
    std::vector<TensorDim> dims;

    TensorShape() = default;
    explicit TensorShape(std::vector<TensorDim> d) : dims(std::move(d)) {}

    // Convenience literal-style constructors for the common ranks.
    static TensorShape make(std::initializer_list<TensorDim> d) {
        return TensorShape(std::vector<TensorDim>(d.begin(), d.end()));
    }

    std::size_t rank() const { return dims.size(); }
    bool empty() const { return dims.empty(); }

    // Element count with checked arithmetic. Returns false on int64 overflow
    // ('out' untouched). A shape with a zero dimension yields 0.
    bool total_elements(std::int64_t& out) const;

    // Validation used at tensor creation and op boundaries. Returns Ok, or
    // the failing TensorStatus with 'error' filled:
    //   - rank > kMaxTensorRank                    -> ResourceLimitExceeded
    //   - any negative dimension                   -> InvalidShape
    //   - any dimension < 0 (same rule, stated)    -> InvalidShape
    //   - element count overflows int64            -> ResourceLimitExceeded
    TensorStatus validate(std::string& error) const;

    // Value equality (determinism checks compare shapes directly).
    friend bool operator==(const TensorShape& a, const TensorShape& b) {
        return a.dims == b.dims;
    }
    friend bool operator!=(const TensorShape& a, const TensorShape& b) { return !(a == b); }

    // Human-readable deterministic form: "[2, 3, 4]".
    std::string describe() const;
};

// Canonical row-major contiguous strides for 'shape' ("s[rank-1] = 1,
// s[d] = s[d+1] * dims[d+1]"). Checked: overflow returns false.
bool contiguous_strides(const TensorShape& shape, std::vector<std::int64_t>& out,
                        std::string& error);

// ---------------------------------------------------------------------------
// TensorLayout
// ---------------------------------------------------------------------------

enum class LayoutKind : std::uint8_t {
    RowMajorContiguous = 0,  // canonical row-major strides
    Strided = 1,             // explicit strides (transpose/broadcast views)
};

const char* to_string(LayoutKind kind);

struct TensorLayout {
    LayoutKind kind = LayoutKind::RowMajorContiguous;
    std::vector<std::int64_t> strides;  // elements (not bytes) per dimension step

    // Canonical layout for a shape (row-major contiguous).
    static TensorLayout contiguous(const TensorShape& shape, std::string& error);

    // True when the strides are exactly the canonical row-major strides for
    // the shape (regardless of the kind tag — the strides are the truth).
    bool is_row_major_contiguous_for(const TensorShape& shape) const;

    // Validation (pure). Returns Ok or the failing status with 'error':
    //   - stride count != shape rank            -> InvalidStride
    //   - any negative stride                   -> InvalidStride (Phase 13
    //       supports non-negative strides only; negative strides are an
    //       explicit refusal, not a silent reinterpretation)
    //   - max reachable offset overflows        -> InvalidStride
    //   - max reachable offset >= storage span  -> InvalidStride
    //     ('storage_elements' = the element count of the storage the layout
    //     indexes; for broadcast strides (0) the reachable span can be
    //     smaller than the logical element count — see broadcast_strides)
    TensorStatus validate(const TensorShape& shape, std::int64_t storage_elements,
                          std::string& error) const;

    friend bool operator==(const TensorLayout& a, const TensorLayout& b) {
        return a.kind == b.kind && a.strides == b.strides;
    }
    friend bool operator!=(const TensorLayout& a, const TensorLayout& b) { return !(a == b); }
};

// Linear element offset of a multi-index ('indices' must have shape.rank()
// entries, each within [0, dims[d])). Checked: out-of-bounds indices,
// negative indices and offset overflow are explicit failures.
bool linear_offset(const TensorShape& shape, const TensorLayout& layout,
                   const std::vector<std::int64_t>& indices, std::int64_t& out,
                   std::string& error);

// ---------------------------------------------------------------------------
// Broadcasting (exact implemented semantics — see the module header)
// ---------------------------------------------------------------------------

// NumPy-style right-aligned broadcast of two shapes. Returns Ok with the
// result shape, or InvalidShape with 'error' naming the first incompatible
// dimension. Broadcast against a 0-element shape yields a 0-element result
// (the tensor-level zero-element refusal still applies before execution).
TensorStatus broadcast_shapes(const TensorShape& a, const TensorShape& b,
                              TensorShape& out, std::string& error);

// Broadcast strides: the strides 'layout' would have when its tensor is
// logically viewed at 'target' shape (1-dimensions get stride 0, leading
// dimensions get stride 0). 'target' must be a valid broadcast of 'source'
// (broadcast_shapes(source, target) == target). Pure; checked.
TensorStatus broadcast_strides(const TensorShape& source, const TensorLayout& layout,
                               const TensorShape& target, TensorLayout& out,
                               std::string& error);

// True when 'candidate' broadcasts 'source' exactly (one-way compatibility:
// every source dimension is compatible with the target's aligned dimension).
bool is_broadcast_compatible(const TensorShape& source, const TensorShape& target);

}  // namespace vortyx::tensor
