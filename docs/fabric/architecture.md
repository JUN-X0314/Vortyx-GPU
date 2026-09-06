# The Adaptive Compute Fabric — Architecture (Phase 16)

## What the fabric IS

`vortyx::fabric` (`src/fabric/`) is Vortyx's adaptive planning layer: it
analyzes workloads, decides placement deterministically, explains every
decision, and hands its plans to the EXISTING execution path. It was
designed — after reading the real code — as a layer ON TOP of the Phase 12
distributed system, not a replacement for any part of it.

## Layer position (enforced by the build graph)

```
service (Phase 14)   worker (Phase 15, beside)
    ↓
tensor (Phase 13)  ── fabric_adapter (Phase 16: TensorGraph → WorkloadGraph)
    ↓
fabric (Phase 16)    ← the planning layer; plans only, executes nothing
    ↓
distributed (Phase 12)  ← the execution world: registry, leases, workers
    ↓
platform (Phase 11) → core
```

- `vortyx_fabric` links `vortyx_distributed` PUBLIC: it reads cluster
  snapshots and reuses Phase 12's rule functions (`candidates_for`,
  `device_supports`, `shard_memory_bytes`, `partition_element_count`) —
  one definition of every rule, never a second copy.
- The build option `VORTYX_ENABLE_FABRIC` (default ON) auto-disables when
  `VORTYX_ENABLE_DISTRIBUTED=OFF`. `FABRIC=OFF` disables the tensor layer
  with it (the adapter builds on the fabric) and reproduces the Phase 15
  build exactly.
- The fabric never sees the service, web or worker layers.

## Components

| File | Component | Role |
|------|-----------|------|
| `workload.*` | `WorkloadDescriptor` | The workload's metadata and intent. Derived deterministically from a `JobEnvelope`. |
| `graph.*` | `WorkloadGraph` | A dependency graph of workloads (insertion-order ids, smallest-ready-id topo order, cycle detection, caps). |
| `cost.*` | `FabricPlannerConfig` + scoring | The deterministic heuristic: named int64 score components, checked arithmetic, immutable per-run weights. |
| `plan.*` | `ComputePlan` | The plan artifact: version, cluster revision, assignments, structured decisions; the pure stale rule; deterministic JSON. |
| `planner.*` | `FabricPlanner` | The planning core (pure): validate → order → per-node filter/score/pick → all-or-nothing plan. |
| `replan.*` | `PlanLineage`, `FabricReplanner` | Bounded plan history; the safe replanning rule (checkpoints preserved, versions monotonic). |
| `policy_bridge.*` | `FabricPolicy` | Implements Phase 12's `ISchedulingPolicy`; carries fabric plans into the orchestrator's existing seam. |
| `contract_fabric.*` | `PlanSummary` | The API-facing bounded plan summary (metadata only). |

## The integration mechanism (and why)

Phase 12's orchestrator has exactly one placement seam:
`ISchedulingPolicy::plan(request, snapshot)`. `FabricPolicy` implements it.
`DistributedOrchestrator::Deps` gained an OPTIONAL `policy_override`
(additive; default null → the config-name policy exactly as before). When
the override is set:

- every placement decision comes from the deterministic planner;
- the orchestrator's stale-plan check, atomic leases, retry waves,
  checkpoint semantics, cancellation and terminal transitions are
  UNTOUCHED (no Phase 12 code path changed behavior);
- each placement event is recorded in the per-job `PlanLineage` with a
  monotonic version.

## What the fabric deliberately does NOT do

- No execution: it never dispatches, never takes leases, never reserves.
  A planner that mutated the cluster would be a second scheduler.
- No second vocabulary: operations are `ComputeOp`; capabilities are
  Phase 12's; partitioning is Phase 12's; memory estimation is Phase 12's.
- No fabricated telemetry: a measured duration exists only when a caller
  measured one; utilization/throughput/temperature do not exist here.
- No ML, no measured-performance claims: the cost model is a documented
  heuristic over metadata (see planning.md).
- No persistence: plan metadata is runtime memory representation. There
  is NO migration and NO schema change in Phase 16; an API exposure
  serves a bounded summary (`PlanSummary`), never the full plan, and the
  web console renders "not available" when the API provides nothing.

## Honest scope

The fabric is a planning and orchestration research layer. There is no
work stealing, no priority fairness guarantee, no cross-device transfer
engine (a plan needing one is refused), and no network path beyond
Phase 12's documented loopback. Each absence is an extension point, not a
TODO disguised as done.
