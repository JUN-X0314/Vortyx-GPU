# Local Development (Phase 11)

Everything in Phase 11 runs with NO Supabase account, NO Vercel project, NO
keys and NO network. The local/mock store is the reference implementation of
the control-plane rules, and both test suites exercise it.

## C++ side

```bash
# Normal build (Vulkan optional as always)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# CPU-only build (identical platform behavior)
cmake -B build-cpuonly -S . -DCMAKE_BUILD_TYPE=Release -DVORTYX_ENABLE_VULKAN=OFF

# Run the tests (PlatformTest + PlatformContractTest join the classic suite)
ctest --test-dir build -C Release --output-on-failure
```

Disable the platform layer entirely (Phase 10-equivalent build):

```bash
cmake -B build-noplatform -S . -DVORTYX_ENABLE_PLATFORM=OFF
```

The platform layer is standard-library-only C++17: no cloud SDK, no HTTP
client, no JSON dependency.

## API layer (TypeScript, Vercel-ready structure)

```bash
cd platform/api

# Run the API test suite — no npm install required (pure node:test)
npm test          # node --test --experimental-strip-types test/   (Node >= 22.6)

# Start the local dev server (in-memory store, http://localhost:3000)
npm run dev
PORT=8080 npm run dev
```

Local auth scheme (MOCK ONLY — loud by design): `Authorization: Bearer
local:<user_id>`. There is no real credential behind it; production uses
Supabase Auth tokens verified server-side.

Smoke tour with curl:

```bash
curl -s localhost:3000/api/health
curl -s localhost:3000/api/platform/info

TOKEN="Authorization: Bearer local:11111111-1111-4111-8111-111111111111"
curl -s -X POST localhost:3000/api/devices/register -H "$TOKEN" \
  -H 'content-type: application/json' \
  -d '{"device_id":"dev-1","protocol_version":"1","software_version":"0.11.0",
       "operating_system":"linux","backends":["cpu"],"operations":["vector_add"]}'

curl -s -X PATCH localhost:3000/api/devices/dev-1/heartbeat -H "$TOKEN"

curl -s -X POST localhost:3000/api/jobs -H "$TOKEN" \
  -H 'content-type: application/json' \
  -d '{"job_id":"job-1","operation":"vector_add","element_count":64,
       "protocol_version":"1","submitted_by_device_id":"dev-1"}'

curl -s -X POST localhost:3000/api/jobs/job-1/cancel -H "$TOKEN"
```

Memory-mode state lives in the dev-server process: restarting it empties
the control plane. That is the documented local/mock behavior — the mode
must never back a real deployment.

## Environment variables (development)

Copy `platform/api/.env.example` to `platform/api/.env.local` if you want to
override defaults. Defaults are already the local/mock mode. See
`security.md` for the server-only vs publishable classification. Real
values are never committed.

## What the local store verifies (test coverage map)

| Requirement | Where it is tested |
|-------------|--------------------|
| device registration / lookup / duplicate | `test_platform.cpp` §6, `store.test.ts` |
| ownership (owner read/update, foreign rejected) | §5–§9, `store.test.ts`, RLS-equivalence rules |
| job creation / lookup / idempotency / conflict | §7, `store.test.ts`, `router.test.ts` |
| job lifecycle incl. invalid transitions | §3, §8, `store.test.ts` |
| result metadata persistence + honesty rules | §9, `store.test.ts` |
| invalid access rejection (401/403/404 mapping) | auth tests, `router.test.ts` |
| request schema / error codes / status mapping | `test_platform_contract.cpp`, `contract.test.ts` |
| serialization determinism + round trip | `test_platform_contract.cpp`, `contract.test.ts` |
| concurrent local-store use | `test_platform.cpp` §10 |
