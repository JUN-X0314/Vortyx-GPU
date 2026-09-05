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

// ---------------------------------------------------------------------------
// Generic compute tasks (Phase 10 — Compute Engine)
// ---------------------------------------------------------------------------

const char* to_string(ComputeOp op) {
    switch (op) {
        case ComputeOp::VectorAdd: return "VectorAdd";
        case ComputeOp::VectorMultiply: return "VectorMultiply";
        case ComputeOp::VectorScale: return "VectorScale";
    }
    return "Unknown";
}

const char* workload_label(ComputeOp op) {
    switch (op) {
        case ComputeOp::VectorAdd: return "vector_add";
        case ComputeOp::VectorMultiply: return "vector_multiply";
        case ComputeOp::VectorScale: return "vector_scale";
    }
    return "unknown";
}

Status validate_compute_task(const ComputeTask& task, std::string& error) {
    error.clear();

    switch (task.op) {
        case ComputeOp::VectorAdd:
        case ComputeOp::VectorMultiply: {
            // Two-input elementwise op: equal, non-empty inputs; the scalar
            // is not an operand of these ops and must be left at its default.
            if (task.a.size() != task.b.size()) {
                error = std::string("compute task '") + to_string(task.op) +
                        "' has mismatched input sizes (a.size=" +
                        std::to_string(task.a.size()) + ", b.size=" +
                        std::to_string(task.b.size()) +
                        "); inputs must be non-empty and equal size";
                return Status::InvalidInput;
            }
            if (task.a.empty()) {
                error = std::string("compute task '") + to_string(task.op) +
                        "' has empty inputs (0 elements have nothing to compute)";
                return Status::InvalidInput;
            }
            if (task.scalar != 0) {
                error = std::string("compute task '") + to_string(task.op) +
                        "' carries a non-zero scalar (" +
                        std::to_string(task.scalar) +
                        "); this operation has no scalar operand — use VectorScale "
                        "or leave scalar at 0";
                return Status::InvalidInput;
            }
            return Status::Ok;
        }
        case ComputeOp::VectorScale: {
            // One-input op: the primary input drives the workload; carrying a
            // second input is a caller bug and is refused, never ignored.
            if (task.a.empty()) {
                error = std::string("compute task '") + to_string(task.op) +
                        "' has an empty input (0 elements have nothing to compute)";
                return Status::InvalidInput;
            }
            if (!task.b.empty()) {
                error = std::string("compute task '") + to_string(task.op) +
                        "' carries a second input (b.size=" +
                        std::to_string(task.b.size()) +
                        "); scaling takes exactly one input — leave b empty";
                return Status::InvalidInput;
            }
            return Status::Ok;
        }
    }
    error = "compute task has an unknown operation";
    return Status::InvalidInput;
}

Status validate_compute_dispatch_buffers(ComputeOp op,
                                         const vortyx::resource::BufferDesc& a_desc,
                                         const vortyx::resource::BufferDesc* b_desc,
                                         const vortyx::resource::BufferDesc& c_desc,
                                         std::string& error) {
    error.clear();

    // Op shape first: which inputs does this operation actually take?
    const bool two_input = (op == ComputeOp::VectorAdd || op == ComputeOp::VectorMultiply);
    if (two_input && b_desc == nullptr) {
        error = std::string("compute dispatch '") + to_string(op) +
                "' requires a second input buffer";
        return Status::InvalidInput;
    }
    if (!two_input && b_desc != nullptr) {
        error = std::string("compute dispatch '") + to_string(op) +
                "' takes exactly one input buffer (b must be null)";
        return Status::InvalidInput;
    }

    // Element representation: every current op is defined over int32.
    if (a_desc.element_size != sizeof(std::int32_t) ||
        c_desc.element_size != sizeof(std::int32_t) ||
        (two_input && b_desc->element_size != sizeof(std::int32_t))) {
        error = std::string("compute dispatch '") + to_string(op) +
                "' requires int32 element buffers (element_size = 4 bytes)";
        return Status::InvalidInput;
    }

    // Equal, non-zero element counts across all involved buffers.
    const std::size_t count = a_desc.element_count;
    if (count == 0 || c_desc.element_count != count ||
        (two_input && b_desc->element_count != count)) {
        error = std::string("compute dispatch '") + to_string(op) +
                "' buffer element counts must be equal and non-zero (a=" +
                std::to_string(a_desc.element_count) +
                ", b=" + (two_input ? std::to_string(b_desc->element_count) : std::string("n/a")) +
                ", c=" + std::to_string(c_desc.element_count) + ")";
        return Status::InvalidInput;
    }

    // Declared device-side roles: inputs readable, output writable.
    using vortyx::resource::has_access;
    using vortyx::resource::ResourceAccess;
    if (!has_access(a_desc.access, ResourceAccess::Read) ||
        (two_input && !has_access(b_desc->access, ResourceAccess::Read))) {
        error = "input buffers must be created with ResourceAccess::Read";
        return Status::InvalidInput;
    }
    if (!has_access(c_desc.access, ResourceAccess::Write)) {
        error = "output buffer must be created with ResourceAccess::Write (c)";
        return Status::InvalidInput;
    }

    return Status::Ok;
}

}  // namespace vortyx::compute
