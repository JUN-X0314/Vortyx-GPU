# Distributed / Multi-GPU Device System — Architecture (Phase 12, v0.12.0)

## What Phase 12 is

Phase 12 turns Vortyx from a single-device engine into a system that can
manage **several logical Vortyx devices as one compute cluster**: devices
are registered in a registry, a scheduler places the shards of one logical
job onto capable devices, workers execute those shards **through the
unchanged local compute path**, failures are retried under an explicit
policy, and shard results are reassembled into one deterministic logical
result.

## What Phase 12 is NOT (stated plainly)

- **No real network transport.** The transport is an interface
  (`IWorkerTransport`); the only implementation is the in-process
  loopback (`LocalInProcessTransport`). No HTTP, gRPC, WebSocket or
  serialization exists anywhere in the layer.
- **No production cloud cluster.** No Kubernetes, no consensus, no
  service discovery. The Supabase/Vercel control plane records metadata
  (see `docs/platform/`), it does not execute anything.
- **No performance-aware scheduling.** All three policies are
  capacity/capability-aware and deterministic; none of them measures
  speed, and no document claims otherwise.
- **No hardware discovery.** Device capacities are self-reported
  configuration. The local simulator's backends are the one honest
  exception: they are queried from a real `Runtime` on the host.

## Layering (additive — nothing below the platform boundary changed)

```
Application / Client
        ↓
Platform / Cloud Control Plane      (vortyx::platform, Phase 11 — unchanged)
        ↓
Distributed Orchestrator            (vortyx::distributed, Phase 12)
        ├─ DeviceRegistry           registration · leases · snapshots
        ├─ Scheduling Policies      round_robin · least_loaded · capability_fit
        ├─ Job Sharding             deterministic element-range partitions
        ├─ Workers                  LocalWorker → existing Compute Runtime
        └─ Transport                IWorkerTransport (loopback implementation)
        ↓
Virtual GPU / Compute Runtime       (Phases 3–10 — unchanged)
```

Build graph (the layering is enforced, not conventional):
`vortyx_distributed → vortyx_platform → vortyx_core`. The core never
includes either upper layer. `VORTYX_ENABLE_PLATFORM=OFF` disables the
distributed layer automatically and reproduces the Phase 10 build exactly;
`VORTYX_ENABLE_DISTRIBUTED=OFF` disables just this layer.

## Components and responsibilities

| Component | Files | Responsibility |
|---|---|---|
| Clock | `clock.hpp/.cpp` | Injectable monotonic time (`IClock`, `SteadyClock`, `FakeClock`). Every timeout in the layer reads this clock — no component calls `std::chrono` directly, and no test sleeps. |
| Resource model | `resource.hpp/.cpp` | `ResourceVector` (compute units, memory bytes, concurrent jobs) with checked fit/add/sub invariants: nothing negative, `allocated ≤ capacity`, releases never go below zero. |
| Device model | `device.hpp/.cpp` | Device state machine (`registering → ready → busy/draining/offline/failed`), health classification, capability claims validated against the Phase 11 vocabulary. Reuses `platform::DeviceId/UserId/DeviceMetadata` — no second identity system. |
| Registry | `registry.hpp/.cpp` | `IDeviceRegistry` + `LocalDeviceRegistry`: idempotent registration, ownership-scoped visibility, transition-table-enforced state updates, atomic lease reservation/release, lazy lease expiry, monotonically increasing cluster revision, immutable snapshots. |
| Leases | `lease.hpp/.cpp` | `DeviceLease` + RAII `LeaseGuard`: a placement becomes real by reserving capacity; error paths return capacity structurally. |
| Snapshots | `cluster.hpp/.cpp` | `ClusterSnapshot`: the immutable, ownership-filtered planning input. Schedulers never read the live registry. |
| Topology | `topology.hpp` | The seam for device-to-device links. Phase 12 ships a static provider; absence of data is expressed as "no links", never as fabricated bandwidth/latency. |
| Sharding | `shard.hpp/.cpp` | `WorkPartition` (ElementRange kind) + `partition_element_count`: contiguous, non-overlapping, exactly-covering ranges; no empty shards (`K > N` yields `N` single-element shards); deterministic shard ids `<job_id>-s<index>`. |
| Job state | `job.hpp/.cpp` | Distributed job lifecycle + `derive_job_status` (the job status is COMPUTED from shard states) + the mapping onto the Phase 11 `JobStatus`. |
| Retry | `retry.hpp/.cpp` | Stable `FailureCode` vocabulary, the documented retryable/non-retryable classifier, `RetryPolicy` with a bounded attempt ceiling and a pure exponential backoff function. |
| Policies | `policy.hpp/.cpp` | `ISchedulingPolicy` over `(PlacementRequest, ClusterSnapshot)` — pure placement. Rejection codes: `invalid_request`, `cluster_empty`, `unsupported_capability`, `no_device_available`, `insufficient_resource`, `device_unhealthy`, `stale_plan`. |
| Workers | `worker.hpp/.cpp` | `IWorker` + `LocalWorker`: validates the assignment, slices the local `ComputeTask` to the shard range, executes through its own exclusive `Runtime` (mutex-serialized — the Phase 4 single-thread contract), reports `ShardResult`. |
| Transport | `transport.hpp/.cpp` | `IWorkerTransport` + `LocalInProcessTransport` (loopback) with deterministic failure injection for tests. |
| Aggregation | `aggregator.hpp/.cpp` | `ResultAggregator`: first-verdict-wins per shard (duplicate-safe), deterministic shard-order reassembly, honest succeeded/failed/cancelled accounting. |
| Heartbeat | `heartbeat.hpp/.cpp` | `HeartbeatMonitor`: turns elapsed time into an explicit health judgment (Unhealthy + Offline) on the injected clock; recovery is a heartbeat. |
| Orchestrator | `orchestrator.hpp/.cpp` | The facade that runs the whole flow and the only component that sees it end to end. |
| Config | `config.hpp/.cpp` | `VORTYX_DISTRIBUTED_*` environment parsing with explicit rejection of invalid values (absent → documented defaults). |
| Simulator | `simulator.hpp/.cpp` | `LocalMultiDeviceSimulator`: N virtual devices, each with its own `Runtime`; backend claims are the runtime's honest answer. |
| Contract | `contract_distributed.hpp/.cpp` | The C++ wire codec for the distributed endpoints (same JSON module, error codes and status mapping as Phase 11). |

## The end-to-end flow

```
submit(auth, request)
  → validate (Phase 11 envelope rules + local payload consistency)
  → [platform store: create_job]                    (optional integration)
  → Planning    (placement plan from a snapshot; leases reserved atomically)
  → Scheduled   (shards recorded with their ranges and devices)
  → Running     (shards dispatched through the transport to workers)
  → retry waves (failed shards re-placed — excluding the failed device —
                 while succeeded shards are never re-run)
  → terminal    (Completed | Failed | Cancelled; partial failure is Failed)
  → [platform store: put_result]                    (metadata only)
```

## Security boundary (unchanged from Phase 11)

- Every job carries the submitter's identity; `job`/`cancel` queries
  apply `is_owner`, and foreign jobs are **NotFound** (anti-enumeration).
- Cluster snapshots and listings are ownership-filtered: a scheduler
  literally cannot see another user's devices.
- The optional platform integration speaks only `IPlatformStore` using the
  submitter's own `AuthContext` — no privileged path, no SDK, no SQL.
- No secrets, no tokens, no hardware identifiers exist in the layer.

## Documentation map

- `device-model.md` — the device state machine, health, capabilities.
- `scheduling.md` — sharding invariants, policies, placement, stale plans.
- `failure-handling.md` — failure codes, retry semantics, leases, heartbeats.
- `api.md` — the distributed wire contract (C++ and TypeScript).
- `local-development.md` — the simulator, the diagnostic tool, config.
