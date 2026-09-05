// Service health (Phase 14) — implementation.

#include "service/health.hpp"

#include "platform/json.hpp"

namespace vortyx::service {

const char* to_string(HealthValue value) {
    switch (value) {
        case HealthValue::Healthy: return "healthy";
        case HealthValue::Unhealthy: return "unhealthy";
        case HealthValue::Unknown: return "unknown";
        case HealthValue::NotConfigured: return "not_configured";
    }
    return "unknown";
}

HealthValue HealthReport::overall() const {
    HealthValue overall = HealthValue::Healthy;
    for (const ComponentHealth& component : components) {
        switch (component.status) {
            case HealthValue::Unhealthy:
                return HealthValue::Unhealthy;  // the worst wins immediately
            case HealthValue::Unknown:
            case HealthValue::NotConfigured:
                if (overall == HealthValue::Healthy) overall = component.status;
                break;
            case HealthValue::Healthy:
                break;  // keeps the running best
        }
    }
    return overall;
}

std::string HealthReport::serialize() const {
    using vortyx::platform::JsonValue;
    JsonValue root = JsonValue::make_object();
    root.add("checked_at_ms", JsonValue::make_number(static_cast<double>(checked_at_ms)));
    root.add("overall", JsonValue::make_string(to_string(overall())));

    JsonValue components_json = JsonValue::make_array();
    for (const ComponentHealth& component : components) {
        JsonValue entry = JsonValue::make_object();
        entry.add("component", JsonValue::make_string(component.component));
        entry.add("status", JsonValue::make_string(to_string(component.status)));
        entry.add("detail", JsonValue::make_string(component.detail));
        components_json.push(std::move(entry));
    }
    root.add("components", std::move(components_json));

    JsonValue devices_json = JsonValue::make_object();
    devices_json.add("total", JsonValue::make_number(static_cast<double>(devices.total)));
    devices_json.add("healthy", JsonValue::make_number(static_cast<double>(devices.healthy)));
    devices_json.add("unhealthy", JsonValue::make_number(static_cast<double>(devices.unhealthy)));
    devices_json.add("offline", JsonValue::make_number(static_cast<double>(devices.offline)));
    root.add("devices", std::move(devices_json));

    return root.serialize();
}

}  // namespace vortyx::service
