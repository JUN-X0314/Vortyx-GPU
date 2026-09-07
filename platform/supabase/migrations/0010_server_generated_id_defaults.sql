-- ============================================================================
-- Vortyx GPU — Server-Generated Id Defaults (Phase 17, v0.17.0)
-- 0010_server_generated_id_defaults.sql
--
-- Fixes the fifth latent production defect surfaced by the live E2E:
--
--   23502: null value in column "id" of relation "projects" violates
--          not-null constraint
--
-- 0003's own comment says "Ids are server-generated UUIDs (the API
-- generates them, format-checked)" — but the API adapter inserts projects
-- with { name } only, and artifact registrations without artifact_id, so
-- the server side must generate them. The DEFAULT was simply never
-- declared: the memory store generates ids in-process, so every test
-- passed, and the real database path failed on first use.
--
-- THE FIX: declare the documented generation at the database (PG13+ core
-- gen_random_uuid, cast to the text id format the contract validates).
-- No client behavior changes; a client that DOES supply a valid id keeps
-- working (service_jobs.job_id remains the client-supplied idempotency key
-- by design — untouched).
--
-- Idempotent: SET DEFAULT is.
-- ============================================================================

alter table public.projects
  alter column id set default gen_random_uuid()::text;

alter table public.artifact_metadata
  alter column artifact_id set default gen_random_uuid()::text;
