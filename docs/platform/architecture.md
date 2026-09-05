# Platform Architecture (Phase 11)

## Where this layer sits

Vortyx GPU is not a web service. It is a GPU/computing technology project
whose compute path has been stable since Phase 10:

```
Application → Scheduler → Task Queue → Virtual GPU → Compute Runtime
                                                   → Resource Manager → Backend → Physical Device
```

Phase 11 adds a SEPARATE layer — the **Platform Control Plane Foundation** —
between the local engine and the future network/distributed phases:

```
Local Vortyx GPU engine (unchanged, C++)
        │  knows NOTHING about the platform layer
        ▼
vortyx::platform  (C++, src/platform/)
        │  identity · metadata · job contract · auth boundary
        │  provider-neutral IPlatformStore
        │  InMemoryPlatformStore (local/mock reference implementation)
        │  strict JSON + API contract codec (the executable contract)
        ▼
IPlatformStore  (provider-neutral seam)
        │
        ├── InMemoryPlatformStore   ← local dev + tests (no infrastructure)
        └── Supabase adapter        ← platform/api/src/supabase-store.ts (TypeScript,
                                       deployed on Vercel; configured AFTER Phase 11)
        ▼
Supabase (PostgreSQL + Auth + RLS)  ·  Vercel (serverless API)
        ▼
Phase 12~15: Device Agents / Distributed Execution / Resource-Aware
Scheduling / Personal Compute Network (NONE of this exists yet)
```

## The dependency rules (enforced by the build graph)

1. `src/core/**` never includes `src/platform/**`. The compute core knows no
   Supabase, no Vercel, no HTTP, no JSON, no JWT. It builds unchanged with
   `VORTYX_ENABLE_PLATFORM=OFF`.
2. `vortyx_platform` (the separate static library) depends on the core in
   exactly one narrow place: the operation vocabulary
   (`vortyx::compute::ComputeOp` / `workload_label`) and the version string.
   One vocabulary, no duplicated op enum.
3. Provider code lives ONLY behind `IPlatformStore`. The C++ repository
   ships no provider implementation; the TypeScript adapter is the only
   module that imports `@supabase/supabase-js`, and no test path ever loads
   it.

## The contract is implemented twice, on purpose

The control-plane contract exists as two implementations of the same
specification, each pinned by its own tests so they cannot drift:

| Side | Location | Role |
|------|----------|------|
| C++ | `src/platform/` + `tests/test_platform*.cpp` | Device-agent side; executable specification of the rules |
| TypeScript | `platform/api/` + `test/*.test.ts` | Vercel-hosted API side; same routes, schemas, codes |

Any behavioral change must land on both sides and both test suites must
agree. `docs/platform/api.md` is the prose version of that shared contract.

## ComputeTask vs JobEnvelope (the boundary that must not blur)

- `vortyx::compute::ComputeTask` — LOCAL execution semantics. Carries the
  actual input data and runs through the unchanged local path.
- `vortyx::platform::JobEnvelope` — REMOTE transport semantics. WHICH
  operation, HOW BIG, who wants it — deliberately NO data payload. ComputeTask
  is never serialized into a JobEnvelope and never travels the network in
  Phase 11.

They share exactly one thing: the operation vocabulary. Everything else
stays separate so Phase 13 partitioning can evolve the local side without
touching the wire contract.

## Job lifecycle vs TaskQueue lifecycle

`vortyx::platform::JobStatus` (queued → running → completed/failed/
cancelled) is the CONTROL-PLANE lifecycle. `vortyx::queue::TaskState` (Phase
6, no Cancelled) is the lifecycle of one task inside one local FIFO queue.
Neither maps onto the other; neither was changed by Phase 11.

## CMake integration

```
option(VORTYX_ENABLE_PLATFORM, ON)
  ON  → vortyx_platform (STATIC) + PlatformTest + PlatformContractTest
  OFF → exactly the Phase 10 build (core untouched)
```

The platform layer is standard-library-only: no cloud SDK, no HTTP client,
no JSON dependency. A C++ consumer that never touches the platform layer
pays nothing for it.

## What Phase 11 deliberately does NOT contain

- No distributed execution, no remote worker, no multi-device anything.
- No real telemetry: no battery, load, memory or connectivity fields — the
  seams are documented, the fields do not exist yet.
- No real Supabase project, no real deployment, no real secrets anywhere.
- No priority scheduling: `JobEnvelope::priority` is a RESERVED transport
  field that nothing reads.
- No Phase 12 device-agent code, no Phase 13 partitioning, no Phase 14
  resource-aware scheduling, no Phase 15 network.
