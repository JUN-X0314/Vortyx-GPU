#pragma once

// Vortyx Distributed / Multi-GPU Device System (Phase 12) — umbrella
// header.
//
// Layering (the Phase 11 diagram extended ADDITIVELY — nothing below the
// platform boundary changed):
//
//   Application / Client
//        ↓
//   Platform / Cloud Control Plane   (vortyx::platform — Phase 11, unchanged)
//        ↓
//   Distributed Orchestrator          (vortyx::distributed — Phase 12)
//        ├─ DeviceRegistry            (registration, leases, snapshots)
//        ├─ Scheduling Policies       (round_robin / least_loaded /
//        │                             capability_fit — pure placement)
//        ├─ Job Sharding              (deterministic element ranges)
//        ├─ Workers                   (LocalWorker -> existing Runtime)
//        └─ Transport                 (LocalInProcessTransport; remote is
//                                      a future IWorkerTransport)
//        ↓
//   Virtual GPU / Compute Runtime    (Phase 3-10, unchanged)
//
// DEPENDENCY RULES (enforced by the build graph):
//   - vortyx_distributed depends on vortyx_platform (identity, auth, job
//     contract, the provider-neutral store) which depends on vortyx_core.
//   - The core NEVER includes the distributed or platform layers; with
//     VORTYX_ENABLE_PLATFORM=OFF (which also disables this layer) the
//     project builds exactly like Phase 10.
//   - No cloud SDK, no HTTP client, no network code of any kind lives in
//     this layer. The transport is an interface; Phase 12 ships only the
//     in-process loopback implementation.
//
// HONEST SCOPE (what Phase 12 IS and IS NOT):
//   IS:    provider-neutral device abstraction, registry, resource model,
//          deterministic scheduling policies, sharding with proven
//          invariants, leases, retry/failure semantics, deterministic
//          aggregation, platform integration, a local multi-device
//          simulator proving the full path end to end.
//   IS NOT: real multi-machine networking, real cloud clusters, real GPU
//          topology discovery, consensus, work stealing, priority
//          scheduling. Those are later phases and are not pretended here.

#include "distributed/aggregator.hpp"
#include "distributed/clock.hpp"
#include "distributed/cluster.hpp"
#include "distributed/config.hpp"
#include "distributed/contract_distributed.hpp"
#include "distributed/debug.hpp"
#include "distributed/device.hpp"
#include "distributed/heartbeat.hpp"
#include "distributed/job.hpp"
#include "distributed/lease.hpp"
#include "distributed/orchestrator.hpp"
#include "distributed/policy.hpp"
#include "distributed/registry.hpp"
#include "distributed/resource.hpp"
#include "distributed/retry.hpp"
#include "distributed/shard.hpp"
#include "distributed/simulator.hpp"
#include "distributed/topology.hpp"
#include "distributed/transport.hpp"
#include "distributed/worker.hpp"
