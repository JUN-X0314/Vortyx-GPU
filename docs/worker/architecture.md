# The Native Execution Boundary — Architecture

Phase 15 separates the Vortyx Platform into two planes:

```
CONTROL PLANE (TypeScript, Vercel or self-hosted Node)
  web console ── API ── stores (memory | Supabase/PostgreSQL)
                        projects · members · service_jobs · quota ·
                        artifacts · audit · rate-limit
                              │
                              │  the WORKER PROTOCOL (JSON over HTTP,
                              │  metadata only — never payloads)
                              ▼
EXECUTION PLANE (C++, a separate process)
  vortyx_worker_agent ── WorkerAgent loop
                        └─ INativeExecutor → SimulatorNativeExecutor
                           └─ the UNCHANGED Phase 12 stack
                              (registry → placement → leases → workers →
                               Runtime → aggregation)
```

## Why the boundary exists

The control plane owns the job lifecycle (state machine, quota, audit). It
must be able to run on serverless infrastructure that cannot host
long-running native processes. Execution needs the real Vortyx engine.
Gluing them with a fake (an API endpoint that marks a job "completed"
without execution) is forbidden by the project's honesty rules — so
Phase 15 defines an explicit protocol and a real agent instead:

- the control plane exposes worker endpoints (`/api/worker/…`) guarded by a
  worker TOKEN (a server-side shared secret, never a user identity);
- the native agent polls `claim`, executes what it claims on the local
  Phase 12 stack, and reports the terminal outcome;
- **a job is `completed` only if a worker really executed it.** With no
  worker connected, jobs stay `queued` — the UI shows exactly that.

## Components (src/worker/)

| Component | Responsibility |
|---|---|
| `native_executor.hpp/.cpp` | `INativeExecutor` — the execution seam. `SimulatorNativeExecutor` builds a local simulated cluster (self-reported capacities, one Runtime per virtual device) and drives the UNCHANGED Phase 12 orchestrator. `synthesize_task` deterministically derives the payload from `(job_id, operation, element_count)` — the documented consequence of the metadata-only control plane: the same job id always produces the same inputs; the VectorScale scalar is part of that synthesized payload, never a control-plane field. |
| `worker_protocol.hpp/.cpp` | The wire types + strict JSON codec (claim/heartbeat/complete), reusing the Phase 11 platform JSON. Unknown fields, wrong types, missing required values are refusals — never guessed around. |
| `worker_transport.hpp` + `http_transport.hpp/.cpp` | `IWorkerApiTransport` and its real HTTP/1.1 client (POSIX sockets / Winsock). **Plain `http` only**: TLS is deliberately not implemented in the C++ core (the dependency policy keeps it standard-library only); terminate TLS at a reverse proxy or keep the agent on a trusted network segment. An `https://` endpoint is refused at configuration time. Connect/read timeouts, a response-size cap, and chunked-response decoding are built in. |
| `worker_agent.hpp/.cpp` | One cycle: `claim` → spawn the heartbeat thread (first beat fires immediately) → `execute` on the real stack → stop heartbeating → report the terminal record. The heartbeat renews the lease and relays the control plane's `cancel_requested` into the executing record via `INativeExecutor::request_cancel` (the orchestrator's own cancel flag — the same mechanism a local owner cancel uses). |
| `worker_main.cpp` | The `vortyx_worker_agent` executable: env-driven configuration, a health probe on start, honest per-cycle logging, graceful SIGINT shutdown. |

## Job state mapping

```
control plane (service_jobs.status)      worker/orchestrator view
─────────────────────────────────────    ─────────────────────────
queued                                   claimable
running (claimed_by, claim_expires_at)   executing (shards placed/running)
running + cancel_requested               the orchestrator cancels at the
                                         next wave boundary → cancelled
completed | failed | cancelled           terminal, reported by the agent
running + expired lease                  reconciliation marks it
                                         failed("worker_lease_expired")
```

## Guarantees and their limits (read this before relying on anything)

- **At-least-once attempts, idempotent commit.** The control plane cannot
  prove exactly-once execution across process crashes (the worker executes
  outside any database transaction). What IS guaranteed: a claim is atomic
  (`FOR UPDATE SKIP LOCKED` in PostgreSQL; a single mutex-guarded section in
  memory) so two workers can never hold one job; a worker's result commit is
  idempotent — a duplicate report returns the existing terminal state and
  never creates a duplicate result; a report from a worker that does not
  hold the claim is refused. This is the documented at-least-once +
  idempotent-commit contract.
- **Recovery, not auto-retry.** A stale lease (expired `claim_expires_at_ms`)
  is reconciled to `failed("worker_lease_expired")`. Nothing re-executes the
  job automatically; a retry is an explicit resubmission (new job id, new
  attempt) — automatic re-execution would need side-effect-free payloads,
  which the metadata-only protocol does not claim.
- **No payloads, ever.** Every protocol message is metadata. The worker
  synthesizes its inputs; the control plane stores result METADATA
  (element count, backend, shard counts).
- **No fake capacity.** The simulated devices' capacities are configuration,
  the backends list is the honest answer of a real Runtime on the host. When
  the host has no Vulkan device, results report `cpu`.
