// Device lease implementation (Phase 12). The registry-side lease
// bookkeeping lives in registry.cpp; this file only implements the RAII
// guard and the state vocabulary.

#include "distributed/lease.hpp"

#include "distributed/registry.hpp"

namespace vortyx::distributed {

const char* to_string(LeaseState state) {
    switch (state) {
        case LeaseState::Active: return "active";
        case LeaseState::Released: return "released";
        case LeaseState::Expired: return "expired";
    }
    return "unknown";
}

LeaseState lease_state_at(const DeviceLease& lease, std::int64_t now_ms) {
    return now_ms > lease.expires_at_ms ? LeaseState::Expired : LeaseState::Active;
}

LeaseGuard::LeaseGuard(IDeviceRegistry* registry, DeviceLease lease)
    : registry_(registry), lease_(std::move(lease)), active_(true) {}

LeaseGuard::~LeaseGuard() {
    if (active_ && registry_ != nullptr) {
        registry_->release_lease(lease_);  // best-effort: registry reports expiry
    }
}

LeaseGuard::LeaseGuard(LeaseGuard&& other) noexcept
    : registry_(other.registry_), lease_(std::move(other.lease_)), active_(other.active_) {
    other.registry_ = nullptr;
    other.active_ = false;
}

LeaseGuard& LeaseGuard::operator=(LeaseGuard&& other) noexcept {
    if (this != &other) {
        if (active_ && registry_ != nullptr) {
            registry_->release_lease(lease_);
        }
        registry_ = other.registry_;
        lease_ = std::move(other.lease_);
        active_ = other.active_;
        other.registry_ = nullptr;
        other.active_ = false;
    }
    return *this;
}

void LeaseGuard::release() {
    if (active_ && registry_ != nullptr) {
        registry_->release_lease(lease_);
    }
    active_ = false;
}

DeviceLease LeaseGuard::detach() {
    active_ = false;
    DeviceLease out = std::move(lease_);
    lease_ = DeviceLease{};
    return out;
}

}  // namespace vortyx::distributed
