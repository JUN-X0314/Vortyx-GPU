# Deployment Preparation Checklist (post-Phase-11, updated Phase 16.0.1)

Phase 11 ships the STRUCTURE and the CONTRACT. Phase 16.0.1 wires the
single-project production topology for real. This checklist records the
OBSERVED production state (dated), the two supported topologies, and the
smoke checks that decide readiness — a READY deployment banner alone
proves nothing.

## 1. Supabase project

**Observed state (verified live over the publishable REST API,
2026-09-07):** the production project answers `/auth/v1/health` → 200
(GoTrue healthy) with the committed publishable key (and 401 without any
key — the key is real and required), while EVERY application table
(`profiles`, `devices`, `jobs`, `job_results`, `distributed_jobs`,
`distributed_shards`, `device_views`, `projects`, `project_members`,
`service_jobs`, `quota_policies`, `artifact_metadata`, `audit_events`,
`rate_limit_windows`) returns 404 PGRST205 "Could not find the table in
the schema cache". Interpretation: the migrations have NOT been applied
to the production project yet. Applying them is the operator action below;
re-run the REST probe afterwards (an existing table yields 200 with an
RLS-scoped empty array, never 404).

1. Create the project (any region close to you).
2. Apply the migration:
   - `supabase db push` from `platform/supabase/` (CLI linked to the
     project), or paste `platform/supabase/migrations/0001_platform_init.sql`
     into the SQL editor and run it.
3. Verify (see the checklist at the end of `database.md`): tables exist, RLS
   is enabled on all four tables.
4. Collect values:
   - Project URL → `SUPABASE_URL`
   - anon/public key → `SUPABASE_ANON_KEY`
   - service_role key → `SUPABASE_SERVICE_ROLE_KEY` (server-only secret;
     Phase 11 endpoints do not use it — store it carefully anyway)

## 2. Server-side environment variables (the Vercel project)

Add (Production + Preview):
- `VORTYX_STORE=supabase`
- `SUPABASE_URL`, `SUPABASE_ANON_KEY`
- `SUPABASE_SERVICE_ROLE_KEY` — used ONLY for the worker-protocol paths
  and reconciliation; never exposed to any browser
- `VORTYX_WORKER_TOKEN`, `VORTYX_RECONCILE_TOKEN` (worker/cron secrets)
- `VORTYX_ALLOWED_ORIGIN` — leave EMPTY for the single-project topology
  (same-origin needs no CORS header); set it to the web origin ONLY in
  the two-project topology. Never `*`.

Missing variables are REPORTED, never faked: in production `/api/health`
MUST report `"store": "supabase"` with `"config_error": null`; a
`config_error` value is a genuine misconfiguration — fix the variable,
never hide the field.

## 3. The Vercel topologies

TWO correct topologies exist; the repository default is the first.

- **Single project (committed default, wired in Phase 16.0.1):** the
  root project serves the static console from `platform/web`
  (`framework: null`, `outputDirectory: platform/web` in the root
  `vercel.json`) AND `/api/*` through the root catch-all
  `api/[...route].ts`, which imports the REAL router
  (`platform/api/src/vercel.ts`) — one implementation, no duplicate
  surface. The root `package.json` declares `@supabase/supabase-js` so
  the catch-all's dynamic import chain resolves when
  `VORTYX_STORE=supabase`. The browser config
  (`platform/web/js/config.js`) is committed with publishable values
  only (verified live: the project's `/auth/v1/health` accepts exactly
  this key).
- **Two projects (still supported):** Root Directory `platform/api`
  (framework: Other; its own `api/` adapters and `vercel.json`) plus a
  static web project rooted at `platform/web`. In this topology set
  `VORTYX_ALLOWED_ORIGIN` to the web origin and put the API origin into
  `js/config.js`'s `apiBaseUrl` (the publishable config is committed;
  a private deployment forks and adjusts it).

**The failure mode to check for (observed on the production project):** a
deployment reported READY while the root URL returned 404 NOT_FOUND, with
`no files were prepared` in the build log. That combination means the
deployment produced ZERO static output files — the classic cause is a
root directory / output directory mismatch (e.g. the project rooted at the
repository root while `platform/web/vercel.json` sits one level down, so
it is never read, or an output directory that does not exist). A READY
status alone proves nothing; the URL must be fetched.

**The second observed failure mode (fixed by the 16.0.1 wiring):** the
root project served the static console but had NO serverless functions,
so every `/api/*` request (including `/api/health`) returned 404 while
the web pages loaded fine. The root `api/` catch-all closes it; verify
with the smoke checks below.

Note: deployment URLs may sit behind Vercel SSO deployment protection
(observed: every `*.vercel.app` URL of this project 302-redirects to
`vercel.com/sso-api`). The smoke checks must then be run from an
authenticated browser session (or temporarily via Vercel's "Protection
Bypass for Automation") — an SSO redirect page is NOT a passing smoke
check.

Smoke checks (all from the live production URL, in an authenticated
session if SSO protection is on):
- `GET /` → 200 with the real `index.html` HTML (never 404, never an SSO
  redirect page).
- `GET /assets/styles.css`, `/js/main.js`, `/js/config.js` → 200.
- `/js/config.js` body: `window.VORTYX_CONFIG` with EXACTLY
  `supabaseUrl` / `supabaseAnonKey` / `apiBaseUrl`, no placeholder values
  (the tests in `platform/web/test/production-config.test.mjs` pin the
  committed file; the deployed body must match it).
- `GET /api/health` → 200 `{status:"ok", store:"supabase",
  config_error:null}` — the /api/health 404 failure mode is closed and
  the store is the REAL Supabase adapter (memory here means the server
  env from section 2 is missing, never an acceptable production state).
- `GET /api/platform/info` → protocol `1`, the deployed software version.
- Client-side routes are HASH routes (`/#/projects`) — refresh cannot 404
  by construction; there is nothing server-side to rewrite. Verify by
  refreshing a deep hash route.
- Sign up / sign in through the console (real Supabase Auth), then load
  the dashboard — an authenticated read (projects list) through the real
  API and RLS.

## 4. Local mock parity check (before and after)

Run `npm run dev` (memory mode) and compare responses against the deployed
API for the same payloads — they must be identical except for timestamps.
Any difference means the two store implementations drifted, which the
shared contract forbids.

## 5. Security review before going public

- No server-only secret is committed anywhere (`git log -p | grep -i key`
  spot check). The committed `platform/web/js/config.js` carries
  publishable values only — the web test suite enforces the allowlist,
  the placeholder scan and the secret-marker scan.
- `SUPABASE_SERVICE_ROLE_KEY` exists only in Vercel's server-side env.
- RLS policies present and enabled (re-run the verification query from
  `database.md`).
- Two-account isolation test: user B cannot read/update user A's devices,
  jobs or results through the deployed API (must be 404).

## 6. Execution plane note

Deploying the API early for experimentation is fine, but know the plane
split: jobs submitted through the deployed API are ACCEPTED and QUEUED in
the control plane; NOTHING executes them until a `vortyx_worker_agent`
(each running the unchanged Phase 12 stack) claims them through the
worker protocol. A queued job with no worker attached is the honest state
— the API never fakes execution.
