// Tensor shape, layout and broadcasting (Phase 13) — implementation.

#include "tensor/shape.hpp"

#include <algorithm>
#include <sstream>

namespace vortyx::tensor {

namespace {

// Checked multiply-add helper: out = acc * m + a; false on int64 overflow.
bool checked_mul_add(std::int64_t acc, std::int64_t m, std::int64_t a, std::int64_t& out) {
    if (m != 0 && (acc > (INT64_MAX) / m || acc < (INT64_MIN) / m)) return false;
    const std::int64_t product = acc * m;
    if (a > 0 && product > (INT64_MAX)-a) return false;
    if (a < 0 && product < (INT64_MIN)-a) return false;
    out = product + a;
    return true;
}

}  // namespace

bool TensorShape::total_elements(std::int64_t& out) const {
    std::int64_t count = 1;
    for (const TensorDim d : dims) {
        if (!checked_mul_add(count, d, 0, count)) return false;
    }
    out = count;
    return true;
}

TensorStatus TensorShape::validate(std::string& error) const {
    if (dims.size() > kMaxTensorRank) {
        error = "tensor rank " + std::to_string(dims.size()) +
                " exceeds the project limit of " + std::to_string(kMaxTensorRank);
        return TensorStatus::ResourceLimitExceeded;
    }
    for (const TensorDim d : dims) {
        if (d < 0) {
            error = "negative tensor dimension (" + std::to_string(d) + ") is refused";
            return TensorStatus::InvalidShape;
        }
    }
    std::int64_t elements = 0;
    if (!total_elements(elements)) {
        error = "tensor element count overflows int64 — refused, never wrapped";
        return TensorStatus::ResourceLimitExceeded;
    }
    return TensorStatus::Ok;
}

std::string TensorShape::describe() const {
    std::ostringstream os;
    os << "[";
    for (std::size_t i = 0; i < dims.size(); ++i) {
        if (i != 0) os << ", ";
        os << dims[i];
    }
    os << "]";
    return os.str();
}

bool contiguous_strides(const TensorShape& shape, std::vector<std::int64_t>& out,
                        std::string& error) {
    out.assign(shape.rank(), 1);
    std::int64_t running = 1;
    for (std::size_t i = shape.rank(); i-- > 0;) {
        out[i] = running;
        if (!checked_mul_add(running, shape.dims[i], 0, running)) {
            error = "contiguous stride computation overflows for shape " + shape.describe();
            return false;
        }
    }
    return true;
}

const char* to_string(LayoutKind kind) {
    switch (kind) {
        case LayoutKind::RowMajorContiguous: return "row_major_contiguous";
        case LayoutKind::Strided: return "strided";
    }
    return "unknown";
}

TensorLayout TensorLayout::contiguous(const TensorShape& shape, std::string& error) {
    TensorLayout layout;
    layout.kind = LayoutKind::RowMajorContiguous;
    if (!contiguous_strides(shape, layout.strides, error)) {
        layout.strides.clear();
        return layout;
    }
    return layout;
}

bool TensorLayout::is_row_major_contiguous_for(const TensorShape& shape) const {
    if (strides.size() != shape.rank()) return false;
    std::vector<std::int64_t> canonical;
    std::string error;
    if (!contiguous_strides(shape, canonical, error)) return false;
    return strides == canonical;
}

TensorStatus TensorLayout::validate(const TensorShape& shape, std::int64_t storage_elements,
                                    std::string& error) const {
    if (strides.size() != shape.rank()) {
        error = "stride count (" + std::to_string(strides.size()) +
                ") does not match shape rank (" + std::to_string(shape.rank()) + ")";
        return TensorStatus::InvalidStride;
    }
    for (const std::int64_t s : strides) {
        if (s < 0) {
            error = "negative strides are refused (Phase 13 supports non-negative strides)";
            return TensorStatus::InvalidStride;
        }
    }
    // The maximum reachable offset: sum over dimensions of (dims[d]-1) * stride[d],
    // skipping broadcast strides (stride 0 contributes nothing). CHECKED
    // ARITHMETIC end to end: the per-dimension product and the running sum
    // are both overflow-guarded. (Regression: the Phase 13 audit — the
    // product was previously computed unchecked, so a hostile stride meta
    // could reach signed-overflow UB here before the refusal below;
    // UBSan-verified via parse_tensor_meta.)
    std::int64_t max_offset = 0;
    for (std::size_t d = 0; d < shape.rank(); ++d) {
        const std::int64_t span = shape.dims[d] - 1;
        std::int64_t reach = 0;
        if (span != 0 && strides[d] != 0) {
            if (!checked_mul_add(span, strides[d], 0, reach)) {
                error = "stride offset computation overflows for shape " + shape.describe();
                return TensorStatus::InvalidStride;
            }
        }
        if (reach != 0) {
            const std::int64_t next = max_offset + reach;
            if (max_offset > 0 && reach > 0 && next < max_offset) {
                error = "stride offset computation overflows for shape " + shape.describe();
                return TensorStatus::InvalidStride;
            }
            max_offset = next;
            if (max_offset < 0) {
                error = "stride offset computation overflows for shape " + shape.describe();
                return TensorStatus::InvalidStride;
            }
        }
    }
    if (storage_elements > 0 && max_offset >= storage_elements) {
        error = "strides reach element offset " + std::to_string(max_offset) +
                " but storage holds only " + std::to_string(storage_elements) + " elements";
        return TensorStatus::InvalidStride;
    }
    if (storage_elements <= 0) {
        // The caller validated the tensor before this check: a storage span of
        // zero can only appear with a zero-element shape, which the tensor
        // creation path refuses before layouts are built. Reaching here means
        // the caller skipped that refusal — an explicit error, not UB.
        error = "layout validated against an empty storage span";
        return TensorStatus::InvalidStride;
    }
    return TensorStatus::Ok;
}

bool linear_offset(const TensorShape& shape, const TensorLayout& layout,
                   const std::vector<std::int64_t>& indices, std::int64_t& out,
                   std::string& error) {
    if (indices.size() != shape.rank()) {
        error = "index count (" + std::to_string(indices.size()) +
                ") does not match shape rank (" + std::to_string(shape.rank()) + ")";
        return false;
    }
    if (layout.strides.size() != shape.rank()) {
        error = "layout stride count does not match shape rank";
        return false;
    }
    std::int64_t offset = 0;
    for (std::size_t d = 0; d < shape.rank(); ++d) {
        const std::int64_t i = indices[d];
        if (i < 0 || i >= shape.dims[d]) {
            error = "index " + std::to_string(i) + " out of bounds for dimension " +
                    std::to_string(d) + " (" + std::to_string(shape.dims[d]) + " elements)";
            return false;
        }
        // Checked accumulation: offset += i * stride[d]. The product itself
        // is overflow-guarded too (an unchecked i*stride could wrap for
        // extreme strides even with both factors in range — the same
        // checked-arithmetic contract the rest of this module documents).
        std::int64_t contribution = 0;
        if (i != 0 && layout.strides[d] != 0) {
            if (!checked_mul_add(i, layout.strides[d], 0, contribution)) {
                error = "linear offset computation overflows";
                return false;
            }
        }
        if (contribution != 0) {
            if ((contribution > 0 && offset > INT64_MAX - contribution) ||
                (contribution < 0 && offset < INT64_MIN - contribution)) {
                error = "linear offset computation overflows";
                return false;
            }
        }
        offset += contribution;
    }
    out = offset;
    return true;
}

TensorStatus broadcast_shapes(const TensorShape& a, const TensorShape& b, TensorShape& out,
                              std::string& error) {
    const std::size_t ra = a.rank();
    const std::size_t rb = b.rank();
    const std::size_t r = std::max(ra, rb);
    std::vector<TensorDim> result(r, 1);
    for (std::size_t i = 0; i < r; ++i) {
        const TensorDim da = (i < r - ra) ? 1 : a.dims[i - (r - ra)];
        const TensorDim db = (i < r - rb) ? 1 : b.dims[i - (r - rb)];
        if (da == db || da == 1) {
            result[i] = db;
        } else if (db == 1) {
            result[i] = da;
        } else {
            error = "broadcast incompatible: dimension " + std::to_string(i) + " (" +
                    std::to_string(da) + " vs " + std::to_string(db) + ")";
            return TensorStatus::InvalidShape;
        }
    }
    out = TensorShape(std::move(result));
    return TensorStatus::Ok;
}

TensorStatus broadcast_strides(const TensorShape& source, const TensorLayout& layout,
                               const TensorShape& target, TensorLayout& out,
                               std::string& error) {
    if (!is_broadcast_compatible(source, target)) {
        error = "shape " + source.describe() + " cannot be broadcast to " + target.describe();
        return TensorStatus::InvalidShape;
    }
    if (layout.strides.size() != source.rank()) {
        error = "source layout does not match the source shape";
        return TensorStatus::InvalidStride;
    }
    TensorLayout result;
    result.kind = LayoutKind::Strided;
    result.strides.assign(target.rank(), 0);
    const std::size_t rs = source.rank();
    const std::size_t rt = target.rank();
    for (std::size_t i = 0; i < rs; ++i) {
        const std::int64_t stride = layout.strides[i];
        const TensorDim src_dim = source.dims[i];
        const TensorDim tgt_dim = target.dims[rt - rs + i];
        // A dimension stretched by broadcast (src 1 -> target >1) gets stride 0;
        // an unstretched dimension keeps its stride.
        result.strides[rt - rs + i] = (src_dim == 1 && tgt_dim != 1) ? 0 : stride;
    }
    out = std::move(result);
    return TensorStatus::Ok;
}

bool is_broadcast_compatible(const TensorShape& source, const TensorShape& target) {
    if (source.rank() > target.rank()) return false;
    const std::size_t rs = source.rank();
    const std::size_t rt = target.rank();
    for (std::size_t i = 0; i < rs; ++i) {
        const TensorDim sd = source.dims[i];
        const TensorDim td = target.dims[rt - rs + i];
        if (sd != td && sd != 1) return false;
    }
    return true;
}

}  // namespace vortyx::tensor
