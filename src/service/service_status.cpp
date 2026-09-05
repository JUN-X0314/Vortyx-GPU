// Service layer result vocabulary (Phase 14) — implementation.

#include "service/service_status.hpp"

namespace vortyx::service {

const char* to_string(ServiceStatus status) {
    switch (status) {
        case ServiceStatus::Ok: return "Ok";
        case ServiceStatus::InvalidInput: return "InvalidInput";
        case ServiceStatus::Unauthenticated: return "Unauthenticated";
        case ServiceStatus::Forbidden: return "Forbidden";
        case ServiceStatus::NotFound: return "NotFound";
        case ServiceStatus::Conflict: return "Conflict";
        case ServiceStatus::QuotaExceeded: return "QuotaExceeded";
        case ServiceStatus::RateLimitExceeded: return "RateLimitExceeded";
        case ServiceStatus::UnsupportedOperation: return "UnsupportedOperation";
        case ServiceStatus::Unavailable: return "Unavailable";
        case ServiceStatus::Internal: return "Internal";
    }
    return "Unknown";
}

const char* service_status_code(ServiceStatus status) {
    switch (status) {
        case ServiceStatus::Ok: return "ok";
        case ServiceStatus::InvalidInput: return "invalid_input";
        case ServiceStatus::Unauthenticated: return "unauthenticated";
        case ServiceStatus::Forbidden: return "forbidden";
        case ServiceStatus::NotFound: return "not_found";
        case ServiceStatus::Conflict: return "conflict";
        case ServiceStatus::QuotaExceeded: return "quota_exceeded";
        case ServiceStatus::RateLimitExceeded: return "rate_limit_exceeded";
        case ServiceStatus::UnsupportedOperation: return "unsupported_operation";
        case ServiceStatus::Unavailable: return "unavailable";
        case ServiceStatus::Internal: return "internal";
    }
    return "unknown";
}

bool service_status_from_code(const std::string& code, ServiceStatus& out) {
    // Linear scan over the full vocabulary: the table is small, the scan is
    // deterministic, and an unknown code is refused (never guessed).
    static const struct {
        ServiceStatus status;
        const char* code;
    } kTable[] = {
        {ServiceStatus::Ok, "ok"},
        {ServiceStatus::InvalidInput, "invalid_input"},
        {ServiceStatus::Unauthenticated, "unauthenticated"},
        {ServiceStatus::Forbidden, "forbidden"},
        {ServiceStatus::NotFound, "not_found"},
        {ServiceStatus::Conflict, "conflict"},
        {ServiceStatus::QuotaExceeded, "quota_exceeded"},
        {ServiceStatus::RateLimitExceeded, "rate_limit_exceeded"},
        {ServiceStatus::UnsupportedOperation, "unsupported_operation"},
        {ServiceStatus::Unavailable, "unavailable"},
        {ServiceStatus::Internal, "internal"},
    };
    for (const auto& entry : kTable) {
        if (code == entry.code) {
            out = entry.status;
            return true;
        }
    }
    return false;
}

ServiceStatus service_status_from_platform(vortyx::platform::Status status) {
    switch (status) {
        case vortyx::platform::Status::Ok: return ServiceStatus::Ok;
        case vortyx::platform::Status::InvalidInput: return ServiceStatus::InvalidInput;
        case vortyx::platform::Status::Unauthenticated: return ServiceStatus::Unauthenticated;
        case vortyx::platform::Status::Forbidden: return ServiceStatus::Forbidden;
        case vortyx::platform::Status::NotFound: return ServiceStatus::NotFound;
        case vortyx::platform::Status::Conflict: return ServiceStatus::Conflict;
        case vortyx::platform::Status::Internal: return ServiceStatus::Internal;
    }
    return ServiceStatus::Internal;
}

int service_status_http(ServiceStatus status) {
    switch (status) {
        case ServiceStatus::Ok: return 200;
        case ServiceStatus::InvalidInput: return 422;
        case ServiceStatus::Unauthenticated: return 401;
        case ServiceStatus::Forbidden: return 403;
        case ServiceStatus::NotFound: return 404;
        case ServiceStatus::Conflict: return 409;
        case ServiceStatus::QuotaExceeded: return 429;
        case ServiceStatus::RateLimitExceeded: return 429;
        case ServiceStatus::UnsupportedOperation: return 422;
        case ServiceStatus::Unavailable: return 503;
        case ServiceStatus::Internal: return 500;
    }
    return 500;
}

}  // namespace vortyx::service
