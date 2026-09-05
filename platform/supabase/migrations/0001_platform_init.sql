-- ============================================================================
-- Vortyx GPU — Platform Control Plane (Phase 11, v0.11.0)
-- 0001_platform_init.sql
--
-- Initial schema for the Supabase-backed control plane. This migration is
-- DEPLOYED BY THE PROJECT OWNER after Phase 11 (creating the actual Supabase
-- project is deliberately deferred until after Phase 11 implementation).
-- Apply with the Supabase CLI (`supabase db push`) or the Supabase SQL
-- editor; migrations are applied in lexicographic file-name order.
--
-- Design rules (mirrored by the C++ InMemoryPlatformStore and the Vercel API
-- layer — the three must agree, tests pin it):
--   * Ownership: a user sees exactly their own rows. Enforced HERE by RLS,
--     and by the API layer — RLS holds even if a server is misconfigured.
--   * No credentials anywhere: authentication is Supabase Auth (auth.users).
--     There is NO password column in this schema, ever.
--   * Honest records: failures require a reason; terminal states are final;
--     unknown values are NULL, never fabricated defaults.
--   * Ids: profile/device owners are provider UUIDs (auth.users.id). Device
--     and job ids are CLIENT-GENERATED UUID-v4 strings (the API validates
--     ^[A-Za-z0-9._-]+$ and length 1..128; generated ids match it). They are
--     stored as text so the control-plane contract stays provider-neutral;
--     the format guarantee is the API's job, not the database's.
-- ============================================================================

-- ----------------------------------------------------------------------------
-- profiles — application profile ONLY.
--
-- Authentication lives in Supabase Auth. This table stores display data the
-- application owns; it NEVER stores passwords, tokens or secrets. One row
-- per auth.users entry, created automatically on signup.
-- ----------------------------------------------------------------------------
create table public.profiles (
  id uuid primary key references auth.users (id) on delete cascade,
  display_name text not null default '',
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

comment on table public.profiles is
  'Application profile for one authenticated user. No credentials are stored here.';

-- ----------------------------------------------------------------------------
-- Shared helpers
-- ----------------------------------------------------------------------------

-- Standard updated_at maintenance (applied by per-table triggers below).
create or replace function public.vortyx_set_updated_at()
returns trigger
language plpgsql
as $$
begin
  new.updated_at = now();
  return new;
end;
$$;

-- Creates a profile row for every new authenticated user. SECURITY DEFINER
-- so the insert runs as the table owner (RLS does not apply to the owner);
-- idempotent via ON CONFLICT.
create or replace function public.vortyx_handle_new_user()
returns trigger
language plpgsql
security definer set search_path = public
as $$
begin
  insert into public.profiles (id) values (new.id)
  on conflict (id) do nothing;
  return new;
end;
$$;

create trigger on_auth_user_created
  after insert on auth.users
  for each row execute function public.vortyx_handle_new_user();

-- ----------------------------------------------------------------------------
-- devices — one registered Vortyx node/installation per row.
--
-- capabilities is a self-reported JSON object; Phase 11 defines its shape as
--   { "backends": ["cpu" | "vulkan", ...],       -- may be absent/empty
--     "operations": ["vector_add" | "vector_multiply" | "vector_scale", ...] }
-- Additional keys may appear later (Phase 12+: memory, connectivity, ...);
-- readers must treat unknown keys as opaque. NO hardware fingerprint, NO
-- MAC address, NO serial number is ever stored.
-- ----------------------------------------------------------------------------
create table public.devices (
  id text primary key
    check (char_length(id) between 1 and 128 and id ~ '^[A-Za-z0-9._-]+$'),
  owner_user_id uuid not null references auth.users (id) on delete cascade,
  display_name text not null default '',
  protocol_version text not null,
  software_version text not null,
  operating_system text not null default '',
  architecture text not null default '',
  capabilities jsonb not null default '{}'::jsonb,
  status text not null default 'offline'
    check (status in ('online', 'offline')),
  last_seen_at timestamptz,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

comment on table public.devices is
  'One registered Vortyx node. Owner-scoped: visible only to its owner (RLS).';

create index devices_owner_idx on public.devices (owner_user_id);

create trigger devices_set_updated_at
  before update on public.devices
  for each row execute function public.vortyx_set_updated_at();

-- ----------------------------------------------------------------------------
-- jobs — the control-plane record of one submitted compute job.
--
-- Status vocabulary and transitions (enforced by the API layer; the checks
-- below pin the invariants the database can prove):
--   queued -> running -> completed | failed | cancelled
--   queued -> cancelled
--   terminal states are final.
-- priority is a RESERVED transport field: it carries no scheduling semantics
-- in Phase 11 (nothing in Vortyx reads it). Stored so the wire contract will
-- not need a breaking change if a later phase defines real semantics.
-- ----------------------------------------------------------------------------
create table public.jobs (
  id text primary key
    check (char_length(id) between 1 and 128 and id ~ '^[A-Za-z0-9._-]+$'),
  owner_user_id uuid not null references auth.users (id) on delete cascade,
  submitted_by_device_id text references public.devices (id) on delete set null,
  operation text not null
    check (operation in ('vector_add', 'vector_multiply', 'vector_scale')),
  element_count bigint not null
    check (element_count > 0 and element_count <= 2147483647),
  requested_backend text not null default ''
    check (requested_backend in ('', 'cpu', 'vulkan')),
  priority smallint not null default 0,
  protocol_version text not null,
  status text not null default 'queued'
    check (status in ('queued', 'running', 'completed', 'failed', 'cancelled')),
  error text not null default '',
  created_at timestamptz not null default now(),
  started_at timestamptz,
  completed_at timestamptz,
  updated_at timestamptz not null default now(),

  -- completed_at is set exactly when a terminal status is reached.
  check (
    (status in ('completed', 'failed', 'cancelled')) = (completed_at is not null)
  ),
  -- failures are never hidden: a failed job carries its reason.
  check (status <> 'failed' or error <> '')
);

comment on table public.jobs is
  'One remotely-managed compute job. Metadata only: job payloads are never stored in Phase 11.';

create index jobs_owner_created_idx on public.jobs (owner_user_id, created_at desc);
create index jobs_owner_status_idx on public.jobs (owner_user_id, status);

create trigger jobs_set_updated_at
  before update on public.jobs
  for each row execute function public.vortyx_set_updated_at();

-- ----------------------------------------------------------------------------
-- job_results — the recorded OUTCOME of a job (metadata only; result payloads
-- are not stored in Phase 11). One row per job at most (job_id is the PK).
-- ----------------------------------------------------------------------------
create table public.job_results (
  job_id text primary key references public.jobs (id) on delete cascade,
  status text not null check (status in ('completed', 'failed')),
  backend text not null default '' check (backend in ('', 'cpu', 'vulkan')),
  error text not null default '',
  result_element_count bigint,
  created_at timestamptz not null default now(),

  check (status <> 'failed' or error <> '')
);

comment on table public.job_results is
  'Execution outcome metadata of one job. Visibility derives from the owning job (RLS).';

-- ============================================================================
-- Row Level Security — the database-level backstop of the ownership rule.
--
-- The API layer resolves the caller's identity from the Supabase Auth JWT
-- and applies the same rule in code; RLS guarantees the rule even if the
-- API layer is ever wrong. There are deliberately NO anon policies: with RLS
-- enabled and none granted, unauthenticated requests see nothing. There are
-- NO delete policies: rows are removed only by cascades from auth.users
-- (account deletion) or by privileged maintenance access.
-- ============================================================================

alter table public.profiles enable row level security;
alter table public.devices enable row level security;
alter table public.jobs enable row level security;
alter table public.job_results enable row level security;

-- profiles -------------------------------------------------------------------

create policy "profiles_select_own"
  on public.profiles for select
  using (auth.uid() = id);

create policy "profiles_update_own"
  on public.profiles for update
  using (auth.uid() = id)
  with check (auth.uid() = id);

-- devices --------------------------------------------------------------------

create policy "devices_insert_own"
  on public.devices for insert
  with check (auth.uid() = owner_user_id);

create policy "devices_select_own"
  on public.devices for select
  using (auth.uid() = owner_user_id);

create policy "devices_update_own"
  on public.devices for update
  using (auth.uid() = owner_user_id)
  with check (auth.uid() = owner_user_id);

-- jobs -----------------------------------------------------------------------
-- Insert additionally proves that a referenced submitting device (when
-- given) exists AND belongs to the same owner; the FK alone would not stop
-- cross-tenant device references.

create policy "jobs_insert_own"
  on public.jobs for insert
  with check (
    auth.uid() = owner_user_id
    and (
      submitted_by_device_id is null
      or exists (
        select 1 from public.devices d
        where d.id = jobs.submitted_by_device_id
          and d.owner_user_id = auth.uid()
      )
    )
  );

create policy "jobs_select_own"
  on public.jobs for select
  using (auth.uid() = owner_user_id);

create policy "jobs_update_own"
  on public.jobs for update
  using (auth.uid() = owner_user_id)
  with check (auth.uid() = owner_user_id);

-- job_results ----------------------------------------------------------------
-- Access derives from the OWNING JOB's ownership, not a stored owner column
-- (no denormalization, nothing to drift).

create policy "job_results_insert_own"
  on public.job_results for insert
  with check (
    exists (
      select 1 from public.jobs j
      where j.id = job_id
        and j.owner_user_id = auth.uid()
    )
  );

create policy "job_results_select_own"
  on public.job_results for select
  using (
    exists (
      select 1 from public.jobs j
      where j.id = job_id
        and j.owner_user_id = auth.uid()
    )
  );
