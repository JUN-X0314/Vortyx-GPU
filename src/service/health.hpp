#pragma once

// Service health (Phase 14).
//
// Per-component health with HONEST values:
//   Healthy      — the component answered and reports itself working
//   Unhealthy    — the component answered and reports a problem
//   Unknown      — the component's health cannot be judged from here
//   NotConfigured — the component is OPTIONAL and not attached (a provider
//                  that was never wired up). NEVER reported as healthy: a
//                  missing provider is not a working provider (the honesty
//                  rule — e.g. an unattached platform store is
//                  "not_configured", not "ok").
//
// Device health is OWNERSHIP-SCOPED (the Phase 12 model): the report carries
// aggregate counts of the CALLER's own devices — never device ids, never a
// global fleet claim (a service-wide device statement would fabricate a
// view the ownership model deliberately does not give).
//
// The report serializes to the strict platform JSON subset (same writer the
// whole project uses — no new serialization stack).

#include <cstdint>
#include <string>
#include <vector>

namespace vortyx::service {

enum class HealthValue : std::uint8_t {
    Healthy = 0,
    Unhealthy = 1,
    Unknown = 2,
    NotConfigured = 3,
};

const char* to_string(HealthValue value);

struct ComponentHealth {
    std::string component;      // "service", "queue", "scheduler", "platform_store", "devices"
    HealthValue status = HealthValue::Unknown;
    std::string detail;         // human-readable, secret-free
};

struct DeviceHealthSummary {
    std::int64_t total = 0;     // the caller's own devices
    std::int64_t healthy = 0;
    std::int64_t unhealthy = 0;
    std::int64_t offline = 0;
};

struct HealthReport {
    std::int64_t checked_at_ms = 0;
    std::vector<ComponentHealth> components;
    DeviceHealthSummary devices;  // caller-scoped aggregate

    // The overall value: the WORST of the component values with the honest
    // ordering Unhealthy < Unknown/NotConfigured (report worse) < Healthy —
    // i.e. any Unhealthy makes the report Unhealthy; otherwise any
    // non-Healthy component makes it not-Healthy too. Pure function.
    HealthValue overall() const;

    // Strict JSON (platform writer):
    // {"checked_at_ms":N,"overall":"healthy","components":[{"component":"...",
    //   "status":"healthy","detail":"..."},...],
    //  "devices":{"total":N,"healthy":N,"unhealthy":N,"offline":N}}
    std::string serialize() const;
};

}  // namespace vortyx::service
