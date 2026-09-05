-- ============================================================================
-- Vortyx GPU — Production Platform Integration (Phase 15, v0.15.0)
-- 0003_service_init.sql
--
-- The SERVICE control plane: projects, memberships, service jobs (with the
-- durable queue/lease state), quota policy, artifact metadata, audit events
-- and the centralized rate-limit counters — plus the worker-protocol
-- functions the native worker agent calls.
--
-- CANONICAL, APPLIED BY THE PROJECT OWNER (like 0001/0002: applying is the
-- deliberate post-phase deployment step). Apply 0001 FIRST, then 0002, then
-- this file (lexicographic order).
--
-- Design rules (unchanged from Phase 11/12 — every layer must agree):
--   * ADDITIVE: no existing table is altered; nothing from 0001/0002 is
--     dropped or renamed. New tables are a new namespace for the SERVICE
--     surface (public.projects / project_members / service_jobs /
--     quota_policies / artifact_metadata / audit_events /
--     rate_limit_windows); public.jobs and its family stay the Phase 11/12
--     source of truth and are untouched.
--   * SOURCE OF TRUTH (each datum lives in exactly one place):
--       projects, project_members   — projects + membership/roles
--       service_jobs                — the service job lifecycle AND the
--                                     durable queue/lease state (queued ->
--                                     claimed/running -> terminal); the
--                                     attempt counter and claim expiry live
--                                     here, not in a separate queue table
--       quota_policies              — per-project quota POLICY; usage is
--                                     DERIVED from in-flight service_jobs
--                                     (terminal = released exactly once, no
--                                     second ledger to drift)
--       artifact_metadata           — artifact METADATA only; NO payload
--                                     bytes are ever stored in the database
--       audit_events                — the audit trail (metadata only: who /
--                                     what / when / outcome / reason)
--       rate_limit_windows          — the centralized fixed-window counters
--   * NO credentials, NO secrets, NO hardware fingerprints, NO payloads,
--     NO tensor data anywhere. The worker's execution payload is synthesized
--     by the native executor (documented in docs/worker/) — the control
--     plane carries metadata only.
--   * NO new ownership model: project rows are owner-scoped (the creator);
--     every other visibility rule derives from project_members. RLS is the
--     backstop; the API layer applies the SAME rules in code first.
--   * THE SINGLE-OWNER INVARIANT is database-enforced: project_members.role
--     admits 'owner' only for the project's creator row (the insert
--     trigger), so no membership path can mint a second owner.
-- ============================================================================

-- ----------------------------------------------------------------------------
-- projects — the service unit of ownership and policy.
-- Ids are server-generated UUIDs (the API generates them, format-checked);
-- the id shape rule mirrors every other Vortyx id.
-- ----------------------------------------------------------------------------

create table public.projects (
  id text primary key
    check (char_length(id) between 1 and 128 and id ~ '^[A-Za-z0-9._-]+$'),
  owner_user_id uuid not null references auth.users (id) on delete cascade,
  name text not null
    check (char_length(name) between 1 and 128 and name !~ '[[:cntrl:]]'),
  status text not null default 'active'
    check (status in ('active', 'archived')),
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

comment on table public.projects is
  'One Vortyx project: the service unit of ownership, quota and jobs. Owner-scoped (RLS) plus member visibility.';

create index projects_owner_idx on public.projects (owner_user_id);

create trigger projects_set_updated_at
  before update on public.projects
  for each row execute function public.vortyx_set_updated_at();

-- ----------------------------------------------------------------------------
-- project_members — one row per (project, user). The creator's owner row is
-- inserted together with the project (trigger below).
-- ----------------------------------------------------------------------------

create table public.project_members (
  project_id text not null references public.projects (id) on delete cascade,
  user_id uuid not null references auth.users (id) on delete cascade,
  role text not null
    check (role in ('owner', 'admin', 'member', 'viewer')),
  created_at timestamptz not null default now(),

  unique (project_id, user_id)
);

comment on table public.project_members is
  'Membership of one user in one project. Exactly one owner per project (the creator) — enforced below.';

create index project_members_user_idx on public.project_members (user_id);

-- THE SINGLE-OWNER INVARIANT, database-enforced:
--   * the only row allowed to carry 'owner' is the creator row inserted with
--     the project (by the trigger);
--   * direct INSERTs/UPDATEs to role='owner' by anyone else are refused.
create or replace function public.vortyx_enforce_single_owner()
returns trigger
language plpgsql
security definer set search_path = public
as $$
declare
  project_owner uuid;
begin
  select owner_user_id into project_owner from public.projects where id = new.project_id;
  if project_owner is null then
    raise exception 'single_owner:project_missing';
  end if;
  if new.role = 'owner' and new.user_id <> project_owner then
    raise exception 'single_owner:owner_not_grantable';
  end if;
  if new.role <> 'owner' and new.user_id = project_owner then
    -- The creator''s row stays Owner; a demotion would orphan the project.
    raise exception 'single_owner:owner_unchangeable';
  end if;
  return new;
end;
$$;

create trigger project_members_single_owner_insert
  before insert on public.project_members
  for each row execute function public.vortyx_enforce_single_owner();

create trigger project_members_single_owner_update
  before update on public.project_members
  for each row execute function public.vortyx_enforce_single_owner();

-- Creates the creator''s owner member row with every new project.
create or replace function public.vortyx_handle_new_project()
returns trigger
language plpgsql
security definer set search_path = public
as $$
begin
  insert into public.project_members (project_id, user_id, role)
  values (new.id, new.owner_user_id, 'owner');
  return new;
end;
$$;

create trigger on_project_created
  after insert on public.projects
  for each row execute function public.vortyx_handle_new_project();

-- ----------------------------------------------------------------------------
-- service_jobs — the service job record: submission metadata, the service
-- lifecycle (queued -> running -> completed|failed|cancelled), the honest
-- execution summary written by the worker's report, and the durable
-- queue/lease state (attempt, claimed_by, claim_expires_at_ms).
--
-- job_id is the CLIENT-supplied idempotency key (the Phase 11 rule): the
-- primary key makes a duplicate logical submission impossible at the
-- storage layer; a same-key different-payload resubmission is the API's
-- conflict. submitted_at_ms/terminal_at_ms are EPOCH MILLISECONDS (the wire
-- contract's timestamps; the control plane never fabricates them).
-- ----------------------------------------------------------------------------

create table public.service_jobs (
  job_id text primary key
    check (char_length(job_id) between 1 and 128 and job_id ~ '^[A-Za-z0-9._-]+$'),
  project_id text not null references public.projects (id) on delete cascade,
  submitted_by uuid not null references auth.users (id) on delete cascade,
  operation text not null
    check (operation in ('vector_add', 'vector_multiply', 'vector_scale')),
  element_count bigint not null
    check (element_count > 0 and element_count <= 2147483647),
  requested_backend text not null default ''
    check (requested_backend in ('', 'cpu', 'vulkan')),
  requested_shard_count integer not null default 1
    check (requested_shard_count between 1 and 64),
  status text not null default 'queued'
    check (status in ('queued', 'running', 'completed', 'failed', 'cancelled')),
  error text not null default '',
  submitted_at_ms bigint not null,
  terminal_at_ms bigint,
  total_shards integer,
  succeeded_shards integer,
  failed_shards integer,
  result_element_count bigint,
  result_backend text,
  attempt integer not null default 0 check (attempt >= 0),
  claimed_by text,
  claim_expires_at_ms bigint,
  cancel_requested boolean not null default false,

  check ((status in ('completed', 'failed', 'cancelled')) = (terminal_at_ms is not null)),
  check (status <> 'failed' or error <> '')
);

comment on table public.service_jobs is
  'One service job: metadata, lifecycle and queue/lease state. Payloads are NEVER stored — execution data is the native worker''s business.';

create index service_jobs_project_idx on public.service_jobs (project_id, submitted_at_ms);
create index service_jobs_status_idx on public.service_jobs (status, submitted_at_ms);
create index service_jobs_claim_idx on public.service_jobs (status, claim_expires_at_ms)
  where status = 'running';

-- The quota POLICY enforcement, atomically at insert: a concurrent
-- submission cannot race past the policy (the check-then-insert gap is
-- closed INSIDE the database). Usage is derived from in-flight rows.
create or replace function public.vortyx_shard_memory_bytes(
  p_element_count bigint,
  p_operation text
) returns bigint
language sql
immutable
as $$
  select p_element_count * (case p_operation when 'vector_scale' then 8 else 12 end);
$$;

create or replace function public.vortyx_enforce_service_quota()
returns trigger
language plpgsql
security definer set search_path = public
as $$
declare
  v_policy record;
  v_active_jobs bigint;
  v_shards bigint;
  v_memory bigint;
begin
  select max_concurrent_jobs, max_running_shards, max_memory_bytes
    into v_policy
    from public.quota_policies
    where project_id = new.project_id;
  if v_policy is null then
    v_policy := row(4, 16, 1073741824);  -- the documented service defaults
  end if;

  select count(*), coalesce(sum(requested_shard_count), 0),
         coalesce(sum(vortyx_shard_memory_bytes(element_count, operation)), 0)
    into v_active_jobs, v_shards, v_memory
    from public.service_jobs
    where project_id = new.project_id
      and status in ('queued', 'running');

  if v_active_jobs + 1 > v_policy.max_concurrent_jobs then
    raise exception 'quota_exceeded:max_concurrent_jobs';
  end if;
  if v_shards + new.requested_shard_count > v_policy.max_running_shards then
    raise exception 'quota_exceeded:max_running_shards';
  end if;
  if v_memory + vortyx_shard_memory_bytes(new.element_count, new.operation)
       > v_policy.max_memory_bytes then
    raise exception 'quota_exceeded:max_memory_bytes';
  end if;
  return new;
end;
$$;

create trigger service_jobs_quota_check
  before insert on public.service_jobs
  for each row execute function public.vortyx_enforce_service_quota();

-- ----------------------------------------------------------------------------
-- quota_policies — per-project quota POLICY (Admin+ writable through the
-- API; RLS read for members). Absent row = the documented defaults.
-- ----------------------------------------------------------------------------

create table public.quota_policies (
  project_id text primary key references public.projects (id) on delete cascade,
  max_concurrent_jobs bigint not null check (max_concurrent_jobs >= 0),
  max_running_shards bigint not null check (max_running_shards >= 0),
  max_memory_bytes bigint not null check (max_memory_bytes >= 0),
  updated_by uuid references auth.users (id) on delete set null,
  updated_at_ms bigint,
  created_at timestamptz not null default now()
);

comment on table public.quota_policies is
  'Per-project quota policy. Usage is NOT stored here — it derives from in-flight service_jobs.';

-- ----------------------------------------------------------------------------
-- artifact_metadata — artifact METADATA only (name, size claim, owner).
-- The per-project count is bounded by the trigger below (a registry that
-- grows without limit is an unbounded-growth defect). There is deliberately
-- NO payload column: artifact BYTES live in a storage provider outside the
-- control plane (documented boundary), never in this database.
-- ----------------------------------------------------------------------------

create table public.artifact_metadata (
  artifact_id text primary key
    check (char_length(artifact_id) between 1 and 128 and artifact_id ~ '^[A-Za-z0-9._-]+$'),
  project_id text not null references public.projects (id) on delete cascade,
  name text not null
    check (char_length(name) between 1 and 128 and name !~ '[[:cntrl:]]'),
  created_by uuid not null references auth.users (id) on delete cascade,
  declared_byte_size bigint not null check (declared_byte_size >= 0),
  created_at_ms bigint not null
);

comment on table public.artifact_metadata is
  'Artifact METADATA only. No payload bytes exist in the control plane.';

create index artifact_metadata_project_idx on public.artifact_metadata (project_id, created_at_ms);

create or replace function public.vortyx_enforce_artifact_capacity()
returns trigger
language plpgsql
security definer set search_path = public
as $$
declare
  v_count bigint;
begin
  select count(*) into v_count from public.artifact_metadata where project_id = new.project_id;
  if v_count >= 256 then
    raise exception 'artifact_capacity:the project has reached the artifact metadata capacity (256)';
  end if;
  return new;
end;
$$;

create trigger artifact_metadata_capacity_check
  before insert on public.artifact_metadata
  for each row execute function public.vortyx_enforce_artifact_capacity();

-- ----------------------------------------------------------------------------
-- audit_events — the audit trail. Metadata only: actor / project / job /
-- action / outcome / reason. No secrets, no payloads (the event shape has no
-- field that could carry them). Signup is audited by the trigger below.
-- ----------------------------------------------------------------------------

create table public.audit_events (
  event_id text primary key
    default 'evt-' || lpad((floor(extract(epoch from clock_timestamp()) * 1000))::text, 13, '0')
             || '-' || lpad(floor(random() * 1000000)::text, 6, '0'),
  timestamp_ms bigint not null,
  actor_user_id uuid references auth.users (id) on delete set null,
  project_id text,
  job_id text,
  action text not null
    check (action in ('auth_signup', 'project_create', 'project_archive', 'membership_change',
                      'job_submit', 'job_cancel', 'job_terminal', 'quota_change',
                      'artifact_register', 'artifact_delete')),
  outcome text not null check (outcome in ('ok', 'denied', 'error')),
  reason_code text not null default '',
  created_at timestamptz not null default now()
);

comment on table public.audit_events is
  'The audit trail: who did what, when, with what outcome. Metadata only, secret-free by construction.';

create index audit_events_actor_idx on public.audit_events (actor_user_id, timestamp_ms desc);
create index audit_events_project_idx on public.audit_events (project_id, timestamp_ms desc)
  where project_id is not null;

-- Signup audit: every new authenticated user leaves one event (the
-- authentication-sensitive operation Phase 15 audits).
create or replace function public.vortyx_audit_new_user()
returns trigger
language plpgsql
security definer set search_path = public
as $$
begin
  insert into public.audit_events (timestamp_ms, actor_user_id, action, outcome)
  values ((floor(extract(epoch from clock_timestamp()) * 1000))::bigint, new.id, 'auth_signup', 'ok');
  return new;
end;
$$;

create trigger on_auth_user_created_audit
  after insert on auth.users
  for each row execute function public.vortyx_audit_new_user();

-- ----------------------------------------------------------------------------
-- rate_limit_windows — the centralized fixed-window counters (the API runs
-- on multiple serverless instances; per-instance memory would make the
-- limit meaningless — documented limitation of the in-memory limiter).
-- Accessed ONLY through vortyx_rate_limit_take (below); no direct client
-- policies (RLS enabled + no policies = deny-all for anon/authenticated).
-- ----------------------------------------------------------------------------

create table public.rate_limit_windows (
  key text primary key,
  window_start bigint not null,
  attempts bigint not null default 0
);

comment on table public.rate_limit_windows is
  'Centralized fixed-window rate-limit counters. Only the take() function touches this table.';

alter table public.rate_limit_windows enable row level security;

-- ============================================================================
-- Worker-protocol functions — the native execution boundary.
--
-- These run with the CALLER of the RPC: the API calls them with the
-- service-role client (the worker's bearer token is checked by the API
-- BEFORE any database access; the database never sees worker tokens).
-- security definer so the service-role path can coordinate rows it does not
-- own; the functions are deliberately narrow (claim/heartbeat/complete/
-- reconcile) and validate their inputs.
-- ============================================================================

-- Fails stale running jobs whose lease expired (the honest recovery:
-- worker_lease_expired; no automatic re-execution — a retry is an explicit
-- resubmission with a new attempt by the operator/user contract).
create or replace function public.vortyx_worker_reconcile()
returns integer
language plpgsql
security definer set search_path = public
as $$
declare
  v_recovered integer := 0;
begin
  update public.service_jobs
    set status = 'failed',
        error = 'worker_lease_expired',
        terminal_at_ms = (floor(extract(epoch from clock_timestamp()) * 1000))::bigint
    where status = 'running'
      and claim_expires_at_ms is not null
      and claim_expires_at_ms < (floor(extract(epoch from clock_timestamp()) * 1000))::bigint;
  get diagnostics v_recovered = row_count;
  return v_recovered;
end;
$$;

-- The atomic claim: reconcile first, then take the OLDEST queued job
-- (FOR UPDATE SKIP LOCKED — two workers can never claim the same job).
create or replace function public.vortyx_worker_claim(
  p_worker_id text,
  p_lease_ms bigint
) returns jsonb
language plpgsql
security definer set search_path = public
as $$
declare
  v_job record;
  v_now bigint;
begin
  if p_worker_id is null or char_length(p_worker_id) = 0 then
    raise exception 'worker_claim:worker_id_required';
  end if;
  if p_lease_ms is null or p_lease_ms < 1000 or p_lease_ms > 600000 then
    raise exception 'worker_claim:lease_out_of_range';
  end if;
  perform public.vortyx_worker_reconcile();
  v_now := (floor(extract(epoch from clock_timestamp()) * 1000))::bigint;

  select * into v_job
    from public.service_jobs
    where status = 'queued'
    order by submitted_at_ms asc
    limit 1
    for update skip locked;

  if v_job is null then
    return jsonb_build_object('claimed', false, 'job', null);
  end if;

  update public.service_jobs
    set status = 'running',
        claimed_by = p_worker_id,
        claim_expires_at_ms = v_now + p_lease_ms,
        attempt = attempt + 1
    where job_id = v_job.job_id;

  return jsonb_build_object('claimed', true, 'job', to_jsonb(
    jsonb_build_object(
      'job_id', v_job.job_id,
      'project_id', v_job.project_id,
      'operation', v_job.operation,
      'element_count', v_job.element_count,
      'requested_backend', v_job.requested_backend,
      'requested_shard_count', v_job.requested_shard_count,
      'attempt', v_job.attempt + 1,
      'lease_expires_at_ms', v_now + p_lease_ms
    )
  ));
end;
$$;

-- The lease renewal + cancellation relay (the worker's liveness contract).
create or replace function public.vortyx_worker_heartbeat(
  p_worker_id text,
  p_job_id text,
  p_lease_ms bigint
) returns jsonb
language plpgsql
security definer set search_path = public
as $$
declare
  v_job record;
  v_new_expiry bigint;
begin
  if p_lease_ms is null or p_lease_ms < 1000 or p_lease_ms > 600000 then
    raise exception 'worker_heartbeat:lease_out_of_range';
  end if;
  v_new_expiry := (floor(extract(epoch from clock_timestamp()) * 1000))::bigint + p_lease_ms;
  update public.service_jobs
    set claim_expires_at_ms = v_new_expiry
    where job_id = p_job_id
      and status = 'running'
      and claimed_by = p_worker_id
    returning * into v_job;
  if v_job is null then
    return jsonb_build_object('accepted', false);
  end if;
  return jsonb_build_object(
    'accepted', true,
    'cancel_requested', v_job.cancel_requested,
    'lease_expires_at_ms', v_new_expiry
  );
end;
$$;

-- The idempotent result commit: a second identical report returns the
-- existing terminal state (no duplicate result); a report from a worker
-- that does not hold the claim is refused.
create or replace function public.vortyx_worker_complete(
  p_worker_id text,
  p_job_id text,
  p_status text,
  p_error text,
  p_backend text,
  p_result_element_count bigint,
  p_shards_total integer,
  p_shards_succeeded integer,
  p_shards_failed integer
) returns jsonb
language plpgsql
security definer set search_path = public
as $$
declare
  v_job record;
  v_now bigint;
begin
  if p_status not in ('completed', 'failed', 'cancelled') then
    raise exception 'worker_complete:bad_status';
  end if;
  if p_status in ('failed', 'cancelled') and (p_error is null or char_length(p_error) = 0) then
    raise exception 'worker_complete:reason_required';
  end if;

  select * into v_job from public.service_jobs where job_id = p_job_id for update;
  if v_job is null then
    raise exception 'worker_complete:no_such_job';
  end if;

  -- Idempotent replay: an already-terminal job returns its existing state
  -- (the duplicate-safe commit rule).
  if v_job.status in ('completed', 'failed', 'cancelled') then
    return jsonb_build_object('recorded', false, 'status', v_job.status);
  end if;
  if v_job.status <> 'running' or v_job.claimed_by is null or v_job.claimed_by <> p_worker_id then
    raise exception 'worker_complete:not_claimed_by_this_worker';
  end if;

  v_now := (floor(extract(epoch from clock_timestamp()) * 1000))::bigint;
  update public.service_jobs
    set status = p_status,
        error = coalesce(p_error, ''),
        terminal_at_ms = v_now,
        total_shards = p_shards_total,
        succeeded_shards = p_shards_succeeded,
        failed_shards = p_shards_failed,
        result_element_count = p_result_element_count,
        result_backend = nullif(p_backend, '')
    where job_id = p_job_id;

  return jsonb_build_object('recorded', true, 'status', p_status);
end;
$$;

-- ============================================================================
-- Row Level Security — the database-level backstop. Every rule below
-- derives visibility from project membership (or ownership), mirroring the
-- API layer's code and the C++ service's tables. No anon policies: with RLS
-- enabled and none granted, unauthenticated requests see nothing.
-- ============================================================================

alter table public.projects enable row level security;
alter table public.project_members enable row level security;
alter table public.service_jobs enable row level security;
alter table public.quota_policies enable row level security;
alter table public.artifact_metadata enable row level security;
alter table public.audit_events enable row level security;

-- projects: visible to owner + members; mutations through the API only
-- (insert is the creator — enforced with check; updates restricted to the
-- owner's archive transition shape).
create policy "projects_select_member"
  on public.projects for select
  using (
    exists (
      select 1 from public.project_members m
      where m.project_id = projects.id and m.user_id = auth.uid()
    )
  );

create policy "projects_insert_own"
  on public.projects for insert
  with check (auth.uid() = owner_user_id);

create policy "projects_update_own"
  on public.projects for update
  using (auth.uid() = owner_user_id)
  with check (auth.uid() = owner_user_id);

-- project_members: members read; the API layer enforces the manage rules
-- (the insert/update policies allow any member to write the table ONLY
-- within the single-owner trigger's constraints — the API is the real
-- gatekeeper, the trigger is the invariant backstop).
create policy "project_members_select_member"
  on public.project_members for select
  using (
    exists (
      select 1 from public.project_members m
      where m.project_id = project_members.project_id and m.user_id = auth.uid()
    )
  );

create policy "project_members_insert_member"
  on public.project_members for insert
  with check (
    exists (
      select 1 from public.project_members m
      where m.project_id = project_members.project_id and m.user_id = auth.uid()
    )
  );

create policy "project_members_delete_member"
  on public.project_members for delete
  using (
    exists (
      select 1 from public.project_members m
      where m.project_id = project_members.project_id and m.user_id = auth.uid()
    )
    and project_members.role <> 'owner'
  );

-- service_jobs: visibility derives from PROJECT MEMBERSHIP (the project is
-- the unit of access; a foreign project's jobs are simply invisible, exactly
-- like RLS on the Phase 11 tables).
create policy "service_jobs_select_member"
  on public.service_jobs for select
  using (
    exists (
      select 1 from public.project_members m
      where m.project_id = service_jobs.project_id and m.user_id = auth.uid()
    )
  );

create policy "service_jobs_insert_member"
  on public.service_jobs for insert
  with check (
    exists (
      select 1 from public.project_members m
      where m.project_id = service_jobs.project_id
        and m.user_id = auth.uid()
        and m.role in ('owner', 'admin', 'member')
    )
    and auth.uid() = submitted_by
  );

create policy "service_jobs_update_member"
  on public.service_jobs for update
  using (
    exists (
      select 1 from public.project_members m
      where m.project_id = service_jobs.project_id and m.user_id = auth.uid()
    )
  )
  with check (
    exists (
      select 1 from public.project_members m
      where m.project_id = service_jobs.project_id and m.user_id = auth.uid()
    )
  );

-- quota_policies: members read; writes go through the API (no direct
-- insert/update policy for regular members — Admin+ writes flow through the
-- API layer's role check; the service role bypasses RLS for the upsert).
create policy "quota_policies_select_member"
  on public.quota_policies for select
  using (
    exists (
      select 1 from public.project_members m
      where m.project_id = quota_policies.project_id and m.user_id = auth.uid()
    )
  );

create policy "quota_policies_write_admin"
  on public.quota_policies for all
  using (
    exists (
      select 1 from public.project_members m
      where m.project_id = quota_policies.project_id
        and m.user_id = auth.uid()
        and m.role in ('owner', 'admin')
    )
  )
  with check (
    exists (
      select 1 from public.project_members m
      where m.project_id = quota_policies.project_id
        and m.user_id = auth.uid()
        and m.role in ('owner', 'admin')
    )
  );

-- artifact_metadata: project-member visibility; creator-or-admin deletion
-- is the API's rule (the delete policy keeps the shape at the DB level).
create policy "artifact_metadata_select_member"
  on public.artifact_metadata for select
  using (
    exists (
      select 1 from public.project_members m
      where m.project_id = artifact_metadata.project_id and m.user_id = auth.uid()
    )
  );

create policy "artifact_metadata_insert_member"
  on public.artifact_metadata for insert
  with check (
    exists (
      select 1 from public.project_members m
      where m.project_id = artifact_metadata.project_id
        and m.user_id = auth.uid()
        and m.role in ('owner', 'admin', 'member')
    )
    and auth.uid() = created_by
  );

create policy "artifact_metadata_delete_member"
  on public.artifact_metadata for delete
  using (
    exists (
      select 1 from public.project_members m
      where m.project_id = artifact_metadata.project_id
        and m.user_id = auth.uid()
        and (
          artifact_metadata.created_by = auth.uid()
          or m.role in ('owner', 'admin')
        )
    )
  );

-- audit_events: an actor reads their OWN events; project admins/owners read
-- the project's events. Inserts flow through the API (which stamps the
-- actor from the VERIFIED subject — no client-claimed actor).
create policy "audit_events_select_own"
  on public.audit_events for select
  using (auth.uid() = actor_user_id);

create policy "audit_events_select_project_admin"
  on public.audit_events for select
  using (
    project_id is not null
    and exists (
      select 1 from public.project_members m
      where m.project_id = audit_events.project_id
        and m.user_id = auth.uid()
        and m.role in ('owner', 'admin')
    )
  );

create policy "audit_events_insert_any_authenticated"
  on public.audit_events for insert
  with check (auth.uid() is not null);
