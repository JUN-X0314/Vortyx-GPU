#include "core/compute/task.hpp"

namespace vortyx::compute {

const char* to_string(Status status) {
    switch (status) {
        case Status::Ok: return "Ok";
        case Status::InvalidInput: return "InvalidInput";
        case Status::NotInitialized: return "NotInitialized";
        case Status::BackendUnavailable: return "BackendUnavailable";
        case Status::BackendError: return "BackendError";
    }
    return "Unknown";
}

Status validate_vector_add(const VectorAddTask& task) {
    if (task.a.size() != task.b.size()) {
        return Status::InvalidInput;
    }
    if (task.a.empty()) {
        return Status::InvalidInput;
    }
    return Status::Ok;
}

Status validate_vector_add_buffers(const vortyx::resource::BufferDesc& a,
                                   const vortyx::resource::BufferDesc& b,
                                   const vortyx::resource::BufferDesc& c,
                                   std::string& error) {
    error.clear();

    // Element representation: vector addition is defined over int32 elements.
    if (a.element_size != sizeof(std::int32_t) || b.element_size != sizeof(std::int32_t) ||
        c.element_size != sizeof(std::int32_t)) {
        error = "vector addition requires int32 element buffers (element_size = 4 bytes); got a=" +
                std::to_string(a.element_size) + ", b=" + std::to_string(b.element_size) + ", c=" +
                std::to_string(c.element_size);
        return Status::InvalidInput;
    }

    // Equal, non-zero element counts across all three buffers.
    if (a.element_count == 0 || a.element_count != b.element_count ||
        a.element_count != c.element_count) {
        error = "buffer element counts must be equal and non-zero (a=" +
                std::to_string(a.element_count) + ", b=" + std::to_string(b.element_count) +
                ", c=" + std::to_string(c.element_count) + ")";
        return Status::InvalidInput;
    }

    // Declared device-side roles: inputs readable, output writable.
    using vortyx::resource::has_access;
    using vortyx::resource::ResourceAccess;
    if (!has_access(a.access, ResourceAccess::Read) || !has_access(b.access, ResourceAccess::Read)) {
        error = "input buffers must be created with ResourceAccess::Read (a, b)";
        return Status::InvalidInput;
    }
    if (!has_access(c.access, ResourceAccess::Write)) {
        error = "output buffer must be created with ResourceAccess::Write (c)";
        return Status::InvalidInput;
    }

    return Status::Ok;
}

}  // namespace vortyx::compute
