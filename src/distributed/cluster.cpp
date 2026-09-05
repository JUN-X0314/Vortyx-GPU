// Cluster snapshot + topology implementation (Phase 12).

#include "distributed/cluster.hpp"

namespace vortyx::distributed {

// ---- topology ------------------------------------------------------------

const char* to_string(LinkType type) {
    switch (type) {
        case LinkType::Unknown: return "unknown";
        case LinkType::Local: return "local";
        case LinkType::SharedMemory: return "shared_memory";
        case LinkType::Pcie: return "pcie";
        case LinkType::Network: return "network";
    }
    return "unknown";
}

const TopologyLink* TopologyView::link_between(const DeviceId& a, const DeviceId& b) const {
    for (const TopologyLink& link : links) {
        if ((link.device_a == a && link.device_b == b) ||
            (link.device_a == b && link.device_b == a)) {
            return &link;
        }
    }
    return nullptr;
}

TopologyView make_static_topology(const std::vector<TopologyLink>& links) {
    TopologyView view;
    view.links = links;
    return view;
}

// ---- snapshot ------------------------------------------------------------

ResourceVector DeviceSnapshot::available() const {
    return resource_vector_sub(capabilities.capacity, allocated);
}

std::vector<DeviceSnapshot> ClusterSnapshot::visible_for(const UserId& owner_user_id) const {
    std::vector<DeviceSnapshot> out;
    for (const DeviceSnapshot& device : devices) {
        if (device.owner_user_id == owner_user_id) {
            out.push_back(device);
        }
    }
    return out;
}

std::vector<DeviceSnapshot> ClusterSnapshot::candidates_for(const UserId& owner_user_id) const {
    std::vector<DeviceSnapshot> out;
    for (const DeviceSnapshot& device : devices) {
        // Ownership FIRST (foreign devices are invisible — the Phase 11
        // rule applied to the cluster), then schedulability, then health.
        // Unknown health is NOT a candidate: unknown is never guessed into
        // usable.
        if (device.owner_user_id != owner_user_id) continue;
        if (!device_state_schedulable(device.state)) continue;
        if (device.health != DeviceHealth::Healthy) continue;
        out.push_back(device);
    }
    return out;
}

bool ClusterSnapshot::empty_for(const UserId& owner_user_id) const {
    return candidates_for(owner_user_id).empty();
}

}  // namespace vortyx::distributed
