#pragma once

// Device leases (Phase 12) — the reservation records of the resource model.
//
// A scheduler DECIDES to place a shard on a device; a lease is what makes
// that decision REAL. The registry grants a lease atomically (under its
// lock, against the device's remaining capacity), which is what prevents
// the classic overcommit race — two schedulers that both see "device free"
// cannot both hold capacity the device does not have.
//
// Lease lifecycle (all states testable with the injected clock):
//   Active    — capacity is held. Created by the registry's reserve().
//   Released  — returned explicitly (execution finished / failed / lease
//               guard destroyed). Capacity is freed; the record is dropped.
//   Expired   — the holder never returned it before expires_at_ms; the
//               registry reclaims the capacity lazily (on the next
//               reserve/heartbeat/snapshot that observes the expiry) and
//               reports the lease as expired to its caller. Leases are a
//               bounded-time safety net, not a permanent grant — a crashed
//               holder cannot leak capacity forever.
//
// RAII: LeaseGuard wraps a lease obtained from the registry and releases it
// on destruction unless explicitly detached, so every error path between
// reservation and hand-off returns capacity even when it unwinds early.
// (The project avoids exceptions, but the guard costs nothing and keeps the
// no-leak invariant structural instead of relying on every call site.)

#include <cstdint>
#include <string>
#include <vector>

#include "distributed/resource.hpp"
#include "platform/identity.hpp"  // DeviceId, JobId (reused)

namespace vortyx::distributed {

using vortyx::platform::DeviceId;  // reused platform identity (see device.hpp)
using vortyx::platform::JobId;

enum class LeaseState {
    Active,    // capacity held
    Released,  // returned by the holder
    Expired,   // reclaimed by the registry after expires_at_ms
};

const char* to_string(LeaseState state);

struct DeviceLease {
    // Lease ids are registry-issued, deterministic, provider-neutral
    // strings (same charset rules as the platform ids): "lease-" + a
    // monotonically increasing registry-local counter. Unique within one
    // registry's lifetime; NOT a UUID because no randomness is needed for
    // a locally-issued, locally-compared token.
    std::string lease_id;

    DeviceId device_id;
    JobId job_id;      // which logical job holds the capacity
    std::string shard_id;  // which shard ("" = a job-level reservation)

    ResourceVector resources;  // exactly what was reserved

    std::int64_t created_at_ms = 0;
    std::int64_t expires_at_ms = 0;  // created_at + ttl; never fabricated
};

// The observed state of a lease at time 'now_ms' (registry-clock monotonic
// ms): Active while now_ms <= expires_at_ms, Expired strictly after. Pure.
LeaseState lease_state_at(const DeviceLease& lease, std::int64_t now_ms);

// ---------------------------------------------------------------------------
// RAII reservation guard
// ---------------------------------------------------------------------------

class IDeviceRegistry;  // forward (complete type needed only in the .cpp)

// Owns one Active lease. Releases it through the issuing registry on
// destruction unless released()/detached(). Moveable (ownership transfer),
// not copyable. Default-constructed guards hold nothing.
class LeaseGuard {
public:
    LeaseGuard() = default;
    LeaseGuard(IDeviceRegistry* registry, DeviceLease lease);
    ~LeaseGuard();

    LeaseGuard(const LeaseGuard&) = delete;
    LeaseGuard& operator=(const LeaseGuard&) = delete;
    LeaseGuard(LeaseGuard&& other) noexcept;
    LeaseGuard& operator=(LeaseGuard&& other) noexcept;

    // True while this guard still holds an Active lease.
    bool holding() const noexcept { return active_; }

    // The lease (valid while holding()).
    const DeviceLease& lease() const noexcept { return lease_; }

    // Returns the capacity now; the guard becomes empty and its destructor
    // will do nothing.
    void release();

    // Detaches WITHOUT releasing: ownership moves to the caller (used when
    // a lease is handed over to a longer-lived execution record).
    DeviceLease detach();

private:
    IDeviceRegistry* registry_ = nullptr;
    DeviceLease lease_;
    bool active_ = false;
};

}  // namespace vortyx::distributed
