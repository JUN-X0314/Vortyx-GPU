-- ============================================================================
-- Vortyx GPU — Distributed / Multi-GPU Device System (Phase 12, v0.12.0)
-- 0002_distributed_init.sql
--
-- The control-plane records for the DISTRIBUTED surface. This migration is
-- CANONICAL but NOT YET APPLIED to the production project — exactly like
-- 0001, deployment is the project owner's deliberate post-phase step
-- (Supabase project: vortyx-gpu, ref ubkxdctfgfzoojlbcaoh,
-- region ap-northeast-2; schema was verified EMPTY before Phase 12).
-- Apply 0001 FIRST (lexicographic order), then this file.
--
-- Design rules (unchanged from Phase 11 — the three layers must agree):
--   * NO new ownership model: distributed rows hang off public.jobs and
--     inherit its owner_user_id. Every policy below derives visibility
--     from the OWNING JOB — the same rule the C++ layer (auth.hpp) and
--     the API layer (auth.ts) enforce; RLS is the backstop.
--   * NO credentials, NO secrets, NO hardware fingerprints anywhere.
--   * Status vocabularies mirror src/distributed/{job,shard}.hpp exactly;
--     the CHECK constraints pin what the database can prove (the C++
--     transition tables remain the executable specification).
--   * Honest records: failures require a reason; unknown values are NULL;
--     metadata only — result DATA is never stored in the control plane.
-- ============================================================================

-- ----------------------------------------------------------------------------
-- distributed_jobs — the distributed execution record of one platform job.
--
-- One row per job (1:1 with public.jobs): a distributed submission is a
-- platform job with a multi-device plan. requested_shard_count is the
-- caller's explicit single-device (1) vs multi-device (>1) choice; the
-- status vocabulary is the DISTRIBUTED lifecycle (finer than the platform
-- job status: planning/scheduled exist here), mirrored by the C++ layer's
-- map_to_platform_job_status when the two records must agree.
-- ----------------------------------------------------------------------------

create table public.distributed_jobs (
  job_id text primary key references public.jobs (id) on delete cascade,
  requested_shard_count integer not null
    check (requested_shard_count between 1 and 2147483647),
  status text not null default 'queued'
    check (status in ('queued', 'planning', 'scheduled', 'running',
                      'completed', 'failed', 'cancelled')),
  error text not null default '',
  created_at timestamptz not null default now(),
  completed_at timestamptz,
  updated_at timestamptz not null default now(),

  -- completed_at is set exactly when a terminal status is reached.
  check (
    (status in ('completed', 'failed', 'cancelled')) = (completed_at is not null)
  ),
  -- failures are never hidden: a failed job carries its reason.
  check (status <> 'failed' or error <> '')
);

comment on table public.distributed_jobs is
  'Distributed execution record of one platform job. Metadata only; visibility derives from the owning job (RLS).';

create index distributed_jobs_status_idx on public.distributed_jobs (status);

create trigger distributed_jobs_set_updated_at
  before update on public.distributed_jobs
  for each row execute function public.vortyx_set_updated_at();

-- ----------------------------------------------------------------------------
-- distributed_shards — the shard table of one distributed job.
--
-- Shard ids are DERIVED on the client ("<job_id>-s<index>", the same
-- charset rules as every platform id). The element range [element_begin,
-- element_end) is the shard's slice of the job's data-parallel domain —
-- placement metadata, never payload. device_id is the CURRENT target
-- (NULL while unplaced); it intentionally has NO foreign key: a device
-- agent may report an assignment before/after the device row changes, and
-- the C++ registry remains the authority on device existence.
-- ----------------------------------------------------------------------------

create table public.distributed_shards (
  shard_id text primary key
    check (char_length(shard_id) between 1 and 128 and shard_id ~ '^[A-Za-z0-9._-]+$'),
  job_id text not null references public.distributed_jobs (job_id) on delete cascade,
  shard_index integer not null check (shard_index >= 0),
  state text not null default 'pending'
    check (state in ('pending', 'assigned', 'running', 'completed',
                     'failed', 'retrying', 'cancelled')),
  element_begin bigint not null check (element_begin >= 0),
  element_end bigint not null check (element_end > element_begin),
  device_id text,
  attempt integer not null default 0 check (attempt >= 0),
  retry_count integer not null default 0 check (retry_count >= 0),
  failure_code text not null default '',
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),

  -- A shard belongs to exactly one index per job; the range must sit
  -- inside a non-negative domain (the exact-coverage invariant is the
  -- planner's job and is pinned by its own tests).
  unique (job_id, shard_index)
);

comment on table public.distributed_shards is
  'One shard of a distributed job: deterministic range + placement/lifecycle metadata. No payload.';

create index distributed_shards_job_idx on public.distributed_shards (job_id, shard_index);
create index distributed_shards_device_idx on public.distributed_shards (device_id)
  where device_id is not null;

create trigger distributed_shards_set_updated_at
  before update on public.distributed_shards
  for each row execute function public.vortyx_set_updated_at();

-- ----------------------------------------------------------------------------
-- device_views — the cluster view the API reports (GET /api/cluster).
--
-- A device agent's scheduling report: the DISTRIBUTED state/health
-- vocabulary (finer than the platform devices.status online/offline,
-- which stays untouched), the self-reported capacity, and the currently
-- allocated resources. Owner-scoped directly (unlike shard visibility,
-- which derives from jobs) — a device view is a first-class owner record
-- exactly like public.devices, whose id it mirrors.
-- ----------------------------------------------------------------------------

create table public.device_views (
  device_id text primary key
    check (char_length(device_id) between 1 and 128 and device_id ~ '^[A-Za-z0-9._-]+$'),
  owner_user_id uuid not null references auth.users (id) on delete cascade,
  state text not null default 'registering'
    check (state in ('registering', 'ready', 'busy', 'draining', 'offline', 'failed')),
  health text not null default 'unknown'
    check (health in ('healthy', 'unhealthy', 'unknown')),
  capacity jsonb not null default '{}'::jsonb,
  allocated jsonb not null default '{}'::jsonb,
  backends jsonb not null default '[]'::jsonb,
  running_shards integer not null default 0 check (running_shards >= 0),
  last_heartbeat_ms bigint,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

comment on table public.device_views is
  'Device-agent scheduling view of one device. Owner-scoped; capacity/allocation are self-reported configuration, never measurements.';

create index device_views_owner_idx on public.device_views (owner_user_id);

create trigger device_views_set_updated_at
  before update on public.device_views
  for each row execute function public.vortyx_set_updated_at();

-- ============================================================================
-- Row Level Security — derived ownership, no new rules.
-- ============================================================================

alter table public.distributed_jobs enable row level security;
alter table public.distributed_shards enable row level security;
alter table public.device_views enable row level security;

-- distributed_jobs -----------------------------------------------------------
-- Insert additionally proves the platform job exists AND belongs to the
-- caller (the same INSERT ... WITH CHECK shape as jobs_insert_own).

create policy "distributed_jobs_insert_own"
  on public.distributed_jobs for insert
  with check (
    exists (
      select 1 from public.jobs j
      where j.id = job_id
        and j.owner_user_id = auth.uid()
    )
  );

create policy "distributed_jobs_select_own"
  on public.distributed_jobs for select
  using (
    exists (
      select 1 from public.jobs j
      where j.id = job_id
        and j.owner_user_id = auth.uid()
    )
  );

create policy "distributed_jobs_update_own"
  on public.distributed_jobs for update
  using (
    exists (
      select 1 from public.jobs j
      where j.id = job_id
        and j.owner_user_id = auth.uid()
    )
  )
  with check (
    exists (
      select 1 from public.jobs j
      where j.id = job_id
        and j.owner_user_id = auth.uid()
    )
  );

-- distributed_shards ----------------------------------------------------------
-- Visibility derives entirely from the OWNING JOB (the job_results
-- pattern — no denormalized owner column, nothing to drift).

create policy "distributed_shards_insert_own"
  on public.distributed_shards for insert
  with check (
    exists (
      select 1 from public.distributed_jobs dj
      join public.jobs j on j.id = dj.job_id
      where dj.job_id = distributed_shards.job_id
        and j.owner_user_id = auth.uid()
    )
  );

create policy "distributed_shards_select_own"
  on public.distributed_shards for select
  using (
    exists (
      select 1 from public.distributed_jobs dj
      join public.jobs j on j.id = dj.job_id
      where dj.job_id = distributed_shards.job_id
        and j.owner_user_id = auth.uid()
    )
  );

create policy "distributed_shards_update_own"
  on public.distributed_shards for update
  using (
    exists (
      select 1 from public.distributed_jobs dj
      join public.jobs j on j.id = dj.job_id
      where dj.job_id = distributed_shards.job_id
        and j.owner_user_id = auth.uid()
    )
  )
  with check (
    exists (
      select 1 from public.distributed_jobs dj
      join public.jobs j on j.id = dj.job_id
      where dj.job_id = distributed_shards.job_id
        and j.owner_user_id = auth.uid()
    )
  );

-- device_views ----------------------------------------------------------------
-- First-class owner records (the devices pattern).

create policy "device_views_insert_own"
  on public.device_views for insert
  with check (auth.uid() = owner_user_id);

create policy "device_views_select_own"
  on public.device_views for select
  using (auth.uid() = owner_user_id);

create policy "device_views_update_own"
  on public.device_views for update
  using (auth.uid() = owner_user_id)
  with check (auth.uid() = owner_user_id);
