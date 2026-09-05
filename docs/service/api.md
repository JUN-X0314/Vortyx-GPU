# Service Contract (Phase 14, extended by Phase 15)

The Phase 14 control plane is a **C++-level contract**
(`vortyx::service`, `src/service/`). This document describes its public
surface and its machine-readable serialization. It is NOT an HTTP API: no
HTTP server exists in the C++ core (a documented non-goal), and the
Vercel/TypeScript layer remains the Phase 11/12 deployment boundary.

## Verification status vocabulary

The project distinguishes these consistently; Phase 14's claims are:

| Property | Status |
|----------|--------|
| Implemented (C++ service layer) | Yes — `src/service/`, 12 source pairs |
| Locally tested | Yes — 6 test suites, see the test table in the README |
| Sanitizer verified | Yes — ASan+UBSan clean |
| Verified in CI | See the repository's Actions runs for the current commit |
| Deployment verified | No — nothing here is deployed; deployment is not part of Phase 14 |

## Result vocabulary

`ServiceStatus` — stable snake_case codes and the HTTP mapping the
contract assigns (the mapping is pinned by tests even though no HTTP
server exists; a future transport inherits it unchanged):

| ServiceStatus | code | HTTP |
|---------------|------|------|
| Ok | `ok` | 200 |
| InvalidInput | `invalid_input` | 422 |
| Unauthenticated | `unauthenticated` | 401 |
| Forbidden | `forbidden` | 403 |
| NotFound | `not_found` | 404 |
| Conflict | `conflict` | 409 |
| QuotaExceeded | `quota_exceeded` | 429 |
| RateLimitExceeded | `rate_limit_exceeded` | 429 |
| UnsupportedOperation | `unsupported_operation` | 422 |
| Unavailable | `unavailable` | 503 |
| Internal | `internal` | 500 |

Anti-enumeration: an unknown project/job AND a foreign one are the SAME
`not_found` (the Phase 11 RLS-equivalent rule).

## JSON views (strict platform JSON subset, deterministic field order)

Project:
```json
{"project_id":"...","owner_user_id":"...","name":"...","status":"active|archived",
 "created_at_ms":N,"updated_at_ms":N}
```

Job:
```json
{"job_id":"...","project_id":"...","submitted_by":"...","operation":"vector_add",
 "element_count":N,"requested_shard_count":N,"requested_backend":"cpu",
 "status":"queued|running|completed|failed|cancelled","error":"",
 "submitted_at_ms":N,"terminal_at_ms":null,
 "total_shards":null,"succeeded_shards":null,"failed_shards":null}
```
Optional fields are `null` when not set — never a fake 0. `error` is
required for `failed` (failures are never hidden) and empty otherwise.

Error:
```json
{"error":{"code":"quota_exceeded","message":"project concurrent-job quota exceeded (4)"}}
```

Metrics (real counters only — nothing measured is estimated):
```json
{"submit_attempts":N,"jobs_submitted":N,"jobs_replayed":N,"jobs_completed":N,
 "jobs_failed":N,"jobs_cancelled":N,"quota_rejections":N,"rate_limit_rejections":N,
 "jobs_queued":N,"jobs_running":N}
```

Health:
```json
{"checked_at_ms":N,"overall":"healthy|unhealthy|unknown|not_configured",
 "components":[{"component":"service|queue|scheduler|platform_store|devices",
                "status":"...","detail":"..."}],
 "devices":{"total":N,"healthy":N,"unhealthy":N,"offline":N}}
```
`platform_store` is `not_configured` (never healthy) when no store is
attached. `devices` aggregates the CALLER's own devices only
(ownership-scoped; no device ids leave the ownership boundary).

## The C++ surface (the facade: `PlatformService`)

```cpp
// projects
create_project(auth, name) / project(auth, id) / projects(auth)
archive_project(auth, id)                                  // Owner only
add_member(auth, id, user, role) / remove_member(...)      // Admin+
members(auth, id)
set_project_quota(auth, id, quota)                          // Admin+, audited
project_usage(auth, id)                                     // Member+

// jobs — the full flow
submit_job(auth, {project_id, DistributedJobRequest}, out, created)
cancel_job(auth, job_id, out)
job(auth, job_id, out) / jobs(auth, optional project_id, out)
wait_for_terminal(auth, job_id, timeout_ms, out)            // honest timeout
distributed_record(auth, job_id, out)                       // Phase 12 detail

// devices (audited passthrough to the Phase 12 orchestrator)
register_device(auth, id, capabilities, created)
set_device_state(auth, id, state)
heartbeat_device(auth, id)

// artifacts (METADATA only — no payload storage exists)
register_artifact(auth, project_id, name, declared_byte_size, out)
artifacts(auth, project_id, out)

// observability
metrics() / audit_tail_for_actor(auth, count) / health_check(auth)
```

`AuthContext` is the Phase 11 type produced by a transport AFTER credential
verification. Phase 14 performs authorization; it never authenticates
tokens itself and never sees secrets.

## Provider-neutral stores (the extension points)

| Interface | In-memory reference | A future provider would be |
|-----------|--------------------|-----------------------------|
| `IProjectStore` | `InMemoryProjectStore` | Supabase/PostgreSQL adapter |
| `IJobQueue` | `InMemoryJobQueue` | Redis/cloud queue |
| `IAuditStore` | `InMemoryAuditStore` | append-only log store |
| `IArtifactStore` | `InMemoryArtifactStore` | object storage (payloads!) |
| `vortyx::platform::IPlatformStore` | `InMemoryPlatformStore` (Phase 11) | Supabase adapter |

None of these adapters is implemented in Phase 14; the interfaces are the
seams, and the in-memory implementations are the executable specification
(the Phase 11 pattern). No external system is claimed to be connected.

---

## Phase 15 additions

### The HTTP service surface (platform/api)

The TypeScript API exposes the service control plane over HTTP. Two
persistence providers behind ONE `IServiceStore` interface: the in-memory
store (tests + local dev) and the Supabase/PostgreSQL adapter (production;
per-user access-token clients under RLS, the service-role client ONLY for
the worker paths). New routes (the Phase 11 `/api/jobs`, `/api/devices` and
Phase 12 `/api/distributed/…`, `/api/cluster` routes are unchanged):

| Route | Method(s) | Purpose |
|---|---|---|
| `/api/me` | GET | The verified subject (from the token, never a body claim) |
| `/api/projects` | GET, POST | List the caller's projects (with role) / create |
| `/api/projects/:id` | GET | Project detail (membership-scoped; foreign = 404) |
| `/api/projects/:id/archive` | POST | Owner-only archival |
| `/api/projects/:id/members` | GET, POST | List / add (role ∈ admin·member·viewer — **owner is never grantable**) |
| `/api/projects/:id/members/:userId` | DELETE | Remove (owner row cannot be removed) |
| `/api/projects/:id/quota` | GET, PUT | Policy view (Member+) / change (Admin+) |
| `/api/projects/:id/usage` | GET | Derived live usage (in-flight jobs) |
| `/api/projects/:id/jobs` | GET, POST | List (paged, `limit` ≤ 100) / submit |
| `/api/service/jobs` | GET | The caller's visible jobs (paged, optional `project_id`) |
| `/api/service/jobs/:id` | GET | Job detail (status, attempt, shard summary, result metadata) |
| `/api/service/jobs/:id/cancel` | POST | Cancel (queued → cancelled; running → cancel flag; terminal → 422) |
| `/api/projects/:id/artifacts` | GET, POST | List / register metadata (bounded per project) |
| `/api/artifacts/:id` | DELETE | Creator or Admin+ (project scope derived server-side) |
| `/api/audit` | GET | The caller's own events |
| `/api/projects/:id/audit` | GET | Project events (Admin+) |
| `/api/metrics` | GET | Real status aggregates over the caller's visible jobs |
| `/api/worker/claim` | POST | **Worker token** — atomically claims the oldest queued job (reconciles stale leases first) |
| `/api/worker/jobs/:id/heartbeat` | POST | **Worker token** — renews the lease; returns `cancel_requested` |
| `/api/worker/jobs/:id/complete` · `/fail` | POST | **Worker token** — idempotent terminal commit (duplicate-safe) |
| `/api/internal/reconcile` | GET, POST | Worker/reconcile token — fails stale leases (`worker_lease_expired`) |

Status codes follow the C++ mapping (`service_status_http`): 401
unauthenticated, 403 forbidden, 404 not-found (anti-enumeration), 409
conflict, 422 invalid input / unsupported operation, 429 quota + rate
limit, 503 unavailable. The unified `{"error":{"code","message"}}` body
applies everywhere.

### Worker protocol wire contract

The native worker (`vortyx_worker_agent`) and the API share a strict JSON
contract (mirrored field-for-field by `src/worker/worker_protocol.hpp` and
`platform/api/src/service-contract.ts`):

```
POST /api/worker/claim          {"worker_id", "lease_ms"}
                             -> {"claimed":false} |
                                {"claimed":true,"job":{job_id, project_id,
                                 operation, element_count, requested_backend,
                                 requested_shard_count, attempt,
                                 lease_expires_at_ms}}

POST /api/worker/jobs/:id/heartbeat   {"worker_id"}
                             -> {"accepted":true,"cancel_requested":bool,
                                 "lease_expires_at_ms":N}   | 409 if not claimed

POST /api/worker/jobs/:id/complete    {"worker_id","status","error","backend",
                                      "result_element_count", "shards_total",
                                      "shards_succeeded", "shards_failed"}
                             -> {"recorded":true,"status":…} |
                                {"recorded":false,"status":…}  (idempotent replay)
```

Errors use the unified error body. Unknown fields are refused on BOTH sides
(the same strictness as every Vortyx contract).

### Persistence (migration 0003)

`platform/supabase/migrations/0003_service_init.sql` is ADDITIVE (0001/0002
untouched) and creates `projects`, `project_members`, `service_jobs` (with
the queue/lease columns), `quota_policies`, `artifact_metadata`,
`audit_events`, `rate_limit_windows` — each with RLS policies that derive
visibility from project membership (the DB backstop of the role table), plus:

- `vortyx_enforce_service_quota` — a BEFORE INSERT trigger that closes the
  check-then-insert race (usage derives from in-flight rows);
- `vortyx_enforce_single_owner` — the database-enforced single-owner
  invariant (only the creator row may carry `owner`);
- `vortyx_enforce_artifact_capacity` — the per-project metadata bound;
- `vortyx_worker_claim` — the atomic `FOR UPDATE SKIP LOCKED` claim;
- `vortyx_rate_limit_take` — the centralized fixed-window limiter (the
  in-process limiter is meaningless across serverless instances; documented).

Apply order: 0001 → 0002 → 0003. The migration is NOT applied to any live
project by this repository's automation — deployment is the owner's
explicit step (the same rule as every earlier migration).

### What is still not implemented (honesty)

No billing, no marketplace, no artifact payload storage (metadata only), no
TLS in the C++ worker, no automatic worker provisioning, no auto-retry of
stale jobs, no real GPU telemetry anywhere. See `docs/worker/` for the
execution-plane boundaries.
