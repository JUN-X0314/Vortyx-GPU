# Failure Handling, Retry, Leases, Heartbeats (Phase 12)

## Job and shard state machines

Two separate lifecycles, deliberately:

- **Job** (distributed): `queued → planning → scheduled → running →
  completed | failed | cancelled`. Terminal states are final. Partial
  failure **is a failure**: "8 shards, 7 succeeded, 1 failed" is `failed`
  with the succeeded count carried honestly in the result — success is
  never disguised, and no extra "partial" state multiplies the Phase 11
  mapping.
- **Shard**: `pending → assigned → running → completed | failed |
  retrying | cancelled` (plus `assigned → pending` for stale-plan
  re-plans). A `retrying` shard returns to `assigned` when re-placed;
  retry exhaustion moves it to `failed` terminally.

The job status is **derived** from the shard states
(`derive_job_status`, pure and unit-tested): any pending/retrying shard →
planning; any assigned → scheduled; any running → running; among
terminal-only sets a failure outranks cancellation, which outranks
success. The derivation is applied through the transition table, so an
illegal drift would be a loud error rather than a silent overwrite.

## Failure codes (the stable vocabulary)

Every failed attempt carries a `FailureCode`; retry decisions and
observability read codes, never error strings.

| Code | Meaning | Retryable |
|---|---|---|
| `worker_execution_failed` | the backend/runtime refused or failed the execution | yes |
| `device_lost` | the device went offline/unhealthy/failed, or no worker serves it | yes |
| `lease_expired` | the reservation lapsed before execution | yes |
| `shard_timeout` | the execution exceeded its deadline | yes |
| `cancelled` | owner-initiated cancellation | no |
| `invalid_assignment` | the assignment failed validation (planner/worker contract bug) | no |
| `duplicate_result` | a second report for a settled shard (visible, never double-counted) | no |

Retryable means "another device/at another time may genuinely succeed
and the work is not done". Non-retryable means "retrying deterministically
repeats a deterministic problem" — refused, not swallowed.

## Retry policy

- `max_attempts` is the **total** attempt ceiling including the first
  (config: `VORTYX_MAX_RETRIES` counts EXTRA attempts; default 3 → 4
  attempts). There is no unbounded mode — infinite retry is impossible by
  construction, and tests pin the exact attempt count at exhaustion.
- Backoff is the pure function `retry_delay_ms = base · 2^(attempt-1)`
  clamped at 60 s. Phase 12's synchronous in-process executor **stamps**
  the shard's `next_attempt_eligible_ms` for observability and re-places
  immediately without blocking (real waiting belongs to a future async
  executor; the delay function itself is unit-tested).
- On failure the shard's device is recorded, and its retry is re-placed
  **excluding that device** — a retry runs elsewhere whenever another
  capable device exists. Succeeded shards are never re-run (checkpoint
  semantics); a failed job's aggregate still reports the succeeded shards'
  counts.

## Leases (reservations with a lifetime)

- `reserve()` grants a lease **atomically** under the registry lock — the
  overcommit race between concurrent schedulers is structurally
  impossible (the second reservation is refused with the honest
  insufficient-memory/concurrency reason).
- `LeaseGuard` (RAII) returns the capacity on every error path; the
  orchestrator detaches guards into its bookkeeping and releases on
  completion, failure, cancellation, or terminal application.
- Releases must match the issued record exactly; double release is
  refused; expired leases are reclaimed lazily and deterministically on
  the injected clock. A crashed holder cannot leak capacity forever.
- Unregistering a device with active leases is refused explicitly, and
  capability changes under live allocations are rejected (no silent
  shrink of capacity that someone holds).

## Heartbeats and liveness

- `HeartbeatMonitor.check(owner)` judges every owned device whose last
  liveness evidence (registration stamp or heartbeat) is older than
  `VORTYX_HEARTBEAT_TIMEOUT_MS`: health → `unhealthy`, and schedulable
  states → `offline`. It never touches `failed` devices (their own
  remediation path) and never revives anything.
- Recovery is explicit: a heartbeat (the documented proof-of-life path)
  returns an offline device to `ready` with `healthy`.
- The orchestrator runs a freshness check **before every first
  placement**, so a plan is never built against stale liveness. All time
  comes from the injected clock — the monitor has no sleeps and no
  threads, and every transition is testable with a `FakeClock`.

## Cancellation

- `cancel_job` is owner-scoped (foreign jobs are invisible), refused for
  terminal jobs (`invalid_input`, the Phase 11 rule), and — because the
  synchronous executor owns the record — acts as a **cancellation
  request**: the executing submit observes the flag at every sequential
  dispatch boundary (and at wave boundaries in threaded mode), cancels the
  not-yet-started shards, releases their leases, and lets in-flight shards
  finish with their real recorded outcomes. The derived status then
  applies (completed + cancelled shards → `cancelled`).
- The loopback transport records `cancel_shard` requests; a future async
  transport implements real in-flight cancellation behind the same seam.

## Duplicate results and idempotency

- The aggregator keeps the **first verdict per shard**; any further report
  for a settled shard is counted as a duplicate — visible in the aggregate
  (`duplicates`), never double-counted, never overwriting the first
  verdict.
- Failed attempts that will be retried do not occupy the aggregator's
  slot — the verdict is the attempt that settles the shard.
- Submission is idempotent by job id (the Phase 11 rule): the same owner
  and payload replays the existing record; a different owner or payload is
  a conflict.
