-- ============================================================================
-- Vortyx GPU — RLS Policy Recursion Fix (Phase 17, v0.17.0)
-- 0006_rls_policy_recursion_fix.sql
--
-- Fixes a REAL defect in 0003 that made every member-visible path fail for
-- ordinary users the moment RLS was evaluated by a non-owner role:
--
--   ERROR 42P17: infinite recursion detected in policy for relation
--                "project_members"
--
-- 0003's project_members SELECT policy queried project_members ITSELF
-- (exists ... from project_members m where ...), so evaluating it triggered
-- it again — Postgres aborts at depth. Every policy that queries
-- project_members (projects_select_member, the service_jobs family,
-- quota_policies, artifact_metadata, audit_events) therefore recursed too,
-- and even POST /api/projects failed (its INSERT ... RETURNING evaluates
-- projects_select_member). The CI integration suite never caught this
-- because it exercises the database as the TABLE OWNER, and owners do not
-- evaluate RLS — exactly the false-green shape Phase 17 exists to close
-- (caught by the live production E2E on the first run).
--
-- THE FIX: a SECURITY DEFINER membership probe. Policy subqueries call the
-- definer function, which reads project_members as its owner (RLS does not
-- apply to the owner — no FORCE RLS anywhere in this schema), breaking the
-- recursion chain. Semantics are UNCHANGED:
--   * members see the member rows of projects they belong to (+ own row),
--   * members insert/delete within projects they belong to (the API's role
--     table remains the real gatekeeper; the trigger still enforces the
--     single-owner invariant),
--   * every other table's policies keep their exact predicates.
--
-- ADDITIVE: no table, column or object from 0001..0005 is dropped; three
-- policies are re-created under their SAME names with recursion-free
-- bodies. Idempotent: drop-if-exists before create; create-or-replace for
-- the helper.
--
-- Applied to production 2026-09-07 (the live E2E reproduction that found
-- this is scripts/production_e2e_test.mjs).
-- ============================================================================

-- ----------------------------------------------------------------------------
-- 1. The recursion-breaking membership probe.
-- ----------------------------------------------------------------------------

create or replace function public.vortyx_is_project_member(p_project_id text)
returns boolean
language sql
stable
security definer
set search_path = public
as $$
  select exists (
    select 1
      from public.project_members m
     where m.project_id = p_project_id
       and m.user_id = auth.uid()
  );
$$;

-- Policy subqueries run with the CALLER's privileges, so every role that
-- can evaluate a membership policy needs EXECUTE (authenticated users via
-- the API; service_role defensively). anon and PUBLIC never evaluate these
-- policies (deny-all via no-policy), so they get nothing.
revoke execute on function public.vortyx_is_project_member(text) from public, anon;
grant execute on function public.vortyx_is_project_member(text) to authenticated, service_role;

-- ----------------------------------------------------------------------------
-- 2. project_members' own policies — the recursive ones — re-created with
--    recursion-free bodies under the SAME names.
-- ----------------------------------------------------------------------------

drop policy if exists "project_members_select_member"
  on public.project_members;
create policy "project_members_select_member"
  on public.project_members for select
  using (
    user_id = auth.uid()
    or public.vortyx_is_project_member(project_members.project_id)
  );

drop policy if exists "project_members_insert_member"
  on public.project_members;
create policy "project_members_insert_member"
  on public.project_members for insert
  with check (
    public.vortyx_is_project_member(project_id)
  );

drop policy if exists "project_members_delete_member"
  on public.project_members;
create policy "project_members_delete_member"
  on public.project_members for delete
  using (
    project_members.role <> 'owner'
    and public.vortyx_is_project_member(project_members.project_id)
  );
