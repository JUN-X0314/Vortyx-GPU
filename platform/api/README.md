# Vortyx Platform API (Phase 11)

The Vercel-ready control-plane API layer of the Vortyx GPU platform
foundation. It implements, in TypeScript, the exact contract that the C++
platform layer (`src/platform/`) pins in tests: same routes, same schemas,
same error codes, same status mapping.

- **No real deployment happens in Phase 11.** Creating the Supabase project,
  running the migrations and deploying to Vercel are the project owner's
  post-Phase-11 steps (checklist: `docs/platform/deployment-checklist.md`).
- **Local/mock mode (default)**: `npm run dev` starts
  [dev-server.mjs](./dev-server.mjs) with the in-process
  `InMemoryPlatformStore` — no accounts, no keys, no network. State is
  per-process and honest about it: this mode is for development and tests,
  never for production.
- **Supabase mode**: `VORTYX_STORE=supabase` + project settings switches the
  provider-neutral `IPlatformStore` (`src/store.ts`) to the Supabase adapter
  (`src/supabase-store.ts`, the only module that knows Supabase). Every query
  runs with the caller's access token, so Row Level Security (see
  `platform/supabase/migrations/`) is the enforcement backstop.

## Layout

```
api/            Vercel function adapters (one file per route; 3 lines each)
src/            Provider-neutral logic: contract, router, store interface,
                memory store, auth rules, Supabase adapter (isolated)
test/           node:test suite (contract, store, router) — no Supabase, no
                network, no secrets required
.env.example    Environment template (server-only vs publishable classified)
```

## Commands

```bash
npm test   # node --test (Node >= 22.6; no npm install required)
npm run dev
```

The full contract documentation lives in `docs/platform/` (repository root).
