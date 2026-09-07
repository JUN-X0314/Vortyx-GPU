-- ============================================================================
-- Vortyx GPU — Project Visibility Timing Fix (Phase 17, v0.17.0)
-- 0009_project_visibility_timing.sql
--
-- Fixes the fourth latent production defect surfaced by the live E2E (the
-- create-project flow still failed after 0006/0007/0008):
--
--   42501: new row violates row-level security policy for table "projects"
--
-- Postgres evaluates INSERT ... RETURNING BEFORE AFTER-row triggers fire.
-- The service adapter inserts a project WITH RETURNING, and the creator's
-- owner member row is only created by the AFTER-insert trigger
-- (vortyx_handle_new_project). At RETURNING time the membership-based
-- projects_select_member policy therefore could not see the brand-new row
-- — the API's owner can never read back the project they just created.
--
-- THE FIX: the project's OWNER sees the project by OWNERSHIP — a fact that
-- is true the instant the row exists, independent of trigger timing — and
-- every other member through the (0006) definer probe. The visible row set
-- is UNCHANGED for every steady-state query (the owner always also has the
-- trigger-created owner member row); only the insert+RETURNING window
-- changes, from "broken" to "correct".
--
-- Idempotent: drop-if-exists before create.
-- ============================================================================

drop policy if exists "projects_select_member"
  on public.projects;
create policy "projects_select_member"
  on public.projects for select
  using (
    owner_user_id = auth.uid()
    or public.vortyx_is_project_member(projects.id)
  );
