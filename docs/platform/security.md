# Security Model & Threat Notes (Phase 11)

## Secrets and key classes

| Value | Class | Where it may appear |
|-------|-------|---------------------|
| `SUPABASE_URL` | server-side config (not secret) | `.env*` (git-ignored), Vercel env vars |
| `SUPABASE_ANON_KEY` | **publishable by design** | same as above; safety comes from RLS, not secrecy |
| `SUPABASE_SERVICE_ROLE_KEY` | **SERVER-ONLY secret** — bypasses RLS | Vercel server-side env only. Used by NO Phase 11 endpoint (reserved). Never in client code, never committed, never logged |
| Supabase Auth access tokens (user JWTs) | bearer credentials | transient: request `Authorization` header → server memory only |

Committed to the repository: `.env.example` (placeholders only). `.env`,
`.env.local`, `.env.production` and `node_modules/` are git-ignored (see
`.gitignore`). No real key exists anywhere in Phase 11 — none was generated,
none is needed.

There is deliberately NO `NEXT_PUBLIC_*` naming: this is a plain Vercel
Functions project, nothing is shipped to a browser, and the future device
agent receives its connection settings through its own configuration.

## The three enforcement layers (defense in depth)

1. **API layer (AuthN)**: the Bearer token is verified server-side through
   Supabase Auth (`auth.getUser`). The verified subject — never a
   client-claimed id — becomes the `AuthContext`.
2. **Store layer (AuthZ)**: every `IPlatformStore` operation applies the one
   ownership rule (`auth.uid() = owner_user_id`) in code. The
   InMemoryPlatformStore is the executable specification of this rule.
3. **Database layer (RLS)**: the same rule re-enforced by PostgreSQL row
   level security. Even a fully misconfigured API server cannot expose
   another user's rows while queries run with the caller's token (the
   adapter's only mode). The service-role bypass is never wired into any
   Phase 11 code path.

All three implement the SAME rule, and the tests pin it
(`test_platform.cpp`, `store.test.ts`, the RLS policies in the migration).

## Anti-enumeration: foreign records are invisible

A single-record lookup of a resource that is missing **or belongs to
another user** returns 404 `not_found` — never 403. Reason: a 403 would
leak which ids exist for other users. This mirrors RLS exactly (a foreign
row is simply invisible), which is why the store layer maps its raw
authorization decision to NotFound for single-record operations.

The one Forbidden that does surface: `POST /api/jobs` referencing a
`submitted_by_device_id` the caller does not own (unknown and foreign are
indistinguishable) — the same outcome the RLS `INSERT … WITH CHECK` policy
produces.

## Identity hygiene (Phase 11 scope)

- Device/job ids are client-generated UUID v4 strings validated against
  `^[A-Za-z0-9._-]{1,128}$` — no hardware fingerprints, no MAC addresses,
  no serial numbers, no PII collection.
- `display_name` is a free label, never used for matching or identity.
- Passwords: never stored, never handled — authentication is delegated to
  Supabase Auth entirely.

## Input hardening

- Strict JSON parser (C++ side): RFC 8259 subset, rejects trailing content,
  leading zeros, NaN/Infinity, lone surrogates, unescaped control
  characters; nesting capped at depth 64 (`kMaxJsonDepth`) — a
  stack-exhaustion guard against hostile payloads.
- Strict schemas: unknown request fields are rejected (`invalid_value`), so
  a typo cannot silently change meaning.
- Ids and enums are validated at every boundary (API body, store, database
  CHECK constraints).
- All list endpoints return records owned by the caller only; there is no
  unscoped query in the codebase.

## Honest-by-construction rules with security value

- Timestamps the client cannot forge server-side truth: owner, status,
  `last_seen_ms`, `created_at_ms` are STORE-managed (server clock), never
  accepted from the request body.
- A failed job/result REQUIRES an error reason — failures cannot be hidden
  by recording a bare "failed".
- Terminal job states are final everywhere (store, tests, DB CHECK).

## Residual risks / scope notes (honest)

- Phase 11 has no rate limiting, no audit logging, no anomaly detection.
  These are operational concerns for the deployment phase; the current API
  surface is intentionally minimal.
- The local/mock store and its `local:<user_id>` token scheme are
  development conveniences with NO security value and are documented as
  such; production runs `VORTYX_STORE=supabase`.
- RLS is only as strong as the Supabase project configuration (e.g. leaked
  service-role keys bypass it) — hence the sharp classification at the top
  of this document.
