// Resource model implementation (Phase 12).

#include "distributed/resource.hpp"

namespace vortyx::distributed {

bool resource_vector_valid(const ResourceVector& vector) {
    return vector.compute_units >= 0 && vector.memory_bytes >= 0 &&
           vector.concurrent_jobs >= 0;
}

bool resource_vector_fits(const ResourceVector& capacity, const ResourceVector& used,
                          const ResourceVector& request) {
    if (!resource_vector_valid(capacity) || !resource_vector_valid(used) ||
        !resource_vector_valid(request)) {
        return false;
    }
    return used.compute_units + request.compute_units <= capacity.compute_units &&
           used.memory_bytes + request.memory_bytes <= capacity.memory_bytes &&
           used.concurrent_jobs + request.concurrent_jobs <= capacity.concurrent_jobs;
}

ResourceVector resource_vector_add(const ResourceVector& a, const ResourceVector& b) {
    ResourceVector out;
    out.compute_units = a.compute_units + b.compute_units;
    out.memory_bytes = a.memory_bytes + b.memory_bytes;
    out.concurrent_jobs = a.concurrent_jobs + b.concurrent_jobs;
    return out;
}

ResourceVector resource_vector_sub(const ResourceVector& a, const ResourceVector& b) {
    ResourceVector out;
    out.compute_units = a.compute_units > b.compute_units ? a.compute_units - b.compute_units : 0;
    out.memory_bytes = a.memory_bytes > b.memory_bytes ? a.memory_bytes - b.memory_bytes : 0;
    out.concurrent_jobs =
        a.concurrent_jobs > b.concurrent_jobs ? a.concurrent_jobs - b.concurrent_jobs : 0;
    return out;
}

bool resource_vector_le(const ResourceVector& a, const ResourceVector& b) {
    return a.compute_units <= b.compute_units && a.memory_bytes <= b.memory_bytes &&
           a.concurrent_jobs <= b.concurrent_jobs;
}

std::string to_string(const ResourceVector& vector) {
    // Stable field order; plain string building (no iostream in library
    // code, matching the platform layer's serializer style).
    std::string out = "compute=";
    out += std::to_string(vector.compute_units);
    out += " memory=";
    out += std::to_string(vector.memory_bytes);
    out += " jobs=";
    out += std::to_string(vector.concurrent_jobs);
    return out;
}

bool shard_memory_bytes(std::uint64_t element_count, vortyx::compute::ComputeOp operation,
                        std::int64_t& out_bytes, std::string& error) {
    // int32 elements are 4 bytes each.
    constexpr std::uint64_t kElementBytes = 4;

    // Buffers resident on the device for one shard: a + b inputs + c output
    // for the two-input ops; a input + c output for VectorScale.
    std::uint64_t buffer_count = 0;
    switch (operation) {
        case vortyx::compute::ComputeOp::VectorAdd:
        case vortyx::compute::ComputeOp::VectorMultiply:
            buffer_count = 3;
            break;
        case vortyx::compute::ComputeOp::VectorScale:
            buffer_count = 2;
            break;
        default:
            error = "unknown operation";
            return false;
    }

    // Overflow refusal: element_count * 4 * buffer_count must fit int64.
    const std::uint64_t per_element = kElementBytes * buffer_count;
    const std::uint64_t max_elements =
        static_cast<std::uint64_t>(INT64_MAX) / per_element;
    if (element_count > max_elements) {
        error = "element count exceeds the addressable memory range";
        return false;
    }
    out_bytes = static_cast<std::int64_t>(element_count * per_element);
    error.clear();
    return true;
}

}  // namespace vortyx::distributed
