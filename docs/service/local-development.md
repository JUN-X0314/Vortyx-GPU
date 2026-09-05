# Local Development (Phase 14 service layer)

## Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -C Release --output-on-failure
```

The service layer builds by default (`VORTYX_ENABLE_SERVICE=ON`). It
requires the tensor layer (and therefore the distributed and platform
layers); turning any of the upper-stack options OFF turns the service OFF
with it:

```bash
cmake -B build -S . -DVORTYX_ENABLE_SERVICE=OFF   # exactly the Phase 13 build
cmake -B build -S . -DVORTYX_ENABLE_TENSOR=OFF    # exactly the Phase 12 build
cmake -B build -S . -DVORTYX_ENABLE_PLATFORM=OFF  # exactly the Phase 10 build
```

## Run the service tests

```bash
ctest --test-dir build -C Release -R Service --output-on-failure
```

Six suites cover the layer end to end (no GPU, no network, no external
provider required):

- `ServiceProjectTest` — projects, memberships, the pure authorization
  table, anti-enumeration and IDOR refusals.
- `ServiceQuotaTest` — the ledger: per-field refusals, exactly-once
  release, replay without double charge, concurrent reserve/release
  consistency.
- `ServiceRateLimitTest` — deterministic fixed windows over `FakeClock`.
- `ServiceQueueTest` — FIFO, idempotent enqueue, exactly-once removal,
  capacity refusal.
- `ServiceJobsTest` — the full submission flow over REAL virtual devices:
  2-device sharded execution with exact results, security refusals, quota
  refusals and releases, idempotent replays, rate limiting, condvar-driven
  cancellation races, archived-project refusal, and a concurrent
  submission storm with consistent end state.
- `ServiceOpsTest` — audit structure/bounds/scope, honest health values,
  artifact metadata, metrics counters, JSON contract round trips.

## Minimal local service (C++)

```cpp
#include "service/service.hpp"

using namespace vortyx::service;

// The Phase 12 execution world (a local cluster over the loopback
// transport + simulated devices — see docs/distributed/local-development).
vortyx::distributed::LocalDeviceRegistry registry(clock);
vortyx::distributed::LocalInProcessTransport transport;
// ... attach workers via the simulator ...

PlatformService::Deps deps;
deps.registry = &registry;
deps.transport = &transport;
deps.clock = clock;                      // the injected clock (determinism)
// deps.platform_store optional; stores default to the in-memory refs.

std::unique_ptr<PlatformService> service;
PlatformService::create(deps, PlatformServiceConfig{}, service, error);

vortyx::platform::AuthContext auth = vortyx::platform::make_authenticated("user-a");
ProjectRecord project;
service->create_project(auth, "demo", project);

SubmitJobRequest request;
request.project_id = project.project_id;
request.distributed = /* envelope + ComputeTask + shard count */;
ServiceJobView job;
bool created = false;
service->submit_job(auth, request, job, created);
service->wait_for_terminal(auth, job.job_id, 10000, job);
```

## Configuration defaults (safe by design)

| Setting | Default | Meaning |
|---------|---------|---------|
| `dispatcher_count` | 2 (1..8) | threads draining the queue into the orchestrator |
| `rate_limit_enabled` | true | submissions are rate limited per user |
| `rate_limit_max_submissions` | 60 / window | the fixed-window limit |
| `rate_limit_window_ms` | 60000 | window length |
| `default_project_quota` | 4 jobs / 16 shards / 1 GiB | per-project policy |
| `audit_max_entries` | 10000 | bounded ring; drops counted honestly |
| `max_queue_depth` | 1024 | capacity refusal past the bound |
| `max_requested_shard_count` | 64 | service-level shard cap |

Nothing is unlimited by default; no external provider is contacted by
default (none is contacted at all — the providers are in-process stores).

## Sanitizers

```bash
cmake -B build-asan -S . -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j && ctest --test-dir build-asan -C Debug --output-on-failure
```
