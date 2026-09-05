// Authentication / authorization boundary implementation (Phase 11).

#include "platform/auth.hpp"

namespace vortyx::platform {

AuthContext make_authenticated(UserId user_id) {
    AuthContext auth;
    auth.authenticated = true;
    auth.user_id = std::move(user_id);
    return auth;
}

AuthContext anonymous() {
    return AuthContext{};
}

Status validate_auth(const AuthContext& auth, std::string& error) {
    if (!auth.authenticated || auth.user_id.empty()) {
        error = "authentication required (no usable identity was presented)";
        return Status::Unauthenticated;
    }
    error.clear();
    return Status::Ok;
}

bool is_owner(const AuthContext& auth, const UserId& owner_user_id) {
    return auth.authenticated && !auth.user_id.empty() && auth.user_id == owner_user_id;
}

Status authorize_record_access(const AuthContext& auth, const UserId& owner_user_id) {
    std::string error;
    Status status = validate_auth(auth, error);
    if (status != Status::Ok) return status;

    if (!is_owner(auth, owner_user_id)) {
        return Status::Forbidden;
    }
    return Status::Ok;
}

}  // namespace vortyx::platform
