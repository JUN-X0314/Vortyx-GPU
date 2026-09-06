# Service Layer Architecture (Phase 14)

## What Phase 14 is

Phase 14 turns Vortyx from a compute stack into a **serviceizable platform
foundation**: a real, tested control plane that wraps the existing layers —
projects, authorization, quota, rate limiting, a job queue, audit, metrics,
health and a machine-readable contract — with the full submission flow
ending in the **unchanged Phase 12 orchestrator**.

"Production-oriented" is the honest description: the architecture and the
control plane are real and locally verified end to end; no claim is made
about a deployed commercial cloud. Everything below distinguishes what is
implemented from what is a documented extension point.

## Where the layer sits

```
Application / Client
     ↓
Service Layer                (vortyx::service — Phase 14, src/service/)
     │   Projects / Memberships / Authorization (pure table)
     │   Quota (project policy ledger)     Rate limiting (fixed window)
     │   Queue (provider-neutral FIFO)     Job Service (the full flow)
     │   Audit (bounded events)            Metrics (real counters only)
     │   Health (honest per-component)     Artifacts (metadata only)
     ↓
Platform Control Plane       (vortyx::platform — Phase 11, unchanged)
     ↓
Distributed Orchestrator     (vortyx::distributed — Phase 12, unchanged)
     ↓
Tensor Layer                 (vortyx::tensor — Phase 13, unchanged)
     ↓
Core Engine                  (vortyx::compute / resource — unchanged)
```

Dependency rule (enforced by the build graph): `vortyx_service` links
`vortyx_tensor` (which brings distributed → platform → core). Nothing below
the service boundary includes anything above it. No circular dependency
exists. With `VORTYX_ENABLE_SERVICE=OFF` — or `TENSOR=OFF` /
`PLATFORM=OFF`, which disable the whole upper stack — the build is exactly
the previous phase's.

## Identity reuse (no second ID scheme)

| Concept    | Type                              | Source     |
|------------|-----------------------------------|------------|
| User       | `vortyx::platform::UserId`        | Phase 11   |
| Job        | `vortyx::platform::JobId`         | Phase 11   |
| Device     | `vortyx::platform::DeviceId`      | Phase 11   |
| Project    | `vortyx::service::ProjectId`      | Phase 14 (same syntax rules, same UUID-v4 shape) |
| Artifact   | `vortyx::service::ArtifactId`     | Phase 14 (same UUID-v4 shape) |

## Components

| Component | Files | Responsibility |
|-----------|-------|----------------|
| `ServiceStatus` | `service_status.*` | The service result vocabulary (stable snake_case codes, HTTP mapping). Separate from `platform::Status` by the same per-layer-vocabulary rule every layer follows. |
| `authz` | `authz.*` | The ONE pure (role, action) → allowed table. The project store, the job service and the tests all consult it — the layers cannot drift. |
| `project` | `project.*` | `IProjectStore` (provider-neutral) + `InMemoryProjectStore` (local/mock reference). Projects, memberships, roles, archival. Authorization is checked inside every method (the `IPlatformStore` pattern). |
| `quota` | `quota.*` | `QuotaEngine`: per-project policy limits with a job-keyed reservation LEDGER. Exactly-once release; replay without double charge; conflict on different dimensions. |
| `ratelimit` | `ratelimit.*` | Deterministic fixed-window limiter over the injected `IClock`. Refused attempts count. In-memory; a distributed limiter is a future provider, not a claim. |
| `queue` | `queue.*` | `IJobQueue` + `InMemoryJobQueue` (bounded FIFO). Idempotent enqueue; exactly-once removal (the cancel-in-queue path). |
| `audit` | `audit.*` | `AuditTrail` + bounded `InMemoryAuditStore`. Events carry who/what/when/outcome/reason-code — and no field a secret could ride in. |
| `metrics` | `metrics.*` | Real counters only (submissions, terminal outcomes, policy refusals, gauges). Nothing measures latency; nothing is estimated. |
| `health` | `health.*` | Per-component health with `not_configured` for unattached providers (never "healthy"). Device health is caller-scoped aggregates (the Phase 12 ownership rule). |
| `artifact` | `artifact.*` | Artifact METADATA registry. No payload storage exists — nothing is uploaded, nothing is faked. |
| `platform_service` | `platform_service.*` | The facade: the submission flow, dispatcher threads, cancellation, queries, observability wiring. |
| `contract_service` | `contract_service.*` | JSON views (projects, jobs, errors, metrics) in the strict platform JSON subset. |

## The submission flow

```
submit_job(auth, request)
  1. Authentication        platform::AuthContext (transport-verified identity)
  2. Request validation    Phase 11 envelope rules + service shard cap
                           + compute-task consistency (op/element count)
  3. Project validation    IProjectStore (visibility = owner or member)
  4. Authorization         authz table: SubmitJob requires Member+
                           archived project → unsupported_operation
  5. Rate limiting         per-user fixed window (replays bypass it)
  6. Quota reservation     project policy ledger (jobs/shards/memory)
  7. Record + queue        one atomic section; FIFO, idempotent
  8. [dispatcher]          the UNCHANGED Phase 12 orchestrator, under the
                           SUBMITTER's identity (no privileged path)
  9. Terminal finalize     quota release (exactly once), metrics, audit
```

Steps 5–8 run as ONE atomic section under the service lock: the
idempotency decision, the rate-limit check, the quota reservation and the
record/queue insertion cannot interleave with a concurrent submission of
the same id. The orchestrator is never called while holding the service
lock.

Every refusal in the flow is a distinct `ServiceStatus` with a stable
code; every accepted submission and every terminal outcome is audited.

## Threading and lock order

- The service runs `dispatcher_count` (1..8, default 2) dispatcher threads
  draining the queue into the orchestrator.
- Lock order (documented, enforced by construction):
  `state_` (service records) → (rate limiter | quota | queue | audit)
  internal locks, one at a time, never inverted.
- The orchestrator, registry and transport are NEVER touched while holding
  `state_` (the Phase 12 rule: no service lock spans a dispatch).
- Condition-variable notifications are issued UNDER the service lock
  (a notify after release can slip between a waiter's predicate check and
  its wait — a lost wakeup; observed under ASan's stretched timings and
  fixed by construction).

## What Phase 14 is NOT (documented non-goals)

- No HTTP server exists in the C++ core. The service contract is a
  C++-level, machine-readable surface; the Vercel/TypeScript API layer
  remains the Phase 11/12 deployment boundary.
- No Supabase/Redis/PostgreSQL/cloud-queue adapters are implemented. The
  provider-neutral interfaces (`IProjectStore`, `IJobQueue`,
  `IAuditStore`, `IArtifactStore`) are the extension points; the in-memory
  implementations are the local/mock references, clearly labeled.
- No billing, no GPU marketplace, no multi-region clusters, no real
  network transport. Each absence is an explicit extension point for a
  future phase, never a TODO disguised as done.

---

## Phase 15.0.1 — production stabilization (what changed and why)

15.0.1 adds no feature. It hardens the Phase 15 control plane against the
concurrency that real operation implies: many simultaneous submissions, many
workers, duplicate deliveries, and direct database access by authenticated
clients.

- **Project-level quota serialization (database)**. The Supabase quota
  trigger (`vortyx_enforce_service_quota`, migration 0003) read the policy
  and the in-flight usage with plain MVCC SELECTs. Two simultaneous
  submissions to one project could both read usage = N, both pass, and both
  insert. Migration 0004 makes the trigger take `SELECT … FOR UPDATE` on the
  project's own row FIRST and re-read usage after acquiring it — a
  serialization point that is exact under any number of API instances,
  because it lives in the row lock, not in any process. Quota semantics are
  untouched: the same three policy fields, the same `queued`/`running`
  usage ledger, the same refusal codes, the same defaults.
- **Artifact-capacity serialization (database)**. The 256-per-project bound
  had the identical count-then-insert race; the capacity trigger now uses
  the SAME project-row lock. One lock key and one lock order for all
  project-level mutations — `projects` row first, then dependent reads —
  mean the quota path and the artifact path can never interleave or
  deadlock (neither ever holds a second lock the other needs first).
- **Idempotency under concurrency (API adapter)**. The TypeScript Supabase
  adapter's pre-check/insert pair handles the concurrent-duplicate case by
  rule, not by luck: a 23505 on the insert re-reads the committed winner
  and applies the same replay/conflict rule as the pre-check (same payload
  = `created: false` replay; different payload/owner/project = conflict).
  The job id's primary key guarantees exactly one logical reservation in
  every interleaving; a retry is never double-charged.
- **Terminal-state immutability (database)**. A BEFORE UPDATE trigger
  freezes terminal `service_jobs` rows for every writer — the API (which
  never edits terminal rows), the service-role worker protocol (which
  replays without writing), and RLS clients (which could previously rewrite
  a terminal job directly over REST). This is the "job ID idempotency
  cannot create duplicate logical reservations / terminal state is
  immutable" invariant enforced where it belongs: in the row.
- **Honest race-loser outcomes (API adapter)**. A cancel that loses the
  claim/reconcile race (the conditional update sees zero rows) now returns
  the same "already terminal" outcome as the memory store and the C++
  service — not a fabricated 500.
- **The centralized rate limiter shipped**. `vortyx_rate_limit_take` was
  documented by 0003 but never defined; 0004 adds it as an atomic
  `INSERT … ON CONFLICT (key) DO UPDATE` counter (row-lock-serialized,
  exact across instances, `authenticated`-only execute). The API's
  submission path now actually works in supabase mode.
- **What did NOT change**: the worker pipeline (agent → protocol → native
  executor → orchestrator → shards), the execution operations
  (`vector_add` / `vector_multiply` / `vector_scale`), stale-lease
  semantics (expired leases are failed honestly, never auto-retried), the
  service-role boundary (service-role keys never reach a browser; worker
  credentials are never logged), the authorization table, the audit
  actions, and the C++ service layer (its quota ledger and artifact
  registry were already mutex-serialized, with the concurrent overcommit
  pinned by `ServiceQuotaTest`).
