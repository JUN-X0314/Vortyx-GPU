# The Fabric Planner — Determinism, Cost Model, Replanning (Phase 16)

## The determinism contract

Planning is a PURE function of (WorkloadGraph, ClusterSnapshot,
FabricPlannerConfig). The same inputs produce the same plan CONTENT —
byte-identical, pinned by serialize-equality tests. Concretely:

- **Integer scoring only.** Every score component is int64. No floating
  point (no cross-platform rounding drift), no wall clock (a plan is not
  "older" by time, only by cluster revision), no hash-map iteration order
  (every walk is in registration or planning order).
- **Checked arithmetic.** Weighted terms multiply through
  `checked_scale`; any overflow REFUSES the candidate or the plan with an
  explicit error — a wrapped score would silently invert a preference.
- **Stable tie-breaks.** Higher score wins; exact ties break to the
  SMALLER device id. Device ids are stable identifiers; iteration order
  never is.
- **Config is immutable per run.** Weights live in `FabricPlannerConfig`,
  validated at every use, passed explicitly to every planning call. There
  is no global mutable configuration; two planners with different configs
  cannot contaminate each other.

## The planning pipeline

1. **Validate the graph** (`validate_workload_graph`): descriptors valid,
   dependencies resolvable, acyclic (cycles are NAMED), caps enforced.
2. **Planning order**: Kahn's algorithm over the dependency structure
   with the ready set ordered by (priority DESC, node id ASC). The
   descriptor priority's ONE effect: higher-priority workloads claim
   resources first among structurally ready nodes. Dependencies always
   plan before dependents regardless of priority.
3. **Per node** (in planning order):
   - candidates = `snapshot.candidates_for(owner)` — ownership-scoped,
     state- and health-filtered by Phase 12's own rule;
   - hard filters: capability claim (`device_supports` — unknown
     capability never matches), exclusion constraints;
   - per-shard resource fit against plan-local availability (the bytes
     earlier nodes/shards of THIS plan already claimed on the device are
     subtracted — no double-booking within a plan);
   - scoring of survivors; best candidate wins by the documented rule.
4. **All-or-nothing**: any unplannable node refuses the WHOLE plan with a
   structured rejection (stage code + node + message). A plan that
   cannot execute is never published as if it could.

## The cost model (a heuristic over real metadata — nothing more)

```
total = base
      - (available_memory - required_memory) * slack_penalty_weight
      - running_shards                        * queue_penalty_weight
      + locality_bonus_weight   (the descriptor's locality hint matches)
      + backend_bonus_weight    (the device's backend preference aligns)
```

- `slack` — the TIGHTEST fit wins (the smallest-slack rule Phase 12's
  CapabilityFit uses, generalized).
- `queue` — the device's CURRENT running-shard count from the snapshot:
  an observed state, not a load prediction.
- `locality` — the caller said data is already resident on that device.
  This is a metadata preference. There is NO transfer engine: the bonus
  expresses "avoid a transfer we cannot do", and a plan needing a
  transfer to an unhinted device is not faked into feasibility.
- `backend` — the device's own claimed preference matches the request.

Every component is recorded per decision (`ScoreBreakdown`) and sums to
the total — the web/API explanation is derived from these recorded facts.
The weights are PLANNER DECISION VALUES, documented as such; they are not
measurements and never appear as performance numbers.

## Rejection stages (stable codes)

`invalid_request`, `cluster_empty`, `unsupported_capability`,
`backend_unavailable`, `excluded_constraint`, `insufficient_resource`,
`device_unhealthy`. Per node, the plan records a bounded list
(`kMaxRecordedRejections`, walked in registration order) plus honest
counts of everything beyond the bound. The policy bridge maps these onto
Phase 12's `PlacementRejection` codes for the orchestrator's reporting.

## Plan identity, staleness and lineage

- The pure planner sets `plan_version = 0` (unassigned). Versioning is a
  publication concern: `PlanLineage::record` stamps 1 for the initial
  publication, +1 for each content CHANGED republication. Identical
  content keeps the current version — a version never names two different
  assignment sets, and identical content never manufactures a fake new
  version.
- The history is a bounded ring (`kMaxEntries = 8`): the current version
  is always retained; the oldest entries fall off.
- **Stale rule (pure)**: `plan_is_stale(plan, snapshot)` ⇔
  `plan.cluster_revision != snapshot.revision`. Stale plans are
  re-planned, never force-executed — the orchestrator's own revision
  check enforces the same rule against the LIVE registry.
- **Replanning** (`FabricReplanner::replan`): refuses a same-revision
  pair (a replan without a cluster change is a caller bug); re-plans
  unfinished work against the FRESH snapshot; preserves SUCCEEDED shards
  VERBATIM (same device, same range — checkpoints; Phase 12's checkpoint
  semantics guarantee they never re-execute); stamps the version
  previous + 1. Terminal jobs are never reopened — replanning consumes
  outcomes, it never mutates job or shard state.

## Priority semantics (minimal, documented)

Priority is a planning-order preference and a scoring tie-break input.
It is NOT a fairness or starvation-freedom claim, and no behavior beyond
planning order depends on it. (The `JobEnvelope`'s previously reserved
`priority` field flows through verbatim; Phase 16 is the anticipated
"later phase" that gave it semantics — see `src/platform/job.hpp`.)
