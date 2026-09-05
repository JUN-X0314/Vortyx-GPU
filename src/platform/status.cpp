// Platform status implementation (Phase 11).

#include "platform/status.hpp"

namespace vortyx::platform {

const char* to_string(Status status) {
    switch (status) {
        case Status::Ok: return "ok";
        case Status::InvalidInput: return "invalid_input";
        case Status::Unauthenticated: return "unauthenticated";
        case Status::Forbidden: return "forbidden";
        case Status::NotFound: return "not_found";
        case Status::Conflict: return "conflict";
        case Status::Internal: return "internal";
    }
    return "unknown";
}

}  // namespace vortyx::platform
