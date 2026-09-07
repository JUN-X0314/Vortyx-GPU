-- ============================================================================
-- Vortyx GPU — Runtime Security Lockdown (Phase 17, v0.17.0)
-- 0005_runtime_security_lockdown.sql
--
-- Closes the privilege-boundary holes the API's own tests could never see:
-- everything below is enforced by the DATABASE, exactly like 0004 hardened
-- concurrency. ADDITIVE: nothing from 0001..0004 is dropped, renamed or
-- re-specified except the one audit INSERT policy that was unsafe as
-- written (dropped and re-created under a new name; its semantics for the
-- API are identical — the API stamps the actor from the verified subject).
--
-- What changed, and WHY:
--
--   1. WORKER-PROTOCOL RPC EXECUTE LOCKDOWN. 0003's worker-protocol
--      functions (vortyx_worker_claim / _heartbeat / _complete / _reconcile)
--      are SECURITY DEFINER — they act with the table owner's privileges
--      over EVERY project's service_jobs. PostgreSQL grants EXECUTE to
--      PUBLIC by default, and Supabase's default privileges additionally
--      grant it to anon and authenticated — so until this migration ANY
--      authenticated user could call them DIRECTLY through PostgREST
--      (POST /rest/v1/rpc/vortyx_worker_claim), bypassing the API's
--      worker-token boundary entirely: claim any queued job, complete it
--      with a fabricated outcome, fail or reconcile arbitrary jobs. The
--      API calls these RPCs with the service-role client only (the worker's
--      bearer token is checked by the API BEFORE any database access), so
--      the locked grant set is: service_role only.
--
--   2. TRIGGER-ONLY DEFINER FUNCTIONS are not RPC surface. The remaining
--      SECURITY DEFINER functions (quota / artifact-capacity / terminal
--      immutability / single-owner enforcement, the auth/project signup
--      handlers) exist to run as triggers. Least privilege: no EXECUTE for
--      anon or authenticated (they were never part of the documented
--      contract; a PostgREST call on a trigger-returning function errors
--      anyway, and the revoke makes that refusal a policy, not luck).
--
--   3. AUDIT ACTOR BINDING. 0003's "audit_events_insert_any_authenticated"
--      policy checked only that the caller was authenticated — the ACTOR
--      column was unconstrained, so any authenticated user could insert an
--      audit event naming ARBITRARY actor_user_id (a forged trail). The
--      API's adapter stamps the actor from the VERIFIED subject on every
--      user-scoped insert, so binding the insert to the caller's own
--      identity is exactly the API's behavior — now enforced by RLS too.
--      (Service-role inserts bypass RLS and are unaffected.)
--
--   4. PLATFORM-SIDE HELPER (observed on hosted Supabase projects; NOT
--      created by any Vortyx migration): public.rls_auto_enable() is a
--      SECURITY DEFINER helper that enables RLS on newly created tables.
--      Ordinary API roles have no business calling it. The revoke is
--      CONDITIONAL (DO block) so this migration also applies verbatim to
--      a vanilla PostgreSQL 17 (the CI integration suite) where the
--      function does not exist.
--
-- Idempotency: REVOKE/GRANT are idempotent; DROP POLICY IF EXISTS +
-- CREATE (the create would fail on re-run only if the policy already
-- exists — drop-if-exists first makes the file safe to re-apply, matching
-- 0004's documented idempotency rule).
-- ============================================================================

-- ----------------------------------------------------------------------------
-- 1. The worker protocol belongs to the service role ONLY.
-- ----------------------------------------------------------------------------

revoke execute on function public.vortyx_worker_claim(text, bigint)
  from public, anon, authenticated;
revoke execute on function public.vortyx_worker_heartbeat(text, text, bigint)
  from public, anon, authenticated;
revoke execute on function public.vortyx_worker_complete(text, text, text, text, text, bigint, integer, integer, integer)
  from public, anon, authenticated;
revoke execute on function public.vortyx_worker_reconcile()
  from public, anon, authenticated;

grant execute on function public.vortyx_worker_claim(text, bigint) to service_role;
grant execute on function public.vortyx_worker_heartbeat(text, text, bigint) to service_role;
grant execute on function public.vortyx_worker_complete(text, text, text, text, text, bigint, integer, integer, integer) to service_role;
grant execute on function public.vortyx_worker_reconcile() to service_role;

-- ----------------------------------------------------------------------------
-- 2. Trigger-only definer functions: no direct EXECUTE for API roles.
-- ----------------------------------------------------------------------------

revoke execute on function public.vortyx_enforce_service_quota()
  from public, anon, authenticated;
revoke execute on function public.vortyx_enforce_artifact_capacity()
  from public, anon, authenticated;
revoke execute on function public.vortyx_enforce_terminal_immutable()
  from public, anon, authenticated;
revoke execute on function public.vortyx_enforce_single_owner()
  from public, anon, authenticated;
revoke execute on function public.vortyx_handle_new_user()
  from public, anon, authenticated;
revoke execute on function public.vortyx_handle_new_project()
  from public, anon, authenticated;
revoke execute on function public.vortyx_audit_new_user()
  from public, anon, authenticated;

-- ----------------------------------------------------------------------------
-- 3. Audit rows: the actor is the VERIFIED subject (never client-chosen).
-- ----------------------------------------------------------------------------

drop policy if exists "audit_events_insert_any_authenticated"
  on public.audit_events;

create policy "audit_events_insert_self"
  on public.audit_events for insert
  with check (auth.uid() = actor_user_id);

-- ----------------------------------------------------------------------------
-- 4. Platform-side rls_auto_enable(): not callable by ordinary API roles
--    (conditional — the function is not a Vortyx object and may not exist).
-- ----------------------------------------------------------------------------

do $$
begin
  if exists (
    select 1
      from pg_proc p
      join pg_namespace n on n.oid = p.pronamespace
     where n.nspname = 'public'
       and p.proname = 'rls_auto_enable'
  ) then
    execute 'revoke execute on function public.rls_auto_enable() from public, anon, authenticated';
  end if;
end $$;
