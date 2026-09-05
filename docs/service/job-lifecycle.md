# Job Lifecycle, Authorization, Quota and Cancellation (Phase 14)

## Job lifecycle (service view)

The service REUSES the Phase 12 distributed job vocabulary — no new state
machine is invented. The service view collapses the orchestrator's
fine-grained states (the same collapse `map_to_platform_job_status`
performs):

```
Queued ──▶ Running ──▶ Completed
   │          │   └──▶ Failed
   └──────────┴──────▶ Cancelled
```

- **Queued** — accepted, waiting in the queue (auth/rate/quota all passed).
- **Running** — dispatched into the Phase 12 orchestrator. The
  orchestrator's own Queued/Planning/Scheduled/Running states live in its
  record (`PlatformService::distributed_record`); the service view shows
  Running while the job is in flight.
- **Completed / Failed / Cancelled** — terminal, immutable. Cancel on a
  terminal job is refused (`invalid_input`), the Phase 11 rule.

Transitions are the Phase 12 transition table restricted to the
service-visible states. Terminal states accept no mutation.

Fine-grained shard states (Pending/Assigned/Running/Retrying/...) remain a
Phase 12 concern and are visible through the distributed record — the
service does not duplicate them.

## Job ownership and visibility

- A job's owner is its SUBMITTER (the authenticated identity — never a
  client-claimed id).
- The submitter always sees and may cancel their own job.
- Project members see the project's jobs per the authz table
  (Viewer+ to view; Admin+ to cancel someone else's job in the project).
- Foreign/unknown jobs are `not_found` — never `forbidden` — the
  anti-enumeration rule Phase 11 established (a foreign row is invisible,
  exactly like RLS).

## Device ownership (the composition rule)

Phase 12's cluster is ownership-scoped: a job executes on devices owned by
the SUBMITTER. Phase 14 composes the layers as they are: a project member
submits jobs that run on **their own** devices; the project provides the
policy boundary (quota/rate/audit), never a privileged path into someone
else's cluster. A member with no devices gets the honest Phase 12 outcome
(`cluster_empty` at scheduling), not a fabricated shared pool.

## Cancellation semantics (all races defined, all tested)

| Race | Outcome |
|------|---------|
| cancel vs queued job | removed from the queue, Cancelled, quota released exactly once |
| cancel vs dispatching job | the dispatcher's atomic checkpoint observes the flag; the job is cancelled WITHOUT reaching the orchestrator |
| cancel vs running job | the orchestrator's cancel flag is set (bounded handoff covers the record-creation window); the in-flight shard finishes (the Phase 12 in-flight rule), remaining work is cancelled; the job terminal state is Cancelled |
| cancel vs completion | one side wins by the underlying state machines; the loser reports `invalid_input` ("already terminal") |

Quota is released EXACTLY ONCE per job no matter which path terminates it
(the ledger refuses the second release). Retry of shards is a Phase 12
concern internal to one submission and never touches the service quota
(one reservation per job lifetime).

## Idempotency

The client-supplied `job_id` is the idempotency key (the Phase 11 rule,
inherited end to end):

- same owner + same project + same envelope + same shard count → **replay**
  of the existing record: no side effects, no queue entry, no quota
  double-charge, not rate-limited;
- same id with a different owner/project/payload → `conflict`;
- concurrent duplicate submissions: the whole decision runs in one atomic
  section — exactly one created job, the others replays (tested with
  threads).

Service-level retry of a terminal job = a NEW submission with a NEW job id
(a replay of a terminal id returns the terminal record, by the Phase 11
rule). Shard-level retries inside one submission are Phase 12's, bounded,
and never re-charged.

## Quota model

Two SEPARATE systems with two separate responsibilities (no double
accounting):

1. **Project Quota (Phase 14)** — policy: how much one project may have in
   flight (concurrent jobs, running shards, reserved memory). The ledger
   is keyed by job id; reservation is atomic; release is exactly-once;
   replay is free; a refusals names the field.
2. **Cluster Reservation (Phase 12)** — allocation: the registry's atomic
   ResourceVector lease per placed shard, held for the shard's execution.
   The orchestrator owns it; it never reads quotas.

A job's memory figure is computed by the same Phase 12
`shard_memory_bytes` accounting on the whole element count — the quota
uses real size math (checked, overflow-refusing), not estimates.

## Rate limiting

Fixed window per key (`submit:<user_id>`), driven by the injected
`IClock` — deterministic and testable (FakeClock), no wall-clock
dependence. Refused attempts count toward the window (the stricter
contract). Replays bypass the limiter (they create no work). The default
policy ships enabled with a finite limit; disabling it is configuration,
not an accident.
