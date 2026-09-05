#pragma once

// DeviceRegistry (Phase 12) — the cluster's device bookkeeping.
//
// Responsibilities (exactly these): device registration (IDEMPOTENT for an
// identical re-registration), unregistration, lookup, listing, state
// updates (transition-table enforced), heartbeat updates, capability
// updates, resource accounting, atomic resource reservation (leases), and
// immutable cluster snapshots with a monotonically increasing revision.
//
// Registration semantics (documented, tested):
//   - A NEW DeviceId is registered in state Registering with
//     health Unknown and the current clock stamp.
//   - Re-registering the SAME DeviceId with the SAME owner and an
//     IDENTICAL capability payload is an IDEMPOTENT replay: Status::Ok,
//     created == false, the existing record returned, a heartbeat is
//     recorded (the device just proved it is alive). This is the contract
//     Phase 12 chose for re-registration — deliberately different from
//     the platform store's job idempotency rule, because a device
//     re-announcing itself is a NORMAL event (restart, agent retry), not a
//     client bug.
//   - Re-registering with a DIFFERENT owner or a DIFFERENT capability
//     payload is Status::Conflict (the error never reveals who owns the
//     existing registration — the Phase 11 anti-enumeration rule).
//
// Reservation semantics:
//   - reserve() checks capacity and records the lease ATOMICALLY under the
//     registry lock. Two concurrent reserves that both "see" free capacity
//     cannot both win (the second fails with Insufficient... — the
//     overcommit race is structurally impossible).
//   - A reserve attempt first reclaims any EXPIRED leases (lazy expiry,
//     deterministic with the injected clock) before checking capacity.
//   - release_lease() accepts only Active leases it issued (id/device/
//     resources must match what was recorded — a stale or foreign lease
//     object is refused); releasing an already-released or unknown lease
//     is a no-op failure, never a double free.
//   - Reserving is refused for devices that are not placement candidates
//     (state/health) — the registry is the second gate behind the policy.
//
// Snapshots and the cluster revision:
//   - Every mutation bumps a monotonically increasing revision counter.
//   - snapshot() returns an immutable ClusterSnapshot carrying the current
//     revision. Schedulers plan against snapshots (never against the live
//     registry); a plan records the revision it was based on, and the
//     orchestrator can detect a stale plan by comparing revisions before
//     executing it.
//
// Threading: one mutex guards all state (the same shape as the Phase 11
// InMemoryPlatformStore); lookups are linear scans over the device list,
// which is honest at registry scale (tens of devices) and deterministic in
// list order (registration order). No lock is held across user callbacks —
// there are none.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "distributed/clock.hpp"
#include "distributed/cluster.hpp"
#include "distributed/device.hpp"
#include "distributed/lease.hpp"
#include "platform/status.hpp"

namespace vortyx::distributed {

using vortyx::platform::DeviceId;  // reused platform identity (see device.hpp)
using vortyx::platform::UserId;

class IDeviceRegistry {
public:
    virtual ~IDeviceRegistry() = default;

    // Registers a device. See the module header for the idempotency rule.
    // 'created' is true for a fresh registration, false for an idempotent
    // replay. Errors: InvalidInput (bad id / capabilities) | Conflict |
    // Internal.
    virtual vortyx::platform::Status register_device(
        const DeviceId& device_id, const UserId& owner_user_id,
        const DeviceCapabilities& capabilities, DeviceDescriptor& out, bool& created) = 0;

    // Removes a device. Only its owner may remove it. Errors: InvalidInput
    // | Forbidden | NotFound | Internal. (Removing a device with active
    // leases first expires/leases-reclaims nothing: the caller must release
    // leases or let them expire; unregister REFUSES while Active leases
    // exist to make the leak rule explicit.)
    virtual vortyx::platform::Status unregister_device(const UserId& requester_user_id,
                                                       const DeviceId& device_id) = 0;

    // Fetches one device. Missing OR foreign -> NotFound (anti-enumeration,
    // the Phase 11 rule). Errors: InvalidInput | NotFound | Internal.
    virtual vortyx::platform::Status device(const UserId& requester_user_id,
                                            const DeviceId& device_id,
                                            DeviceDescriptor& out) = 0;

    // Lists the requester's devices in registration order.
    virtual vortyx::platform::Status devices(const UserId& requester_user_id,
                                             std::vector<DeviceDescriptor>& out) = 0;

    // Applies a state transition (the documented table in device.hpp).
    // Registering -> Ready is the registry's activation path. Errors:
    // InvalidInput (illegal transition | unknown device) | NotFound |
    // Internal.
    virtual vortyx::platform::Status update_device_state(const UserId& requester_user_id,
                                                         const DeviceId& device_id,
                                                         DeviceState to) = 0;

    // Records a heartbeat: refreshes the monotonic stamp, marks health
    // Healthy, and — the recovery rule — transitions an Offline device back
    // to Ready (the documented proof-of-life path). Errors: NotFound |
    // Internal.
    virtual vortyx::platform::Status heartbeat_device(const UserId& requester_user_id,
                                                      const DeviceId& device_id) = 0;

    // Sets the device's HEALTH classification (ownership-scoped, like
    // every other method — the heartbeat monitor calls this AS the owner
    // when a liveness judgment is made). Health changes are a judgment
    // record; the state machine is NOT affected here (state changes go
    // through update_device_state). Errors: NotFound | Internal.
    virtual vortyx::platform::Status set_device_health(const UserId& requester_user_id,
                                                       const DeviceId& device_id,
                                                       DeviceHealth health) = 0;

    // Replaces the capability payload (re-announcement). Same idempotency/
    // conflict shape as register_device. Errors: InvalidInput | NotFound |
    // Conflict | Internal.
    virtual vortyx::platform::Status update_device_capabilities(
        const UserId& requester_user_id, const DeviceId& device_id,
        const DeviceCapabilities& capabilities) = 0;

    // ---- resource reservation --------------------------------------------

    // Atomically reserves capacity on one device for one shard/job. On
    // success 'out' carries an Active lease and the device's allocated
    // vector includes the request. 'error' carries the human-readable
    // rejection reason on failure (insufficient memory / concurrency /
    // compute, non-schedulable state, unhealthy). Errors: InvalidInput |
    // NotFound (unknown device) | Internal.
    // 'ttl_ms' bounds the lease: expires_at_ms = now + ttl.
    virtual vortyx::platform::Status reserve(const UserId& requester_user_id,
                                             const DeviceId& device_id, const JobId& job_id,
                                             const std::string& shard_id,
                                             const ResourceVector& resources, std::int64_t ttl_ms,
                                             DeviceLease& out, std::string& error) = 0;

    // Returns an Active lease's capacity. Errors: InvalidInput (unknown or
    // non-Active lease or mismatched record) | Internal. Returns the lease
    // with state Released for observability; double release is refused.
    virtual vortyx::platform::Status release_lease(const DeviceLease& lease) = 0;

    // Looks up one lease by id (any state). NotFound when unknown.
    virtual vortyx::platform::Status lease(const std::string& lease_id,
                                           DeviceLease& out) = 0;

    // Expires any lease whose expires_at_ms has passed ('now_ms' from the
    // registry clock — injected for determinism) and returns how many were
    // reclaimed. Called lazily by reserve()/snapshot(); exposed for tests
    // and for an explicit sweep.
    virtual std::size_t expire_leases(std::int64_t now_ms) = 0;

    // ---- snapshot ----------------------------------------------------------

    // An immutable view of the cluster at the CURRENT revision (expired
    // leases reclaimed first). Never fails.
    virtual ClusterSnapshot snapshot() = 0;

    // The current revision (monotonically increasing; bumped by every
    // mutation including lease grants/releases).
    virtual std::uint64_t revision() const = 0;
};

// The local implementation: in-process, mutex-guarded, deterministic
// registration-order listing. Clearly a LOCAL/MOCK registry: it persists
// nothing and is not a distributed consensus store; remote/database-backed
// registries implement IDeviceRegistry behind the same seam.
class LocalDeviceRegistry final : public IDeviceRegistry {
public:
    // Non-owning clock pointer; the clock must outlive the registry (the
    // owner of both constructs them together).
    explicit LocalDeviceRegistry(std::shared_ptr<IClock> clock);

    LocalDeviceRegistry(const LocalDeviceRegistry&) = delete;
    LocalDeviceRegistry& operator=(const LocalDeviceRegistry&) = delete;
    LocalDeviceRegistry(LocalDeviceRegistry&&) = delete;
    LocalDeviceRegistry& operator=(LocalDeviceRegistry&&) = delete;

    vortyx::platform::Status register_device(const DeviceId& device_id,
                                             const UserId& owner_user_id,
                                             const DeviceCapabilities& capabilities,
                                             DeviceDescriptor& out, bool& created) override;
    vortyx::platform::Status unregister_device(const UserId& requester_user_id,
                                               const DeviceId& device_id) override;
    vortyx::platform::Status device(const UserId& requester_user_id, const DeviceId& device_id,
                                    DeviceDescriptor& out) override;
    vortyx::platform::Status devices(const UserId& requester_user_id,
                                     std::vector<DeviceDescriptor>& out) override;
    vortyx::platform::Status update_device_state(const UserId& requester_user_id,
                                                 const DeviceId& device_id,
                                                 DeviceState to) override;
    vortyx::platform::Status heartbeat_device(const UserId& requester_user_id,
                                              const DeviceId& device_id) override;
    vortyx::platform::Status set_device_health(const UserId& requester_user_id,
                                               const DeviceId& device_id,
                                               DeviceHealth health) override;
    vortyx::platform::Status update_device_capabilities(
        const UserId& requester_user_id, const DeviceId& device_id,
        const DeviceCapabilities& capabilities) override;

    vortyx::platform::Status reserve(const UserId& requester_user_id, const DeviceId& device_id,
                                     const JobId& job_id, const std::string& shard_id,
                                     const ResourceVector& resources, std::int64_t ttl_ms,
                                     DeviceLease& out, std::string& error) override;
    vortyx::platform::Status release_lease(const DeviceLease& lease) override;
    vortyx::platform::Status lease(const std::string& lease_id, DeviceLease& out) override;
    std::size_t expire_leases(std::int64_t now_ms) override;

    ClusterSnapshot snapshot() override;
    std::uint64_t revision() const override;

private:
    // Caller must hold mutex_. Finds by id; nullptr when absent.
    DeviceDescriptor* find_locked(const DeviceId& device_id);
    std::size_t reclaim_expired_locked(std::int64_t now_ms);
    void bump_revision_locked();

    std::shared_ptr<IClock> clock_;

    std::vector<DeviceDescriptor> devices_;       // registration order
    std::vector<DeviceLease> leases_;             // issued leases (any state)
    std::uint64_t next_lease_number_ = 1;         // deterministic lease ids
    std::uint64_t revision_ = 0;                  // cluster revision

    mutable std::mutex mutex_;
};

}  // namespace vortyx::distributed
