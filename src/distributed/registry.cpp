// LocalDeviceRegistry implementation (Phase 12).
//
// Every state change goes through the documented transition table, every
// reservation through the capacity gate, every mutation through the
// revision counter — the registry cannot drift from its own contract
// because it contains no other code path.

#include "distributed/registry.hpp"

namespace vortyx::distributed {

namespace {

// Common refusal text for single-record access to missing OR foreign
// devices: identical to what RLS produces (a foreign row is invisible).
constexpr const char* kNoSuchDevice = "no such device";

bool identical_capabilities(const DeviceCapabilities& a, const DeviceCapabilities& b) {
    return a.metadata.protocol_version == b.metadata.protocol_version &&
           a.metadata.software_version == b.metadata.software_version &&
           a.metadata.operating_system == b.metadata.operating_system &&
           a.metadata.architecture == b.metadata.architecture &&
           a.metadata.backends == b.metadata.backends &&
           a.metadata.operations == b.metadata.operations &&
           a.metadata.display_name == b.metadata.display_name &&
           a.capacity.compute_units == b.capacity.compute_units &&
           a.capacity.memory_bytes == b.capacity.memory_bytes &&
           a.capacity.concurrent_jobs == b.capacity.concurrent_jobs &&
           a.max_concurrent_shards == b.max_concurrent_shards;
}

}  // namespace

LocalDeviceRegistry::LocalDeviceRegistry(std::shared_ptr<IClock> clock)
    : clock_(std::move(clock)) {}

// ---------------------------------------------------------------------------
// Registration / listing
// ---------------------------------------------------------------------------

vortyx::platform::Status LocalDeviceRegistry::register_device(const DeviceId& device_id,
                                                              const UserId& owner_user_id,
                                                              const DeviceCapabilities& capabilities,
                                                              DeviceDescriptor& out,
                                                              bool& created) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    vortyx::platform::Status status =
        vortyx::platform::validate_id("device_id", device_id, error);
    if (status != vortyx::platform::Status::Ok) return status;
    status = vortyx::platform::validate_id("owner_user_id", owner_user_id, error);
    if (status != vortyx::platform::Status::Ok) return status;
    status = validate_device_capabilities(capabilities, error);
    if (status != vortyx::platform::Status::Ok) return status;

    // Idempotency rule (documented in registry.hpp): identical owner +
    // identical payload -> replay; anything else -> Conflict without
    // revealing the existing owner.
    for (const DeviceDescriptor& existing : devices_) {
        if (existing.device_id != device_id) continue;
        if (existing.owner_user_id == owner_user_id &&
            identical_capabilities(existing.capabilities, capabilities)) {
            // Idempotent replay: the device just proved it is alive — the
            // stamp refreshes, health turns Healthy, and the documented
            // recovery/activation paths apply (Offline -> Ready,
            // Registering -> Ready; a Failed device needs re-registration,
            // Ready/Busy/Draining keep their state).
            for (DeviceDescriptor& listed : devices_) {
                if (listed.device_id != device_id) continue;
                listed.last_heartbeat_ms = clock_->now_ms();
                listed.health = DeviceHealth::Healthy;
                if (listed.state == DeviceState::Offline ||
                    listed.state == DeviceState::Registering) {
                    listed.state = DeviceState::Ready;
                }
                out = listed;
                break;
            }
            created = false;
            bump_revision_locked();
            return vortyx::platform::Status::Ok;
        }
        error = "device_id is already registered with a different owner or capabilities";
        return vortyx::platform::Status::Conflict;
    }

    DeviceDescriptor descriptor;
    descriptor.device_id = device_id;
    descriptor.owner_user_id = owner_user_id;
    descriptor.capabilities = capabilities;
    descriptor.state = DeviceState::Registering;
    descriptor.health = DeviceHealth::Unknown;
    descriptor.last_heartbeat_ms = clock_->now_ms();
    descriptor.registered_at_ms = descriptor.last_heartbeat_ms;

    devices_.push_back(descriptor);
    out = descriptor;
    created = true;
    bump_revision_locked();
    return vortyx::platform::Status::Ok;
}

vortyx::platform::Status LocalDeviceRegistry::unregister_device(const UserId& requester_user_id,
                                                                const DeviceId& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    vortyx::platform::Status status =
        vortyx::platform::validate_id("device_id", device_id, error);
    if (status != vortyx::platform::Status::Ok) return status;

    for (std::size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].device_id != device_id) continue;
        if (devices_[i].owner_user_id != requester_user_id) {
            return vortyx::platform::Status::NotFound;  // foreign -> invisible
        }
        // Active leases pin the device: the leak rule is explicit, not
        // implicit. The holder must release (or the leases expire) first.
        const std::int64_t now = clock_->now_ms();
        for (const DeviceLease& lease : leases_) {
            if (lease.device_id == device_id &&
                lease_state_at(lease, now) == LeaseState::Active) {
                error = "device has active leases; release them before unregistering";
                return vortyx::platform::Status::InvalidInput;
            }
        }
        devices_.erase(devices_.begin() + static_cast<std::ptrdiff_t>(i));
        bump_revision_locked();
        return vortyx::platform::Status::Ok;
    }
    error = kNoSuchDevice;
    return vortyx::platform::Status::NotFound;
}

vortyx::platform::Status LocalDeviceRegistry::device(const UserId& requester_user_id,
                                                     const DeviceId& device_id,
                                                     DeviceDescriptor& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    vortyx::platform::Status status =
        vortyx::platform::validate_id("device_id", device_id, error);
    if (status != vortyx::platform::Status::Ok) return status;

    DeviceDescriptor* found = find_locked(device_id);
    if (found == nullptr || found->owner_user_id != requester_user_id) {
        error = kNoSuchDevice;
        return vortyx::platform::Status::NotFound;
    }
    out = *found;
    return vortyx::platform::Status::Ok;
}

vortyx::platform::Status LocalDeviceRegistry::devices(const UserId& requester_user_id,
                                                      std::vector<DeviceDescriptor>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out.clear();
    for (const DeviceDescriptor& descriptor : devices_) {
        if (descriptor.owner_user_id == requester_user_id) {
            out.push_back(descriptor);
        }
    }
    return vortyx::platform::Status::Ok;
}

// ---------------------------------------------------------------------------
// State / heartbeat / capabilities
// ---------------------------------------------------------------------------

vortyx::platform::Status LocalDeviceRegistry::update_device_state(const UserId& requester_user_id,
                                                                  const DeviceId& device_id,
                                                                  DeviceState to) {
    std::lock_guard<std::mutex> lock(mutex_);

    DeviceDescriptor* found = find_locked(device_id);
    if (found == nullptr || found->owner_user_id != requester_user_id) {
        return vortyx::platform::Status::NotFound;  // foreign -> invisible
    }
    if (!device_state_transition_valid(found->state, to)) {
        // The refusal text lives with the caller (the documented table is
        // public); the registry only refuses.
        return vortyx::platform::Status::InvalidInput;
    }
    found->state = to;
    bump_revision_locked();
    return vortyx::platform::Status::Ok;
}

vortyx::platform::Status LocalDeviceRegistry::heartbeat_device(const UserId& requester_user_id,
                                                               const DeviceId& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    DeviceDescriptor* found = find_locked(device_id);
    if (found == nullptr || found->owner_user_id != requester_user_id) {
        return vortyx::platform::Status::NotFound;  // foreign -> invisible
    }
    found->last_heartbeat_ms = clock_->now_ms();
    found->health = DeviceHealth::Healthy;  // the heartbeat is the evidence
    // Recovery path: Offline (or Registering) devices that prove liveness
    // come back Ready. Ready/Busy/Draining/Failed are not changed by a
    // bare heartbeat (Failed needs re-registration or the explicit
    // Offline step first).
    if (found->state == DeviceState::Offline || found->state == DeviceState::Registering) {
        found->state = DeviceState::Ready;
    }
    bump_revision_locked();
    return vortyx::platform::Status::Ok;
}

vortyx::platform::Status LocalDeviceRegistry::set_device_health(const UserId& requester_user_id,
                                                                const DeviceId& device_id,
                                                                DeviceHealth health) {
    std::lock_guard<std::mutex> lock(mutex_);

    DeviceDescriptor* found = find_locked(device_id);
    if (found == nullptr || found->owner_user_id != requester_user_id) {
        return vortyx::platform::Status::NotFound;  // foreign -> invisible
    }
    if (found->health == health) {
        return vortyx::platform::Status::Ok;  // no change, no revision bump
    }
    found->health = health;
    bump_revision_locked();
    return vortyx::platform::Status::Ok;
}

vortyx::platform::Status LocalDeviceRegistry::update_device_capabilities(
    const UserId& requester_user_id, const DeviceId& device_id,
    const DeviceCapabilities& capabilities) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    vortyx::platform::Status status = validate_device_capabilities(capabilities, error);
    if (status != vortyx::platform::Status::Ok) return status;

    DeviceDescriptor* found = find_locked(device_id);
    if (found == nullptr || found->owner_user_id != requester_user_id) {
        error = kNoSuchDevice;
        return vortyx::platform::Status::NotFound;
    }
    // A capability change that alters the payload is a real update; the
    // identical payload is a replay (Ok, no change). Conflicts with active
    // allocations are refused — silently shrinking capacity under live
    // leases would break the accounting invariants.
    if (!identical_capabilities(found->capabilities, capabilities)) {
        const std::int64_t now = clock_->now_ms();
        for (const DeviceLease& lease : leases_) {
            if (lease.device_id == device_id &&
                lease_state_at(lease, now) == LeaseState::Active) {
                error = "device has active leases; capabilities cannot change until they end";
                return vortyx::platform::Status::Conflict;
            }
        }
        found->capabilities = capabilities;
        bump_revision_locked();
    }
    return vortyx::platform::Status::Ok;
}

// ---------------------------------------------------------------------------
// Resource reservation
// ---------------------------------------------------------------------------

vortyx::platform::Status LocalDeviceRegistry::reserve(const UserId& requester_user_id,
                                                      const DeviceId& device_id,
                                                      const JobId& job_id,
                                                      const std::string& shard_id,
                                                      const ResourceVector& resources,
                                                      std::int64_t ttl_ms,
                                                      DeviceLease& out,
                                                      std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Deterministic expiry sweep first: expired leases free their capacity
    // before this request is judged.
    reclaim_expired_locked(clock_->now_ms());

    if (!resource_vector_valid(resources) ||
        (resources.compute_units == 0 && resources.memory_bytes == 0 &&
         resources.concurrent_jobs == 0)) {
        error = "reservation must request at least one resource";
        return vortyx::platform::Status::InvalidInput;
    }
    if (ttl_ms <= 0) {
        error = "lease ttl must be positive";
        return vortyx::platform::Status::InvalidInput;
    }

    DeviceDescriptor* found = find_locked(device_id);
    if (found == nullptr || found->owner_user_id != requester_user_id) {
        error = kNoSuchDevice;
        return vortyx::platform::Status::NotFound;
    }
    if (!device_state_schedulable(found->state)) {
        error = std::string("device is not schedulable (state: ") + to_string(found->state) + ")";
        return vortyx::platform::Status::InvalidInput;
    }
    if (found->health != DeviceHealth::Healthy) {
        error = std::string("device health is ") + to_string(found->health);
        return vortyx::platform::Status::InvalidInput;
    }
    if (resources.concurrent_jobs > found->capabilities.max_concurrent_shards) {
        error = "reservation exceeds the device's declared max_concurrent_shards";
        return vortyx::platform::Status::InvalidInput;
    }
    if (!resource_vector_fits(found->capabilities.capacity, found->allocated, resources)) {
        // Distinguish the scarce dimension honestly.
        const ResourceVector after = resource_vector_add(found->allocated, resources);
        if (after.memory_bytes > found->capabilities.capacity.memory_bytes) {
            error = "insufficient memory on device";
        } else if (after.concurrent_jobs > found->capabilities.capacity.concurrent_jobs) {
            error = "insufficient concurrency on device";
        } else {
            error = "insufficient compute on device";
        }
        return vortyx::platform::Status::InvalidInput;
    }

    DeviceLease lease;
    lease.lease_id = "lease-" + std::to_string(next_lease_number_);
    ++next_lease_number_;
    lease.device_id = device_id;
    lease.job_id = job_id;
    lease.shard_id = shard_id;
    lease.resources = resources;
    lease.created_at_ms = clock_->now_ms();
    lease.expires_at_ms = lease.created_at_ms + ttl_ms;

    found->allocated = resource_vector_add(found->allocated, resources);
    leases_.push_back(lease);
    out = lease;
    bump_revision_locked();
    return vortyx::platform::Status::Ok;
}

vortyx::platform::Status LocalDeviceRegistry::release_lease(const DeviceLease& lease) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    for (std::size_t i = 0; i < leases_.size(); ++i) {
        DeviceLease& recorded = leases_[i];
        if (recorded.lease_id != lease.lease_id) continue;
        // The release must match the issued record exactly (a stale or
        // altered lease object is refused) — a double release or a foreign
        // release can never free someone else's capacity.
        if (recorded.device_id != lease.device_id ||
            recorded.resources.compute_units != lease.resources.compute_units ||
            recorded.resources.memory_bytes != lease.resources.memory_bytes ||
            recorded.resources.concurrent_jobs != lease.resources.concurrent_jobs) {
            error = "lease record mismatch";
            return vortyx::platform::Status::InvalidInput;
        }
        const LeaseState state = lease_state_at(recorded, clock_->now_ms());
        if (state != LeaseState::Active) {
            error = std::string("lease is not active (state: ") + to_string(state) + ")";
            return vortyx::platform::Status::InvalidInput;
        }
        DeviceDescriptor* device = find_locked(recorded.device_id);
        if (device != nullptr) {
            device->allocated = resource_vector_sub(device->allocated, recorded.resources);
        }
        // The released record is dropped: history lives with the caller
        // (the lease object it holds), the registry keeps only live state.
        leases_.erase(leases_.begin() + static_cast<std::ptrdiff_t>(i));
        bump_revision_locked();
        return vortyx::platform::Status::Ok;
    }
    error = "no such lease";
    return vortyx::platform::Status::InvalidInput;
}

vortyx::platform::Status LocalDeviceRegistry::lease(const std::string& lease_id,
                                                    DeviceLease& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const DeviceLease& recorded : leases_) {
        if (recorded.lease_id == lease_id) {
            out = recorded;
            return vortyx::platform::Status::Ok;
        }
    }
    std::string error;
    error = "no such lease";
    return vortyx::platform::Status::NotFound;
}

std::size_t LocalDeviceRegistry::expire_leases(std::int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    return reclaim_expired_locked(now_ms);
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

ClusterSnapshot LocalDeviceRegistry::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    reclaim_expired_locked(clock_->now_ms());

    ClusterSnapshot view;
    view.revision = revision_;
    view.devices.reserve(devices_.size());
    for (const DeviceDescriptor& descriptor : devices_) {
        DeviceSnapshot device;
        device.device_id = descriptor.device_id;
        device.owner_user_id = descriptor.owner_user_id;
        device.capabilities = descriptor.capabilities;
        device.state = descriptor.state;
        device.health = descriptor.health;
        device.allocated = descriptor.allocated;
        device.last_heartbeat_ms = descriptor.last_heartbeat_ms;
        device.running_shards = descriptor.running_shards;
        view.devices.push_back(device);
    }
    return view;
}

std::uint64_t LocalDeviceRegistry::revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

// ---------------------------------------------------------------------------
// private helpers (mutex held)
// ---------------------------------------------------------------------------

DeviceDescriptor* LocalDeviceRegistry::find_locked(const DeviceId& device_id) {
    for (DeviceDescriptor& descriptor : devices_) {
        if (descriptor.device_id == device_id) return &descriptor;
    }
    return nullptr;
}

std::size_t LocalDeviceRegistry::reclaim_expired_locked(std::int64_t now_ms) {
    std::size_t reclaimed = 0;
    for (std::size_t i = 0; i < leases_.size();) {
        const DeviceLease& lease = leases_[i];
        if (now_ms > lease.expires_at_ms) {
            DeviceDescriptor* device = find_locked(lease.device_id);
            if (device != nullptr) {
                device->allocated = resource_vector_sub(device->allocated, lease.resources);
            }
            leases_.erase(leases_.begin() + static_cast<std::ptrdiff_t>(i));
            ++reclaimed;
            bump_revision_locked();
            continue;  // do not advance i: the erase shifted the tail
        }
        ++i;
    }
    return reclaimed;
}

void LocalDeviceRegistry::bump_revision_locked() { ++revision_; }

}  // namespace vortyx::distributed
