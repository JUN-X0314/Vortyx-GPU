# Control-Plane API Contract (Phase 11)

The single source of truth for the HTTP surface. The TypeScript layer
(`platform/api/`) implements it on Vercel; the C++ contract codec
(`src/platform/contract.*`) implements the same request/response/error
vocabulary so device-side tests pin exactly what the server does.

## General rules

- **Base path**: `/api/*`. JSON request and response bodies.
- **Authentication**: `Authorization: Bearer <token>` on every data route.
  In production the token is a Supabase Auth access token, verified
  server-side (`auth.getUser`). Local/mock mode accepts the documented
  `local:<user_id>` scheme — mock only, never a real credential.
- **Error schema** (every failing response, both layers):

  ```json
  { "error": { "code": "<stable machine code>", "message": "<human text>" } }
  ```

- **Status codes** (consistent across every endpoint):

  | Status | Meaning |
  |--------|---------|
  | 200 | success |
  | 400 | request body is not valid JSON (`invalid_json`) |
  | 401 | no / unusable credentials (`unauthenticated`) |
  | 403 | authenticated but the operation violates an authorization rule (`forbidden`) |
  | 404 | resource does not exist **or belongs to another user** (`not_found`) — foreign records are invisible, mirroring RLS; existence is never leaked |
  | 405 | route exists, method not allowed (`method_not_allowed`) |
  | 409 | duplicate id / already-terminal state (`conflict`) |
  | 422 | schema or semantic validation failure (all other `InvalidInput` codes) |
  | 500 | unexpected internal failure / server misconfiguration (`internal_error`, `config_error`) |

- **Stable error codes**: `invalid_json`, `missing_field`, `invalid_type`,
  `invalid_enum`, `invalid_id`, `invalid_value`,
  `unsupported_protocol_version`, `invalid_request` (store-level validation),
  `unauthenticated`, `forbidden`, `not_found`, `conflict`,
  `method_not_allowed`, `internal_error`, `config_error` (server env only).
- **Idempotency**: `POST /api/jobs` is idempotent by `job_id` (see below).
  All other POST/PATCH operations are create-only or monotone; repeating
  them yields the documented error (409/422), never a silent duplicate.
- **Timestamps**: epoch milliseconds. `null` means "not set / not reported"
  — never a fabricated 0.
- **Route resolution precedes authentication**: an unknown route is 404 and
  a wrong method is 405 even without credentials (route existence is not a
  secret).

## Endpoints

### GET /api/health

Liveness + configuration readiness. No authentication.

```json
{ "status": "ok", "protocol_version": "1", "store": "memory|supabase", "config_error": null }
```

Never reports secret values. `config_error` carries the diagnostic message
when `VORTYX_STORE=supabase` is set but its settings are missing.

### GET /api/platform/info

What this control plane speaks. No authentication.

```json
{
  "protocol_version": "1",
  "software_version": "0.11.0",
  "operations": ["vector_add", "vector_multiply", "vector_scale"],
  "backends": ["cpu", "vulkan"]
}
```

### POST /api/devices/register

Register a NEW device owned by the authenticated user. **AuthZ**: the owner
is the authenticated subject — a client-claimed owner is not accepted.

Request:

```json
{
  "device_id": "uuid-v4-string",
  "protocol_version": "1",
  "software_version": "0.11.0",
  "operating_system": "linux",
  "architecture": "x86_64",
  "display_name": "workstation",
  "backends": ["cpu", "vulkan"],
  "operations": ["vector_add", "vector_multiply", "vector_scale"]
}
```

Only `device_id`, `protocol_version`, `software_version` are required;
unknown fields are rejected (strict contract — a typo must fail loudly).
`backends`/`operations` entries are validated against the contract
vocabulary. Ids must match `^[A-Za-z0-9._-]{1,128}$` (generated ids are
UUID v4). No hardware fingerprint is collected anywhere.

Responses: `200` the device record · `409` device_id already registered
(any owner — no existence leak) · `401`/`422` as above.

Device record shape (also used by GET /api/devices and heartbeat):

```json
{
  "device_id": "dev-1",
  "owner_user_id": "…",
  "display_name": "workstation",
  "protocol_version": "1",
  "software_version": "0.11.0",
  "operating_system": "linux",
  "architecture": "x86_64",
  "backends": ["cpu"],
  "operations": ["vector_add"],
  "status": "online",
  "last_seen_ms": 1700000000000,
  "created_at_ms": 1700000000000
}
```

### GET /api/devices

List the authenticated user's devices in registration order.
`200 { "devices": [record…] }`.

### PATCH /api/devices/:id/heartbeat

Mark an OWN device as heard-from: `status: "online"`, server-stamped
`last_seen_ms`. No body. `200` record · `404` missing or foreign id.

### POST /api/jobs

Submit a job. **Idempotent by `job_id`**: resubmitting the same id with the
same owner + payload returns the EXISTING job with `"created": false`
(200); the same id with a different payload or a different owner is `409`.

Request:

```json
{
  "job_id": "uuid-v4-string",
  "operation": "vector_add",
  "element_count": 1024,
  "requested_backend": "cpu",
  "priority": 3,
  "protocol_version": "1",
  "created_at_ms": 1700000000000,
  "submitted_by_device_id": "dev-1"
}
```

- Required: `job_id`, `operation`, `element_count`, `protocol_version`.
- `operation` ∈ `vector_add | vector_multiply | vector_scale` (else 422
  `invalid_enum`).
- `element_count`: integer, 1 ≤ n ≤ 2147483647 (else 422 `invalid_value`).
- `requested_backend`: absent/`""` or `cpu`/`vulkan` (else 422). Recorded,
  never silently remapped; honoring it is Phase 12+ execution policy.
- `priority`: RESERVED. Stored verbatim; nothing in Phase 11 reads it.
- `submitted_by_device_id`: optional. The store proves the device exists
  AND is owned by the caller — unknown or foreign references are `403`
  `forbidden` (exactly what the RLS INSERT policy produces).

Response `200` (job record + `created` flag):

```json
{
  "job_id": "job-1", "owner_user_id": "…", "submitted_by_device_id": "dev-1",
  "operation": "vector_add", "element_count": 1024,
  "requested_backend": "cpu", "priority": 3, "protocol_version": "1",
  "status": "queued", "error": "",
  "created_at_ms": 1700000000000, "started_at_ms": null, "completed_at_ms": null,
  "created": true
}
```

### GET /api/jobs

List the authenticated user's jobs in submission order.
`200 { "jobs": [record…] }`.

### GET /api/jobs/:id

One own job, any state. `200` record · `404` missing or foreign id.

### POST /api/jobs/:id/cancel

Owner cancellation. `200` the cancelled record (`status: "cancelled"`,
`error: "cancelled"`, `completed_at_ms` stamped). `404` missing/foreign ·
`422` already-terminal (illegal transition — the transition table is
pinned by tests on both sides).

## Documented status transition table

```
queued  → running | cancelled
running → completed | failed | cancelled
completed | failed | cancelled  (terminal: no further transitions)
```

A transition to `failed` REQUIRES a non-empty error reason (failures are
never hidden) — enforced by the store layer on both sides and reflected in
the database (`CHECK (status <> 'failed' OR error <> '')`).

## Result endpoints (Phase 11 note)

`POST /api/jobs/:id/result` and result retrieval exist at the STORE level
(`put_result` / `result` in `IPlatformStore`, tested on both sides) but are
deliberately NOT exposed as HTTP routes in Phase 11: there is no real
executor yet (that is Phase 12+), and shipping result endpoints with no
possible legitimate caller would be a fake feature. The store contract and
its rules are the tested, ready seam.

## Validation examples (all pinned by tests on BOTH sides)

| Input | Status | `error.code` |
|-------|--------|--------------|
| body is not JSON | 400 | `invalid_json` |
| missing `job_id` | 422 | `missing_field` |
| `operation: "matrix_multiply"` | 422 | `invalid_enum` |
| `element_count: 0` / `-5` / `1.5` | 422 | `invalid_value` |
| `requested_backend: "cuda"` | 422 | `invalid_enum` |
| `device_id: "bad id"` | 422 | `invalid_id` |
| `protocol_version: "2"` | 422 | `unsupported_protocol_version` |
| unknown field `ghost: 1` | 422 | `invalid_value` |
| no Authorization header | 401 | `unauthenticated` |
| GET another user's job | 404 | `not_found` |
| submit through a foreign device | 403 | `forbidden` |
| duplicate device registration | 409 | `conflict` |
| cancel an already-terminal job | 422 | `invalid_request` |
