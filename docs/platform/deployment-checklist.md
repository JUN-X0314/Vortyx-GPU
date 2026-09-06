# Deployment Preparation Checklist (post-Phase-11, updated Phase 16)

Phase 11 ships the STRUCTURE and the CONTRACT. It deliberately performs no
real deployment and contains no real secrets. When you are ready, these are
the steps the structure was designed for.

## 1. Supabase project

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

## 2. Vercel project (the API)

1. Import the repository; set the **Root Directory** to `platform/api`.
2. Add environment variables (Production + Preview):
   - `VORTYX_STORE=supabase`
   - `SUPABASE_URL`, `SUPABASE_ANON_KEY`
   - `SUPABASE_SERVICE_ROLE_KEY` (only if/when a future phase needs
     privileged server jobs — never expose it)
3. Deploy. `npm install` on Vercel installs `@supabase/supabase-js` (the
   declared dependency; the adapter is the only importer).
4. Smoke checks:
   - `GET https://<project>/api/health` → `"store": "supabase"`,
     `"config_error": null`
   - `GET /api/platform/info` → protocol `1`, three operations, two backends
   - Register a device with a real Supabase access token from any test user
     (create the user in the Supabase dashboard).

## 3. Vercel project (the web console) — Phase 15/16

The web console (`platform/web`) is a no-build static SPA. It is deployed
as a SEPARATE Vercel project from the API (never one project serving both —
their root directories differ).

TWO correct configurations exist; pick ONE per project and do not mix
them (Vercel reads `vercel.json` from the project's Root Directory only):

- **Preferred: Root Directory = `platform/web`.** The project then reads
  `platform/web/vercel.json` (`"framework": null, "outputDirectory": ".",
  "cleanUrls": true`) and serves the directory as-is: `index.html`,
  `assets/styles.css`, `js/main.js`, `js/config.js`.
- **Fallback: Root Directory = repository root.** The root `vercel.json`
  (added in Phase 16: `"framework": null, "outputDirectory":
  "platform/web"`) directs the static output at `platform/web`. This is
  the repository-side fix for the observed failure mode below; keep the
  two files consistent if you ever touch them, and never point a
  root-rooted project at `platform/api` this way (the API project must be
  rooted at `platform/api` so its `api/` serverless functions resolve).

**The failure mode to check for (observed on the production project):** a
deployment reported READY while the root URL returned 404 NOT_FOUND, with
`no files were prepared` in the build log. That combination means the
deployment produced ZERO static output files — the classic cause is a
root directory / output directory mismatch (e.g. the project rooted at the
repository root while `platform/web/vercel.json` sits one level down, so
it is never read, or an output directory that does not exist). A READY
status alone proves nothing; the URL must be fetched.

Smoke checks (all from the live production URL):
- `GET /` → 200 with the real `index.html` HTML (never 404).
- `GET /assets/styles.css`, `/js/main.js`, `/js/config.js` → 200.
- Client-side routes are HASH routes (`/#/projects`) — refresh cannot 404
  by construction; there is nothing server-side to rewrite. Verify by
  refreshing a deep hash route.
- `js/config.js` is runtime configuration and is NOT committed (see
  `.gitignore`); provide it through the deployment (a build step or a
  committed copy in a private fork). The console must show an explicit
  error state when the API is unreachable — never fake data.

## 4. Local mock parity check (before and after)

Run `npm run dev` (memory mode) and compare responses against the deployed
API for the same payloads — they must be identical except for timestamps.
Any difference means the two store implementations drifted, which the
shared contract forbids.

## 5. Security review before going public

- No real key is committed anywhere (`git log -p | grep -i key` spot check).
- `SUPABASE_SERVICE_ROLE_KEY` exists only in Vercel's server-side env.
- RLS policies present and enabled (re-run the verification query from
  `database.md`).
- Two-account isolation test: user B cannot read/update user A's devices,
  jobs or results through the deployed API (must be 404).

## 6. What NOT to deploy yet

Phase 11 is a control-plane foundation. There is still no remote executor:
jobs submitted through the deployed API will be ACCEPTED and QUEUED but
nothing executes them. Execution arrives with the Phase 12 device agents.
Deploying the API early for experimentation is fine — just know that
`queued` is currently the end of the line.
