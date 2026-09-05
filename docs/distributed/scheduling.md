# Scheduling: Sharding, Policies, Placement (Phase 12)

## The separation that matters

- The **policy** answers "which device for this shard?" as a pure function
  of `(PlacementRequest, ClusterSnapshot)`. No locks, no registry access,
  no clock, no I/O.
- The **orchestrator** owns everything temporal: taking the snapshot,
  checking plan freshness against the live registry revision, reserving
  leases atomically, executing, retrying.

## Job sharding — the partition contract

One logical job is split into shards. A shard's work description is a
`WorkPartition`; Phase 12 implements exactly one kind, `ElementRange` — a
contiguous `[begin, end)` slice of the data-parallel domain
`[0, element_count)` documented on `ComputeTask` since Phase 10. The kind
tag exists so a future operation with a different partition shape adds its
own kind instead of overloading the range.

`partition_element_count(N, K)` guarantees (property-tested):

1. **Exact coverage** — every element belongs to exactly one shard; the
   union of the ranges is `[0, N)`.
2. **No overlap, no gaps** — ranges are contiguous and ascending.
3. **No empty shards** — `K > N` yields `N` single-element shards, never
   padded empties.
4. **Balance** — shard sizes differ by at most one element.
5. **Determinism** — the same `(N, K)` always produces the same ranges.

Shard ids are deterministic: `<job_id>-s<index>`. A `job_id` so long that
its derived shard ids would exceed the platform id cap is **refused
explicitly** at submission — never silently truncated.

Elementwise slices compose: slice → execute → reassemble is **bit-exact**
against the full-range execution (integer semantics, range-independent —
pinned by tests for all three operations).

## Placement request / decision

```
PlacementRequest { job_id, owner, operation, requested_backend,
                   element_count, requested_shard_count,
                   allow_fallback, excluded_devices }
ClusterSnapshot  { revision, devices[+capacity/allocated/state/health] }
        ↓  ISchedulingPolicy::plan
PlacementPlan   { accepted?, rejection?, cluster_revision,
                  shards[{index, range, device_id, resources}] }
```

- The snapshot's candidate filter applies, in order: **ownership** (only
  the requester's devices are even visible), **state** (ready/busy),
  **health** (healthy — `unknown` is not healthy), **capability claims**
  (the operation label and, when requested, the backend), then the
  request's **exclusions** (a retry never re-targets the device it just
  failed on while another device exists).
- A plan records the **cluster revision** it was computed from.

## Rejection codes (stable, lowercase like the Phase 11 contract)

| Code | Meaning |
|---|---|
| `invalid_request` | zero elements / zero shards / non-derivable shard ids |
| `cluster_empty` | the owner has no devices registered at all |
| `device_unhealthy` | devices exist but none is schedulable (offline/draining/failed) |
| `unsupported_capability` | schedulable devices exist but none claims the operation/backend |
| `no_device_available` | capable devices exist but fewer than requested with fallback off — or all capable devices are excluded |
| `insufficient_resource` | a shard does not fit any capable device's remaining capacity |
| `stale_plan` | (set by the orchestrator) the cluster changed between plan and execution and re-planning never settled |

A rejected placement is a **terminal, honest failure** of the job — the
stable code and human reason are recorded on the job record. Nothing ever
falls back silently.

## The three policies (all deterministic)

- **`round_robin`** — rotates through the capacity-fitting candidates in
  registration order (a per-policy cursor; the same request sequence over
  the same cluster rotates identically). Prefers devices not already taken
  by the plan.
- **`least_loaded`** — ranks by fewest allocated concurrent jobs, then
  fewest allocated memory, then registration order. This is a
  **capacity-aware** policy based on self-reported allocations; it is not
  a measured-performance policy and makes no speed claims.
- **`capability_fit`** — best fit: the candidate whose remaining memory
  most tightly covers the shard's need (smallest slack), tie-break
  registration order.

Unknown policy names are refused at orchestrator creation — never
silently defaulted (`make_scheduling_policy` returns null; `create` fails).

## Fallback semantics

- `requested_shard_count > capable devices` with `allow_fallback=true`
  **coalesces** to one shard per device (never more shards than devices,
  never empty shards).
- With `allow_fallback=false` the same request is rejected
  `no_device_available` — the multi-device requirement is never silently
  dropped.
- A single-device cluster with a multi-device request and fallback on
  simply executes single-device (that is the policy).

## Cluster revision and stale plans

Every registry mutation bumps the revision. The orchestrator compares a
plan's recorded revision against the registry's current revision **right
before reserving**; a mismatch (or a failed reservation — capacity lost
under us) triggers a bounded re-plan (`kMaxPlanAttempts = 8`). A cluster
that never settles fails the job with the honest instability reason.
Stale plans are **never force-executed**.

## Topology seam

`TopologyView`/`TopologyLink` express device-to-device relationships
(`local`, `shared_memory`, `pcie`, `network`, `unknown`) with optional
bandwidth/latency metadata that defaults to "not reported". Phase 12 ships
a static provider; nothing treats "no topology data" as "optimal
interconnect", and no hardware discovery is pretended.
