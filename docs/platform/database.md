# Database Schema & RLS (Phase 11)

Migration: `platform/supabase/migrations/0001_platform_init.sql` — real
PostgreSQL, applied in lexicographic order (Supabase CLI `supabase db push`
or the SQL editor). **No migration is applied in Phase 11**: creating the
actual Supabase project is the project owner's post-Phase-11 step (see
`deployment-checklist.md`).

## Tables

### profiles

| Column | Type | Notes |
|--------|------|-------|
| id | uuid PK | `references auth.users(id) on delete cascade` |
| display_name | text not null default '' | |
| created_at / updated_at | timestamptz | `now()` defaults; trigger-maintained |

Application profile ONLY. Authentication lives in Supabase Auth
(`auth.users`); there is **no password column anywhere** — no credential
system is created, ever. A `security definer` trigger
(`vortyx_handle_new_user`) creates the profile row on signup, idempotently.

### devices

| Column | Type | Notes |
|--------|------|-------|
| id | text PK | client-generated UUID v4 string; `CHECK` length 1..128 + `^[A-Za-z0-9._-]+$` |
| owner_user_id | uuid NOT NULL | `references auth.users(id) on delete cascade` |
| display_name | text | caller-chosen label, never an identity |
| protocol_version / software_version | text NOT NULL | self-reported |
| operating_system / architecture | text | '' = not reported |
| capabilities | jsonb default '{}' | documented shape below; unknown keys are opaque to readers |
| status | text | `online` / `offline` (CHECK) |
| last_seen_at | timestamptz | server-stamped |
| created_at / updated_at | timestamptz | trigger-maintained |

Index: `devices_owner_idx (owner_user_id)`.

`capabilities` shape (Phase 11): `{"backends": ["cpu"|"vulkan", …],
"operations": ["vector_add"|…], …}`. Phase 12+ may add keys (memory,
connectivity, battery, load); Phase 11 adds no metric it cannot measure.

**No hardware fingerprint, MAC address or serial number is stored anywhere.**

### jobs

| Column | Type | Notes |
|--------|------|-------|
| id | text PK | client-generated UUID v4 string; same CHECK as devices |
| owner_user_id | uuid NOT NULL | FK → auth.users, cascade |
| submitted_by_device_id | text | FK → devices(id) `on delete set null` |
| operation | text | CHECK in `vector_add / vector_multiply / vector_scale` |
| element_count | bigint | CHECK `0 < n <= 2147483647` (the int32 engine domain) |
| requested_backend | text | CHECK in `'' / cpu / vulkan`; recorded, never remapped |
| priority | smallint default 0 | **RESERVED** — nothing reads it in Phase 11 |
| protocol_version | text NOT NULL | |
| status | text | CHECK in `queued / running / completed / failed / cancelled` |
| error | text | failure/cancel reason |
| created_at / started_at / completed_at / updated_at | timestamptz | |

Integrity CHECKs (the database-level pin of the honesty rules):

- `(status IN ('completed','failed','cancelled')) = (completed_at IS NOT NULL)`
- `status <> 'failed' OR error <> ''` — a failed job always carries a reason.

Indexes: `jobs_owner_created_idx (owner_user_id, created_at DESC)`,
`jobs_owner_status_idx (owner_user_id, status)`.

The status transition TABLE (queued→running→…, terminal is final) is
enforced by the API/store layer and pinned by tests on both sides;
PostgreSQL CHECK constraints pin the invariants the database can prove.

### job_results

| Column | Type | Notes |
|--------|------|-------|
| job_id | text PK | FK → jobs(id) `on delete cascade` — one row per job at most |
| status | text | CHECK in `completed / failed` |
| backend | text | CHECK in `'' / cpu / vulkan` |
| error | text | required when failed (CHECK) |
| result_element_count | bigint | nullable — metadata only, no payload storage |
| created_at | timestamptz | |

Result PAYLOADS (the actual int32 arrays) are deliberately not stored in
Phase 11 — the envelope is metadata only; payload storage/streaming is a
later phase with a real executor.

## UUID / id strategy (explicit)

- Profile and owner ids: provider UUIDs (`auth.users.id`).
- Device and job ids: CLIENT-generated UUID-v4 strings. Stored as `text`
  with a CHECK constraint (charset + length) so the control-plane contract
  stays provider-neutral; the API validates the same rule, and generated
  ids satisfy it by construction. Job ids double as the submission
  idempotency key.

## Row Level Security

RLS is ENABLED on all four tables; the policies implement exactly the rule
the C++ store and the API layer apply (`auth.uid() = owner_user_id`):

| Table | Policy name | Commands | Rule |
|-------|-------------|----------|------|
| profiles | `profiles_select_own` | SELECT | `auth.uid() = id` |
| profiles | `profiles_update_own` | UPDATE | `auth.uid() = id` (with check) |
| devices | `devices_insert_own` | INSERT | `auth.uid() = owner_user_id` |
| devices | `devices_select_own` | SELECT | `auth.uid() = owner_user_id` |
| devices | `devices_update_own` | UPDATE | `auth.uid() = owner_user_id` (with check) |
| jobs | `jobs_insert_own` | INSERT | owner match **and** `submitted_by_device_id` (when set) references an own device (EXISTS subquery) |
| jobs | `jobs_select_own` | SELECT | `auth.uid() = owner_user_id` |
| jobs | `jobs_update_own` | UPDATE | `auth.uid() = owner_user_id` (with check) |
| job_results | `job_results_insert_own` | INSERT | EXISTS owning job |
| job_results | `job_results_select_own` | SELECT | EXISTS owning job |

Properties worth noting:

- **No anon policies**: with RLS on, unauthenticated requests see nothing.
- **No DELETE policies**: rows leave only via `auth.users` cascades
  (account deletion) or privileged maintenance access.
- **job_results derives visibility from the owning job** (no denormalized
  owner column — nothing to drift).
- The service-role role bypasses RLS by design; no Phase 11 endpoint uses
  it (see security.md).

## Verifying the schema after deployment

1. `supabase db push` (or paste the migration into the SQL editor).
2. Confirm `\dt public.*` shows profiles/devices/jobs/job_results.
3. Confirm RLS is on: `select relname, relrowsecurity from pg_class where
   relnamespace = 'public'::regnamespace;`
4. Register two test users; create a device with user A; verify user B's
   `select` on `devices` returns zero rows (RLS in action).

---

## Phase 15 — the service control plane (migration 0003_service_init.sql)

`0003_service_init.sql` is ADDITIVE: it alters nothing from 0001/0002 and
adds the SERVICE surface's tables. Apply order stays lexicographic:
0001 → 0002 → 0003.

### Tables and their source-of-truth role

| Table | Source of truth for | Notes |
|---|---|---|
| `projects` | projects | owner = creator; `active`/`archived` |
| `project_members` | membership + roles | exactly one `owner` row per project (trigger-enforced) |
| `service_jobs` | the service job lifecycle AND the durable queue/lease state | `job_id` PK = the idempotency key; `claimed_by`/`claim_expires_at_ms`/`attempt` live here (no separate queue table) |
| `quota_policies` | per-project quota POLICY | usage is NOT stored — it derives from in-flight `service_jobs` |
| `artifact_metadata` | artifact METADATA | no payload column exists; per-project count bounded (trigger) |
| `audit_events` | the audit trail | metadata only; signup audited by a trigger |
| `rate_limit_windows` | centralized fixed-window counters | touched only by `vortyx_rate_limit_take` |

### Database-enforced invariants (the API layer ALSO enforces these — defense in depth)

| Invariant | Mechanism |
|---|---|
| Quota policy (concurrent jobs / shards / memory) | `vortyx_enforce_service_quota()` BEFORE INSERT trigger — closes the check-then-insert race for concurrent submissions |
| Single-owner invariant | `vortyx_enforce_single_owner()` trigger on `project_members` — only the creator row may carry `owner`; grants/demotions refused |
| Artifact metadata bound | `vortyx_enforce_artifact_capacity()` trigger — 256 per project |
| Atomic worker claim | `vortyx_worker_claim()` RPC — reconcile stale leases + `FOR UPDATE SKIP LOCKED` selection (two workers can never claim one job) |
| Idempotent result commit | `vortyx_worker_complete()` RPC — duplicate reports return the existing terminal state; foreign/lost claims refused |
| Centralized rate limiting | `vortyx_rate_limit_take()` RPC — one fixed window per key in the database (per-instance memory would be meaningless across serverless instances) |
| Stale-lease recovery | `vortyx_worker_reconcile()` — running + expired lease → `failed("worker_lease_expired")` |

### RLS policy map (0003)

| Table | Policy | Mode | Rule |
|---|---|---|---|
| projects | `projects_select_member` | SELECT | project-membership EXISTS |
| projects | `projects_insert_own` / `projects_update_own` | INSERT/UPDATE | `auth.uid() = owner_user_id` |
| project_members | `project_members_select_member` | SELECT | membership EXISTS |
| project_members | `project_members_insert_member` / `_delete_member` | INSERT/DELETE | membership EXISTS (the single-owner trigger constrains the shapes) |
| service_jobs | `service_jobs_select_member` | SELECT | membership EXISTS (foreign projects are INVISIBLE — the RLS mirror of the API's 404) |
| service_jobs | `service_jobs_insert_member` | INSERT | Member+ role and `auth.uid() = submitted_by` |
| service_jobs | `service_jobs_update_member` | UPDATE | membership EXISTS |
| quota_policies | `quota_policies_select_member` / `_write_admin` | SELECT / ALL | membership / Admin+ role |
| artifact_metadata | `artifact_metadata_select_member` / `_insert_member` / `_delete_member` | SELECT/INSERT/DELETE | membership; insert as creator; delete as creator or Admin+ |
| audit_events | `audit_events_select_own` | SELECT | `auth.uid() = actor_user_id` |
| audit_events | `audit_events_select_project_admin` | SELECT | project Admin/Owner membership |
| audit_events | `audit_events_insert_any_authenticated` | INSERT | the API stamps the actor from the VERIFIED subject |
| rate_limit_windows | (none — RLS enabled with no policies) | — | deny-all for anon/authenticated; only the security-definer RPC touches it |

The service-role client (used ONLY by the worker-protocol paths and
reconciliation) bypasses RLS by design; the API never exposes it to a
browser.
