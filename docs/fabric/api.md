# The Fabric — API Surface and Contract (Phase 16)

## C++ entry points

All types live in `namespace vortyx::fabric` (umbrella header
`src/fabric/fabric.hpp`).

```cpp
// Build a planner (immutable per-run config; validated at use).
FabricPlannerConfig config;             // weights; validate() documents bounds
FabricPlanner planner(config);

// Plan a workload graph against a snapshot (PURE; version left unassigned).
WorkloadGraph graph;                    // nodes + dependencies (graph.hpp)
ClusterSnapshot snapshot = registry.snapshot();
ComputePlan plan;
std::string error;
if (planner.plan_graph(graph, "graph-id", snapshot, plan, error)
        != vortyx::platform::Status::Ok) {
    // plan.rejection carries {code, node, message}; plan.nodes is empty.
}

// Drive the EXISTING orchestrator through the policy seam:
DistributedOrchestrator::Deps deps;     // registry / transport / clock as before
deps.policy_override = make_fabric_policy(config);   // NEW optional seam
DistributedOrchestrator::create(deps, dist_config, orchestrator, error);
// ... every submit/cancel/job call is UNCHANGED from Phase 12/15.

// Inspect the plan history for a job (the policy owns it).
const FabricPolicy* policy = /* the same instance handed to deps */;
const PlanLineage* lineage = policy->lineage_for(job_id);
const ComputePlan* current = policy->last_plan_for(job_id);
FabricPolicyCounters counters = policy->counters();  // real counters only

// Serialize deterministically (no deserializer exists on purpose).
std::string json = serialize_compute_plan(plan);
```

## The service integration (opt-in)

```cpp
PlatformServiceConfig config;           // Phase 14 service
config.fabric_planning = true;          // DEFAULT OFF — Phase 15 behavior
config.fabric_planner  = config_value;  // validated at create()
```

When enabled, every DISPATCHED job's view carries plan metadata:

| Field | Meaning | When absent |
|-------|---------|-------------|
| `plan_available` | the job was fabric-planned | false (never planned: fabric off, cancelled-in-queue, or refused placement) |
| `plan.plan_version` | publication version (monotonic) | — |
| `plan.planner` / `planner_version` | planner identity | — |
| `plan.devices` | devices the plan assigned work to (deduplicated, plan order) | — |
| `plan.reason` | the structured reason summary, derived from the plan's recorded decisions | — |

Cancelled-in-queue jobs are never planned: `plan_available` stays false —
an honest absence, never a fabricated plan.

## The contract JSON (C++ service contract)

`serialize_service_job` appends ONE additive nullable field:

```json
"plan": null
"plan": {
  "plan_version": 1,
  "planner": "adaptive_fabric",
  "planner_version": "0.16.0",
  "cluster_revision": 7,
  "devices": ["dev-b"],
  "reason": "accepted: planned 1 workload; w1 -> dev-b (1 candidate(s) rejected)"
}
```

Field order is canonical (the strict Phase 11 JSON writer); the same plan
always serializes to the same bytes. The TypeScript control plane
(`platform/api`) does NOT include the fabric — its job payloads are
unchanged, and the web console renders their absent plan as the explicit
"not available" state.

## Web console

The job detail view (`platform/web/js/views/job-view.js`) renders the plan
section from `job.plan` ONLY:

- present → plan version, planner, planner version, devices, reason —
  every value the API actually sent (all strings via `textContent`; no
  HTML injection);
- null/absent → the explicit "Not available" note (planning is opt-in on
  the control plane). The UI never invents a version, a device or a
  reason.

The row logic is the pure `planSummaryRows(plan)` — pinned by
`platform/web/test/web-logic.test.mjs`.

## What this API surface deliberately does NOT include

- No plan persistence endpoint, no migration: plan metadata is runtime
  memory; nothing in Phase 16 touches the database schema.
- No worker-protocol change: workers never see plans (control plane owns
  planning policy; workers execute and report).
- No full-plan wire exposure: the API serves the bounded summary only.
- No new rate-limit subsystem, no artifact changes, no new authentication
  path. The plan is project/job metadata and inherits the EXACT existing
  authorization boundary (a foreign user's job view remains NotFound —
  anti-enumeration, unchanged).
