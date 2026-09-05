#pragma once

// Vortyx Platform / Cloud Layer Foundation (Phase 11) — umbrella header.
//
// Layering (unchanged core, additive platform layer):
//
//   Application ─▶ Scheduler ─▶ Task Queue ─▶ Virtual GPU ─▶ Compute Runtime
//                                                             └▶ Backends
//
//   (unchanged compute path — knows NOTHING about the platform layer)
//
//   Application / Device Agent (Phase 12+)
//        └▶ vortyx::platform   — identity, metadata, job contract, auth,
//                                provider-neutral IPlatformStore, the local
//                                InMemoryPlatformStore, the strict JSON
//                                module and the API contract codec
//             └▶ (behind IPlatformStore, future) Supabase-backed control
//                plane / Vercel-hosted API — configured by environment
//                variables, NEVER linked into or known by the compute core
//
// Dependency rules enforced by this structure (Phase 11 core requirements):
//   - src/core/** must not include src/platform/** (no Supabase/Vercel/HTTP/
//     JSON knowledge anywhere in the compute path — verified by the fact
//     that the platform layer compiles as a SEPARATE library and the core
//     builds unchanged with VORTYX_ENABLE_PLATFORM=OFF).
//   - platform → core is allowed in exactly one narrow place: the operation
//     vocabulary (ComputeOp/workload_label) and the version string.
//   - provider code (Supabase or otherwise) exists only behind
//     IPlatformStore; the C++ repository ships no provider implementation —
//     the TypeScript adapter lives in platform/api.

#include "platform/auth.hpp"
#include "platform/contract.hpp"
#include "platform/identity.hpp"
#include "platform/job.hpp"
#include "platform/json.hpp"
#include "platform/memory_store.hpp"
#include "platform/metadata.hpp"
#include "platform/status.hpp"
#include "platform/store.hpp"
