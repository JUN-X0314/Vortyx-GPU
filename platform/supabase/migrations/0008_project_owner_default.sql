-- ============================================================================
-- Vortyx GPU — Project Owner Default (Phase 17, v0.17.0)
-- 0008_project_owner_default.sql
--
-- Fixes the third latent production defect surfaced by the live E2E:
-- POST /api/projects failed with
--
--   42501: new row violates row-level security policy for table "projects"
--
-- The service adapter inserts projects with { name } only — the OWNER is
-- the authenticated caller BY DEFINITION (the API never accepts a
-- client-claimed owner), but 0003 declared owner_user_id with NO default,
-- so the inserted row carried owner_user_id = NULL and the
-- projects_insert_own WITH CHECK (auth.uid() = owner_user_id) could not
-- pass. The Phase 11 adapter supplies owner_user_id explicitly on
-- devices/jobs; the service path simply never had the default.
--
-- THE FIX: default the column to auth.uid(). Safety is unchanged and
-- explicit: the WITH CHECK still requires owner_user_id = auth.uid(), so
-- no client can ever create a project naming a DIFFERENT owner — the
-- default only fills the value the API legitimately omits. The
-- vortyx_handle_new_project trigger then creates the creator's owner
-- member row exactly as before.
--
-- Idempotent: SET DEFAULT is.
-- ============================================================================

alter table public.projects
  alter column owner_user_id set default auth.uid();
