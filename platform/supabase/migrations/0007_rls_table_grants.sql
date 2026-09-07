-- ============================================================================
-- Vortyx GPU — Table Privilege Grants (Phase 17, v0.17.0)
-- 0007_rls_table_grants.sql
--
-- Fixes the second latent production defect the live E2E surfaced (the one
-- right after 0006 fixed the policy recursion):
--
--   ERROR 42501: permission denied for table "projects"
--   hint: GRANT SELECT ON public.projects TO authenticated;
--
-- RLS POLICIES are not privileges. 0001..0003 relied on the Supabase
-- platform's DEFAULT privileges to grant anon/authenticated/service_role
-- access to new public tables — a platform-side default that does not apply
-- to tables created through the Management API SQL endpoint (the migrations
-- were applied there, so the defaults never fired and EVERY user-scoped
-- statement failed with 42501 before RLS was even evaluated).
--
-- The fix is the explicit, deterministic grant set — the documented
-- privilege surface, derived from the policies and the API adapter:
--   * authenticated gets exactly the operations a policy allows
--     (a table privilege without a policy still yields zero rows/403 —
--     RLS remains the enforcement point),
--   * anon gets NOTHING (there are deliberately no anon policies),
--   * service_role gets ALL on every table (server-only; RLS bypass is
--     not a grant substitute),
--   * rate_limit_windows stays UNGRANTED to anon/authenticated — the
--     centralized counters are touched ONLY by the definer function
--     vortyx_rate_limit_take (0004), the documented deny-all-by-construction.
--
-- Idempotent: GRANT statements are.
-- ============================================================================

-- 0001 tables (the platform control plane) -----------------------------------

grant select, update on public.profiles to authenticated;
grant select, insert, update on public.devices to authenticated;
grant select, insert, update on public.jobs to authenticated;
grant select, insert on public.job_results to authenticated;

-- 0002 tables (the distributed surface) ---------------------------------------

grant select, insert, update on public.distributed_jobs to authenticated;
grant select, insert, update on public.distributed_shards to authenticated;
grant select, insert, update on public.device_views to authenticated;

-- 0003 tables (the service control plane) --------------------------------------

grant select, insert, update on public.projects to authenticated;
grant select, insert, delete on public.project_members to authenticated;
grant select, insert, update on public.service_jobs to authenticated;
grant select, insert, update, delete on public.quota_policies to authenticated;
grant select, insert, delete on public.artifact_metadata to authenticated;
grant select, insert on public.audit_events to authenticated;

-- service_role: full table access (server-only role; used by the definer
-- RPCs' owner context and any future server-side maintenance path).

grant all on public.profiles to service_role;
grant all on public.devices to service_role;
grant all on public.jobs to service_role;
grant all on public.job_results to service_role;
grant all on public.distributed_jobs to service_role;
grant all on public.distributed_shards to service_role;
grant all on public.device_views to service_role;
grant all on public.projects to service_role;
grant all on public.project_members to service_role;
grant all on public.service_jobs to service_role;
grant all on public.quota_policies to service_role;
grant all on public.artifact_metadata to service_role;
grant all on public.audit_events to service_role;
grant all on public.rate_limit_windows to service_role;

-- anon: NOTHING (no grants — the no-anon-policies deny-all stands).
