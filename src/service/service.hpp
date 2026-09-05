#pragma once

// Vortyx Service Layer / Production GPU Platform Foundation (Phase 14) —
// umbrella header.
//
// Layering (the Phase 13 diagram extended ADDITIVELY — nothing below the
// tensor boundary changed):
//
//   Application / Client
//        ↓
//   Service Layer                    (vortyx::service — Phase 14)
//        │  Projects / Memberships / Authorization (authz table)
//        │  Quota (project policy ledger)   Rate limiting (fixed window)
//        │  Queue (provider-neutral FIFO)   Job Service (the full flow)
//        │  Audit (bounded events)          Metrics (real counters only)
//        │  Health (honest per-component)   Artifacts (metadata only)
//        ↓
//   Platform / Cloud Control Plane   (vortyx::platform — Phase 11, unchanged)
//        ↓
//   Distributed Orchestrator          (vortyx::distributed — Phase 12,
//        │                             unchanged; the service HANDS JOBS to
//        │                             it — never a second scheduler)
//        ↓
//   Tensor Layer                      (vortyx::tensor — Phase 13, unchanged)
//        ↓
//   Core Engine                       (vortyx::compute / resource — unchanged)
//
// DEPENDENCY RULES (enforced by the build graph):
//   - vortyx_service depends on vortyx_tensor (which brings
//     vortyx_distributed -> vortyx_platform -> vortyx_core) — the additive
//     stack pattern Phases 11-13 established.
//   - The core/platform/distributed/tensor layers NEVER include the service
//     layer. No circular dependency exists (service -> tensor only).
//   - With VORTYX_ENABLE_SERVICE=OFF — or TENSOR=OFF / PLATFORM=OFF, which
//     disable the whole upper stack — the project builds exactly like the
//     previous phase.
//
// HONEST SCOPE (what Phase 14 IS and IS NOT):
//   IS:    a real, tested service control plane over the existing layers:
//          projects with membership roles and a pure authorization table,
//          a project quota ledger with exactly-once release, deterministic
//          rate limiting, a provider-neutral queue, the full submission
//          flow into the Phase 12 scheduler, bounded audit events, real
//          counters, honest health reporting, artifact metadata, and a
//          machine-readable contract — with local end-to-end execution
//          over virtual devices.
//   IS NOT: a real cloud deployment (no HTTP server exists in the C++
//          core), a Supabase/Redis/PostgreSQL integration (the provider
//          interfaces are the extension points; the in-memory stores are
//          the local/mock references), multi-tenant network isolation, real
//          billing, a GPU marketplace, or any performance claim. Each
//          absence is an explicit, documented non-goal — not a TODO
//          disguised as done.

#include "service/artifact.hpp"
#include "service/audit.hpp"
#include "service/authz.hpp"
#include "service/contract_service.hpp"
#include "service/health.hpp"
#include "service/metrics.hpp"
#include "service/platform_service.hpp"
#include "service/project.hpp"
#include "service/quota.hpp"
#include "service/queue.hpp"
#include "service/ratelimit.hpp"
#include "service/service_status.hpp"
