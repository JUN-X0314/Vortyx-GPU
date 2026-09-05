// Heartbeat monitor implementation (Phase 12).

#include "distributed/heartbeat.hpp"

#include <vector>

namespace vortyx::distributed {

HeartbeatMonitor::HeartbeatMonitor(IDeviceRegistry& registry, std::shared_ptr<IClock> clock,
                                   std::int64_t timeout_ms)
    : registry_(registry), clock_(std::move(clock)), timeout_ms_(timeout_ms) {}

std::size_t HeartbeatMonitor::check(const UserId& owner_user_id) {
    const std::int64_t now = clock_->now_ms();
    last_check_ms_ = now;

    std::size_t newly_timed_out = 0;

    std::vector<DeviceDescriptor> devices;
    if (registry_.devices(owner_user_id, devices) != vortyx::platform::Status::Ok) {
        return 0;  // listing failed (invalid owner id); nothing judged
    }

    for (const DeviceDescriptor& device : devices) {
        // Timed out: the last PROOF OF LIFE is older than the timeout. A
        // device that never heartbeat since registration is judged on the
        // same rule (its registration stamp is its liveness evidence).
        if (device.last_heartbeat_ms > now ||
            now - device.last_heartbeat_ms <= timeout_ms_) {
            continue;  // fresh enough (or clock went backwards — never punish that)
        }

        const bool newly_unhealthy = device.health != DeviceHealth::Unhealthy;
        if (newly_unhealthy) {
            registry_.set_device_health(owner_user_id, device.device_id, DeviceHealth::Unhealthy);
            ++total_timeouts_;
            ++newly_timed_out;
        }
        // Schedulable devices leave service; Registering/Failed/Offline
        // are left to their own paths (Registering has not joined yet;
        // Failed is already out; Offline is already out).
        if (device_state_schedulable(device.state)) {
            registry_.update_device_state(owner_user_id, device.device_id, DeviceState::Offline);
        }
    }
    return newly_timed_out;
}

}  // namespace vortyx::distributed
