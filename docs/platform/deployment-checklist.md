# Deployment Preparation Checklist (post-Phase-11, updated Phase 17.0)

Phase 11 ships the STRUCTURE and the CONTRACT. Phase 16.0.1 wires the
single-project production topology for real. Phase 17 makes the wire
VERIFIED: the checks below are executable commands
(`npm run smoke:production`, `npm run verify:schema`,
`npm run e2e:production`, `python3 scripts/apply_migrations.py`) — a READY
deployment banner alone proves nothing.

## 1. Supabase project

**Observed state (2026-09-07, RESOLVED):** before Phase 17 the production
project answered `/auth/v1/health` → 200 while EVERY application table
returned 404 PGRST205 — zero migrations applied. **Now:** migrations
0001–0010 are applied VERBATIM in lexicographic order (by
`python3 scripts/apply_migrations.py`, which records each file in the
standard `supabase_migrations.schema_migrations` ledger) and
`npm run verify:schema` pins the live database against the repository
inventory — tables, RLS, policies, worker-RPC grants, triggers and the
SECURITY DEFINER inventory — with PASS/FAIL/DRIFT exit codes.

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

**Observed state (2026-09-07, RESOLVED):** configured on the Vercel
project for Production + Preview — `VORTYX_STORE=supabase`,
`SUPABASE_URL`, `SUPABASE_ANON_KEY`, `SUPABASE_SERVICE_ROLE_KEY`
(server-only), `VORTYX_WORKER_TOKEN`, `VORTYX_RECONCILE_TOKEN`,
`VORTYX_ALLOWED_ORIGIN=""`. Since Phase 17 a production deployment
WITHOUT this contract cannot quietly fall back to the memory store:
`readConfig` treats `VERCEL_ENV=production` without
`VORTYX_STORE=supabase` as a `config_error` and the API answers 500
config_error — loud, never faked.

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

**The failure mode to check for (observed on the production project,
RESOLVED by the Phase 17 buildCommand):** a deployment can report READY
while the runtime is dead — the root `api/[...route].ts` catch-all was
transpiled per-file with its literal `"../platform/api/src/vercel.ts"
import preserved, the module never entered the lambda bundle, and every
`/api/*` request died with `ERR_MODULE_NOT_FOUND`. The buildCommand now
bundles the catch-all into ONE self-contained function (the whole
platform/api chain AND @supabase/supabase-js inlined) — and
`npm run verify:bundle` builds the SAME artifact and invokes it, in CI,
before any deploy.

**The third observed failure mode (found in Phase 17, fixed by the
explicit rewrite):** the zero-config catch-all only ever matched
`/api/<one-segment>` — every deeper path (`/api/platform/info`,
`/api/projects/:id`, the whole worker protocol) fell through to the
filesystem 404. Verified against the PREVIOUS deployment, so it predates
Phase 17 and made the smoke checks below impossible to pass. The
`rewrites` entry in the root `vercel.json` pins the contract: every
`/api/*` request, any depth, is served by the single API function.

Deployment URLs previously sat behind Vercel SSO deployment protection
(observed: every `*.vercel.app` URL 302-redirected to
`vercel.com/sso-api`). **RESOLVED 2026-09-07:** project SSO protection is
disabled — the console is publicly reachable; an SSO redirect page is
still treated as a failing smoke check by `npm run smoke:production`.

Smoke checks — ALL AUTOMATED by `npm run smoke:production`
(redirect-intolerant, secret-free, exit code 0 only when every check
passes); the last full run against the live production URL passed 11/11
(2026-09-07):
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
