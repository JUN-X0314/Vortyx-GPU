-- ============================================================================
-- Vortyx GPU — Production Stabilization (Phase 15.0.1, v0.15.1)
-- 0004_service_hardening.sql
--
-- Concurrency + integrity hardening of the Phase 15 service control plane.
-- ADDITIVE in spirit: nothing from 0001/0002/0003 is dropped or renamed;
-- this file REPLACES two trigger function BODIES in place (same names, same
-- signatures, same error-message shapes) and adds what 0003 documented but
-- did not ship.
--
-- What changed, and WHY each change needs the database (comments are the
-- design record; every claim below is enforced by SQL, not by convention):
--
--   1. PROJECT-ROW QUOTA SERIALIZATION (the race 0003 actually had):
--      vortyx_enforce_service_quota() read the policy and the in-flight
--      usage with plain MVCC SELECTs and then let the INSERT proceed. Two
--      concurrent submissions for the same project both read usage = N,
--      both pass "N + 1 <= limit", and both insert: the policy is exceeded
--      by exactly the requests racing each other. The check-then-act gap
--      is closed by taking SELECT ... FOR UPDATE on the PROJECT's own row
--      FIRST — every submission for project P now serializes on P's single
--      row lock, and the usage COUNT re-read AFTER acquiring the lock sees
--      every submission committed before it (READ COMMITTED takes a fresh
--      snapshot per statement). Quota semantics are UNCHANGED: the same
--      policy fields, the same 'queued'/'running' usage ledger, the same
--      error messages. Different projects lock different rows and stay
--      fully parallel.
--
--   2. ARTIFACT-CAPACITY SERIALIZATION (same race, same fix):
--      vortyx_enforce_artifact_capacity() counted rows with no lock, so
--      two concurrent inserts at count = 255 both passed and produced 257.
--      The trigger now takes the SAME project-row lock first. Sharing one
--      lock key with the quota trigger is deliberate: quota checks and
--      artifact capacity checks can never interleave with each other, and
--      the lock ORDER is identical everywhere (project row first, then
--      dependent-table reads), so the two paths cannot deadlock — neither
--      path ever holds a second lock the other path needs first.
--
--   3. THE MISSING RATE-LIMIT FUNCTION (0003 documented it, nothing
--      defined it): 0003's rate_limit_windows comment says the table is
--      "accessed ONLY through vortyx_rate_limit_take (below)" but no such
--      function existed — in supabase mode every job submission failed at
--      the limiter call. The function is defined here, atomic by
--      construction: INSERT ... ON CONFLICT (key) DO UPDATE takes the
--      counter row's lock, so concurrent takes on one key are serialized
--      by the database and the returned attempts value is exact across
--      API instances (the whole reason the counters live in the database).
--      Fixed-window semantics mirror the memory/C++ limiters exactly
--      (window boundaries are exact multiples; refused attempts count).
--
--   4. TERMINAL-STATE IMMUTABILITY (integrity as a constraint, not a
--      convention): the API and the worker protocol never modify a
--      terminal service_job, but RLS grants members UPDATE on their
--      project's rows, so a direct REST caller could resurrect or rewrite
--      a terminal job (fabricate a result, un-cancel, re-run). A BEFORE
--      UPDATE trigger now freezes terminal rows for EVERY writer
--      (application, service-role, RLS client alike). The documented
--      lifecycle transitions (claim, heartbeat lease renewal, cancel
--      relay, completion, reconciliation) only ever touch non-terminal
--      rows and are unaffected.
--
--   5. ONE MINIMAL INDEX for the hot path added in (1): the trigger's
--      usage read filters service_jobs by project + in-flight status.
--      A partial index keeps that read O(in-flight rows of the project)
--      — bounded by the quota itself — instead of scanning the project's
--      whole job history. Reason recorded here per the project rule that
--      every index earns its place.
--
--   6. 'project_missing' — the quota/artifact triggers raise this shape
--      when the locked project row does not exist (the FK would reject
--      the row anyway; the explicit raise makes the outcome honest and
--      mappable). The API adapter maps it to the standard not_found.
--
--   7. THE DEFAULT-QUOTA PATH FIXED (a real 0003 defect, reproduced on
--      PostgreSQL 17 while verifying this migration): 0003's trigger
--      selected the policy into a bare `record` and fell back to
--      `v_policy := row(4, 16, …)` when no quota_policies row existed.
--      That fallback creates an ANONYMOUS record (fields f1/f2/f3), so
--      every submission to a project that never set a policy — the
--      documented default path — failed with "record v_policy has no
--      field max_concurrent_jobs". The policy is now read into scalar
--      variables and the defaults are real. Same values (4 / 16 /
--      1073741824), same semantics, working path.
--
-- Lock-order contract (single rule, all project-level mutations):
--   projects row (FOR UPDATE)  ->  service_jobs / artifact_metadata reads
--   No path locks a dependent row first and the project row second, so
--   the reverse order that produces deadlocks cannot occur.
--
-- Idempotency: re-running this file is safe (CREATE OR REPLACE with
-- unchanged signatures, IF NOT EXISTS index, idempotent GRANT/REVOKE).
-- ============================================================================

-- ----------------------------------------------------------------------------
-- 1. Quota enforcement: project-row serialization (semantics unchanged).
-- ----------------------------------------------------------------------------

create or replace function public.vortyx_enforce_service_quota()
returns trigger
language plpgsql
security definer set search_path = public
as $$
declare
  v_max_jobs bigint;
  v_max_shards bigint;
  v_max_memory bigint;
  v_active_jobs bigint;
  v_shards bigint;
  v_memory bigint;
  v_project text;
begin
  -- THE SERIALIZATION POINT: lock THIS project's row before reading the
  -- policy or the usage. A concurrent submission for the same project
  -- blocks here until the earlier transaction commits, then re-reads
  -- usage INCLUDING that commit — "read 4, pass, insert, insert" can no
  -- longer produce 6. The FK guarantees the row exists for any insert
  -- that reaches the RI check; the explicit branch keeps the outcome
  -- honest if one somehow does not.
  select id into v_project
    from public.projects
    where id = new.project_id
    for update;
  if v_project is null then
    raise exception 'project_missing';
  end if;

  -- The policy is read into SCALARS on purpose: 0003 selected into a bare
  -- `record` and fell back to `row(4, 16, …)`, an anonymous record whose
  -- fields are f1/f2/f3 — so the documented no-policy-row default path
  -- (every project that never set a policy) failed outright with
  -- "record v_policy has no field max_concurrent_jobs". Scalars make the
  -- default path real: no row -> all three NULL -> the defaults below.
  select max_concurrent_jobs, max_running_shards, max_memory_bytes
    into v_max_jobs, v_max_shards, v_max_memory
    from public.quota_policies
    where project_id = new.project_id;
  if v_max_jobs is null then
    v_max_jobs := 4;          -- the documented service defaults
    v_max_shards := 16;
    v_max_memory := 1073741824;
  end if;

  -- Read AFTER the lock (fresh READ COMMITTED snapshot): sees every
  -- committed in-flight job, including ones this transaction waited for.
  select count(*), coalesce(sum(requested_shard_count), 0),
         coalesce(sum(vortyx_shard_memory_bytes(element_count, operation)), 0)
    into v_active_jobs, v_shards, v_memory
    from public.service_jobs
    where project_id = new.project_id
      and status in ('queued', 'running');

  if v_active_jobs + 1 > v_max_jobs then
    raise exception 'quota_exceeded:max_concurrent_jobs';
  end if;
  if v_shards + new.requested_shard_count > v_max_shards then
    raise exception 'quota_exceeded:max_running_shards';
  end if;
  if v_memory + vortyx_shard_memory_bytes(new.element_count, new.operation)
       > v_max_memory then
    raise exception 'quota_exceeded:max_memory_bytes';
  end if;
  return new;
end;
$$;

-- The trigger from 0003 keeps firing under the SAME name — only the
-- function body above changed. No trigger DDL needed here.

-- ----------------------------------------------------------------------------
-- 2. Artifact capacity: the SAME project-row lock, the SAME order.
-- ----------------------------------------------------------------------------

create or replace function public.vortyx_enforce_artifact_capacity()
returns trigger
language plpgsql
security definer set search_path = public
as $$
declare
  v_count bigint;
  v_project text;
begin
  -- Same serialization point (and same lock key) as the quota trigger:
  -- concurrent artifact inserts at count = 255 now take turns on the
  -- project row, and the loser re-counts AFTER the winner's commit.
  -- Lock order never inverts (project row first, nothing else locked).
  select id into v_project
    from public.projects
    where id = new.project_id
    for update;
  if v_project is null then
    raise exception 'project_missing';
  end if;

  select count(*) into v_count from public.artifact_metadata where project_id = new.project_id;
  if v_count >= 256 then
    raise exception 'artifact_capacity:the project has reached the artifact metadata capacity (256)';
  end if;
  return new;
end;
$$;

-- ----------------------------------------------------------------------------
-- 3. The centralized fixed-window rate limiter (documented by 0003,
--    defined here). The ONLY writer of rate_limit_windows.
-- ----------------------------------------------------------------------------

create or replace function public.vortyx_rate_limit_take(
  p_key text,
  p_window_ms bigint,
  p_max bigint
) returns boolean
language plpgsql
security definer set search_path = public
as $$
declare
  v_now bigint;
  v_window_start bigint;
  v_attempts bigint;
begin
  if p_key is null or char_length(p_key) = 0 or char_length(p_key) > 256 then
    raise exception 'rate_limit:invalid_key';
  end if;
  if p_window_ms is null or p_window_ms < 1000 or p_window_ms > 3600000 then
    raise exception 'rate_limit:invalid_window';
  end if;
  if p_max is null or p_max < 1 or p_max > 1000000 then
    raise exception 'rate_limit:invalid_max';
  end if;

  -- Fixed window: boundaries are exact multiples of the window size
  -- (mirror of the memory/C++ limiter arithmetic).
  v_now := (floor(extract(epoch from clock_timestamp()) * 1000))::bigint;
  v_window_start := v_now - (v_now % p_window_ms);

  -- THE ATOMIC TAKE: ON CONFLICT DO UPDATE locks the counter row, so two
  -- API instances taking the same key at the same moment serialize here
  -- and each observes the other's increment (no read-then-write race).
  -- A window boundary transition resets the counter in the same atomic
  -- statement. Refused attempts count (the take happens before the
  -- refusal — the documented limiter semantics).
  insert into public.rate_limit_windows (key, window_start, attempts)
  values (p_key, v_window_start, 1)
  on conflict (key) do update
    set window_start = v_window_start,
        attempts = case
          when rate_limit_windows.window_start = v_window_start
            then rate_limit_windows.attempts + 1
          else 1
        end
  returning attempts into v_attempts;

  return v_attempts <= p_max;
end;
$$;

-- The limiter is reachable only by authenticated callers (the API calls it
-- with the user's access token; the worker path never rate-limits).
-- Row access stays deny-all through RLS; this narrows the FUNCTION grant.
revoke execute on function public.vortyx_rate_limit_take(text, bigint, bigint) from public;
revoke execute on function public.vortyx_rate_limit_take(text, bigint, bigint) from anon;
grant execute on function public.vortyx_rate_limit_take(text, bigint, bigint) to authenticated;
grant execute on function public.vortyx_rate_limit_take(text, bigint, bigint) to service_role;

-- ----------------------------------------------------------------------------
-- 4. Terminal-state immutability for service_jobs (every writer, every path).
-- ----------------------------------------------------------------------------

create or replace function public.vortyx_enforce_terminal_immutable()
returns trigger
language plpgsql
security definer set search_path = public
as $$
begin
  -- A terminal job is FROZEN: no field may change, no matter who issues
  -- the UPDATE (the API never edits terminal rows, the worker protocol
  -- replays without writing, and RLS clients must not be able to rewrite
  -- history — a completed job's result or a cancelled job's state are
  -- exactly the records the audit trail and the user saw).
  if old.status in ('completed', 'failed', 'cancelled')
     and new is distinct from old then
    raise exception 'terminal_immutable:job % is terminal (%)', old.job_id, old.status;
  end if;
  return new;
end;
$$;

drop trigger if exists service_jobs_terminal_immutable on public.service_jobs;
create trigger service_jobs_terminal_immutable
  before update on public.service_jobs
  for each row execute function public.vortyx_enforce_terminal_immutable();

-- ----------------------------------------------------------------------------
-- 5. The in-flight usage index (the quota trigger's hot read).
-- ----------------------------------------------------------------------------

-- The trigger reads only in-flight rows of one project; this partial index
-- serves exactly that set (bounded by the project's own quota) instead of
-- scanning the project's full job history on every submission.
create index if not exists service_jobs_inflight_idx
  on public.service_jobs (project_id)
  where status in ('queued', 'running');
