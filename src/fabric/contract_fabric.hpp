#pragma once

// Fabric contract serialization (Phase 16) — the API-facing plan summary.
//
// The FULL ComputePlan stays in-process (plan.hpp documents why); what an
// API serves is a bounded, human-readable SUMMARY of the latest plan:
// version, planner identity, the devices the plan selected (deterministic
// order), and the structured reason text. Metadata only — no payload, no
// secret, no telemetry (the summary is derived from the plan, which
// already guarantees that by construction).

#include <cstdint>
#include <string>
#include <vector>

#include "fabric/plan.hpp"

namespace vortyx::fabric {

// The API-facing plan summary (the service copies this into its job view).
struct PlanSummary {
    std::uint32_t plan_version = 0;
    std::string planner_name;
    std::string planner_version;
    std::uint64_t cluster_revision = 0;
    // The devices the plan assigned work to, in the plan's deterministic
    // node order (deduplicated; first-assignment order).
    std::vector<std::string> devices;
    // The structured reason summary (human-readable; every fact in it is
    // derived from the plan's own recorded decision — nothing invented).
    std::string reason_summary;
};

// Builds the summary from a plan (pure; deterministic — the devices come
// from the plan's node/shard order, the reason text from the recorded
// scores and rejections).
PlanSummary summarize_plan(const ComputePlan& plan);

// {"plan_version":N,"planner":"...","planner_version":"...",
//  "cluster_revision":N,"devices":["..."],"reason":"..."}
// Deterministic strict JSON (the platform writer, canonical field order).
std::string serialize_plan_summary(const PlanSummary& summary);

}  // namespace vortyx::fabric
