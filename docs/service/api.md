# Service Contract (Phase 14)

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
