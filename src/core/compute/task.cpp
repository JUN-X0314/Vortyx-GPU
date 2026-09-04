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

}  // namespace vortyx::compute
