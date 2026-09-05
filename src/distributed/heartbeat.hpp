#pragma once

// Heartbeat / health monitoring (Phase 12).
//
// The monitor turns TIME into an explicit HEALTH JUDGMENT — it never
// claims to know why a device went quiet. A device whose last heartbeat is
// older than the configured timeout is marked Unhealthy and, per the
// documented device state table, transitioned Offline (Ready/Busy ->
// Offline is a legal transition; the monitor only ever uses legal ones).
// Unhealthy devices are excluded from placement by the snapshot's
// candidate filter; recovery is a heartbeat (the registry's documented
// proof-of-life path).
//
// DETERMINISM: all time comes from the injected IClock. check() is pure
// bookkeeping — no sleeps, no threads. The orchestrator calls check()
// before planning (so placement only sees fresh liveness); tests call it
// directly with a FakeClock and pin every transition.
//
// Failure counts: the monitor records how many checks found a device
// timed out (an honest counter of OBSERVED timeouts — not a prediction of
// failures).

#include <memory>
#include <string>

#include "distributed/clock.hpp"
#include "distributed/registry.hpp"
#include "platform/identity.hpp"

namespace vortyx::distributed {

using vortyx::platform::UserId;  // reused platform identity (see device.hpp)

class HeartbeatMonitor {
public:
    // 'timeout_ms' must be positive (validated: Status::InvalidInput via
    // valid() == false otherwise — construction never throws).
    HeartbeatMonitor(IDeviceRegistry& registry, std::shared_ptr<IClock> clock,
                     std::int64_t timeout_ms);

    // True when the monitor's parameters are usable.
    bool valid() const { return timeout_ms_ > 0; }

    // Marks every of the OWNER's devices whose last heartbeat is older
    // than the timeout: health -> Unhealthy, state -> Offline (for
    // Ready/Busy/Draining devices; Registering/Failed stay; Offline stays).
    // Ownership-scoped like every registry operation — the monitor acts AS
    // this owner (Phase 12's local cluster has one owner per orchestrator;
    // a multi-tenant control plane iterates owners behind the same
    // judgment logic). Returns the number of devices NEWLY judged timed
    // out by THIS check.
    std::size_t check(const UserId& owner_user_id);

    // The last check's timestamp (registry-clock ms; 0 = never checked).
    std::int64_t last_check_ms() const { return last_check_ms_; }

    // How many timeouts were observed in total across all checks
    // (observability — an honest counter, not a prediction).
    std::size_t total_timeouts_observed() const { return total_timeouts_; }

private:
    IDeviceRegistry& registry_;
    std::shared_ptr<IClock> clock_;
    std::int64_t timeout_ms_;

    std::int64_t last_check_ms_ = 0;
    std::size_t total_timeouts_ = 0;
};

}  // namespace vortyx::distributed
