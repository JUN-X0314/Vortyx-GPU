# Distributed API Contract (Phase 12)

The distributed surface extends the Phase 11 control-plane contract with
the same conventions: the unified error body
`{"error":{"code":"...","message":"..."}}`, the same stable error codes,
the same status mapping (200 / 400 `invalid_json` / 401 / 403 / 404 / 405 /
409 / 422 / 500), the same strict JSON module, and byte-deterministic
serialization. The contract is implemented twice — C++
(`src/distributed/contract_distributed.*`, the reference) and TypeScript
(`platform/api/src/{contract,router,distributed}.ts`, the Vercel mirror) —
and pinned by tests on both sides.

**Metadata only.** A distributed submission carries no compute payload
(the local `ComputeTask` never travels the control plane), responses carry
no result data (the reassembled output lives with the local caller), and
any unknown field in a request is rejected.

## Endpoints

All routes require authentication (the Phase 11 `Authorization: Bearer`
surface) except where noted. Ownership is enforced end to end: foreign
jobs and devices are invisible (`404 not_found`).

### `GET /api/cluster`

The caller's device scheduling view.

```json
{
  "revision": 41,
  "devices": [
    {
      "device_id": "device-0",
      "state": "ready",
      "health": "healthy",
      "capacity": {"compute_units": 0, "memory_bytes": 8388608, "concurrent_jobs": 2},
      "allocated": {"compute_units": 0, "memory_bytes": 0, "concurrent_jobs": 0},
      "backends": ["cpu"],
      "running_shards": 0,
      "last_heartbeat_ms": 5000
    }
  ]
}
```

`state` uses the distributed device vocabulary
(`registering|ready|busy|draining|offline|failed`), `health` is
`healthy|unhealthy|unknown`. The revision is the registry's monotonic
cluster revision. In the local/mock control plane the view starts empty —
cluster scheduling state is reported by device agents, and the C++
registry remains its authority.

### `POST /api/distributed/jobs`

Submit a distributed job (metadata only).

```json
{
  "job_id": "job-1",
  "operation": "vector_add",
  "element_count": 4096,
  "requested_shard_count": 4,
  "requested_backend": "cpu",
  "priority": 0,
  "protocol_version": "1"
}
```

- `requested_shard_count` is REQUIRED (the single-device vs multi-device
  choice is explicit): `1` = single-device execution, `> 1` = multi-device.
- `requested_backend` is optional (`""` = the device's own preference).
- Idempotency: the same `job_id` with the same owner and payload replays
  the existing record (with `created: false` in the C++ orchestration
  path); a different owner or payload is `409 conflict`.
- Validation failures use the shared codes: `invalid_json` (400),
  `missing_field` / `invalid_type` / `invalid_enum` / `invalid_value` /
  `invalid_id` / `unsupported_protocol_version` (422).

Response: the job record (below) with `created: true` added by the
orchestration path.

### `GET /api/distributed/jobs`

The caller's distributed jobs, submission order:
`{"jobs": [ <job record>, ... ]}`.

### `GET /api/distributed/jobs/:id`

One own job (any state). Field order is part of the contract:

```json
{
  "job_id": "job-1",
  "operation": "vector_add",
  "element_count": 4096,
  "requested_backend": "cpu",
  "requested_shard_count": 4,
  "status": "running",
  "error": "",
  "shards": [
    {
      "shard_id": "job-1-s0",
      "index": 0,
      "state": "completed",
      "element_begin": 0,
      "element_end": 1024,
      "device_id": "device-0",
      "attempt": 1,
      "retry_count": 0,
      "failure_code": ""
    }
  ],
  "shard_count": 1,
  "succeeded": 1,
  "failed": 0,
  "cancelled": 0,
  "duplicates": 0,
  "completed": false,
  "created_at_ms": 1725500000000,
  "completed_at_ms": null
}
```

`status` uses the distributed vocabulary (`queued | planning | scheduled |
running | completed | failed | cancelled`); shard `state` uses
(`pending | assigned | running | completed | failed | retrying |
cancelled`); `failure_code` carries the stable failure vocabulary
(`worker_execution_failed`, `device_lost`, `lease_expired`,
`shard_timeout`, `cancelled`, `invalid_assignment`, `duplicate_result`).

### `GET /api/distributed/jobs/:id/shards`

The shard table alone: `{"job_id": "...", "shards": [ ... ]}`.

### `POST /api/distributed/jobs/:id/cancel`

Owner cancellation of a non-terminal job (`200` with the updated record;
terminal jobs are `422 invalid_input`; foreign jobs are `404`).
Cancellation follows the semantics in `failure-handling.md`.

## Platform integration (the Phase 11 records)

A distributed submission IS a platform job: the C++ orchestrator mirrors
the lifecycle through the provider-neutral `IPlatformStore` —
`create_job` (queued), `update_job` (running), then `put_result`, whose
terminal transition carries the honest outcome metadata (`backend`,
`result_element_count`, the failure reason when failed). The result
PAYLOAD is never stored remotely. The Supabase schema for the distributed
records lives in `platform/supabase/migrations/0002_distributed_init.sql`
(canonical; like 0001, deliberately not yet applied to the production
project) and derives every permission from the owning job — no new
ownership rules.

## CLI / debug visibility

- `vortyx_cluster` (built with the project) runs a real four-device local
  cluster end to end and prints the cluster view, the job's shard table
  and a bit-exactness verification against a host reference.
- `to_debug_string(ClusterSnapshot)` / `to_debug_string(job)` render the
  same deterministic state for tests and logs.
