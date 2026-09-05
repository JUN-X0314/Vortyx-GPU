# Local Development (Phase 12)

Phase 12 needs **no GPU hardware, no network, no cloud account and no
secrets** to build, test or exercise the distributed path. Everything runs
in one process against the local/mock registry and the loopback transport.

## Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
ctest --test-dir build -C Release --output-on-failure
```

The distributed tests (`DistributedTest`, `DistributedSchedulerTest`,
`DistributedJobsTest`, `DistributedWorkerTest`,
`DistributedOrchestratorTest`, `DistributedContractTest`) run on every
system: the CPU backend is always available, GPU tests keep their honest
SKIP behavior, and every time-dependent path is driven by an injected
`FakeClock` — there is not a single sleep in the suite.

Useful variants:

```bash
# The Phase 10 build (no platform, no distributed layer):
cmake -B build-noplatform -S . -DVORTYX_ENABLE_PLATFORM=OFF

# Only the distributed layer disabled (platform stays):
cmake -B build-nodist -S . -DVORTYX_ENABLE_DISTRIBUTED=OFF

# Memory-safety run of the distributed suite:
cmake -B build-asan -S . -DCMAKE_CXX_FLAGS="-fsanitize=address -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
```

## The diagnostic tool

```bash
./build/vortyx_cluster
```

Builds a four-device local cluster (differing memory/concurrency
capacities), submits a 40,000-element `vector_add` as four shards, runs
the real path — registry → placement → leases → workers → compute runtime
→ aggregation — and prints the cluster view, the shard table, and the
verification of the reassembled result against a host reference
(bit-exact). It is a diagnostic, not a benchmark: it makes no performance
claims, and the simulated capacities are the configuration printed in the
device model.

## Writing a distributed scenario in code

```cpp
#include "distributed/distributed.hpp"

auto clock = std::make_shared<vortyx::distributed::FakeClock>(1000);
vortyx::distributed::LocalDeviceRegistry registry(clock);
vortyx::distributed::LocalInProcessTransport transport;

vortyx::distributed::DistributedConfig config;      // safe defaults
config.enabled = true;

vortyx::distributed::DistributedOrchestrator::Deps deps;
deps.registry = &registry;
deps.transport = &transport;
deps.clock = clock;
// deps.platform_store = &store;   // optional Phase 11 integration

std::unique_ptr<vortyx::distributed::DistributedOrchestrator> orch;
DistributedOrchestrator::create(deps, config, orch, error);

// Virtual devices with self-reported capacities (honest backend claims
// are queried from a real Runtime on this host):
vortyx::distributed::LocalMultiDeviceSimulator sim(registry, transport, "user");
SimulatorDeviceConfig dev;
dev.device_id = "device-0";
dev.capacity.memory_bytes = 64 * 1024 * 1024;
dev.capacity.concurrent_jobs = 2;
dev.max_concurrent_shards = 2;
sim.add_device(dev, created, error);

// Submit — synchronous: returns at the terminal state.
vortyx::distributed::DistributedJobRequest request;
request.envelope.job_id = "my-job";
request.envelope.operation = vortyx::compute::ComputeOp::VectorAdd;
request.envelope.element_count = n;
request.task = task;               // the local payload (stays local)
request.requested_shard_count = 2; // 1 = single-device, >1 = multi-device

orch->submit(vortyx::platform::make_authenticated("user"), request, job, created);
// job.status / job.result.data / job.shards[i].state are now final.
```

Observability in tests and tools:

- `orch->cluster_snapshot(owner)` — the immutable, ownership-filtered
  view (revision, states, health, capacities, allocations).
- `to_debug_string(snapshot)` / `to_debug_string(job)` — deterministic
  multi-line dumps (stable field order; safe to assert on).
- `orch->check_heartbeats(owner)` — an explicit liveness judgment pass.
- Deterministic failure injection for failure/retry tests:
  `transport.inject_failure(device_id, count, code)`.

## Configuration

`DistributedConfig::from_environment()` parses (absent → documented
default; present-but-invalid → explicit rejection, never a silent fix):

| Variable | Default | Meaning |
|---|---|---|
| `VORTYX_DISTRIBUTED_ENABLED` | `false` | the master switch for config-driven construction |
| `VORTYX_SCHEDULER_POLICY` | `least_loaded` | `round_robin` / `least_loaded` / `capability_fit` |
| `VORTYX_MAX_DEVICES` | `16` | registration bound per owner (0 = unlimited) |
| `VORTYX_HEARTBEAT_TIMEOUT_MS` | `30000` | liveness judgment threshold |
| `VORTYX_MAX_RETRIES` | `3` | EXTRA attempts per shard (total = 1 + this) |
| `VORTYX_RETRY_BACKOFF_MS` | `10` | exponential backoff base |
| `VORTYX_LEASE_TTL_MS` | `600000` | reservation time-to-live |
| `VORTYX_ALLOW_SINGLE_DEVICE_FALLBACK` | `true` | coalesce when devices are fewer than shards |
| `VORTYX_MAX_SHARDS_PER_JOB` | `64` | per-job placement bound |
| `VORTYX_SHARD_THREADS` | `0` | 0 = sequential waves; N = at most N shards in flight |

Existing users who never touch any of this keep the Phase 11/10 behavior
exactly: the layer is an additive library nothing in the old path calls.

## The TypeScript control plane (optional, local/mock)

```bash
cd platform/api
node --test --experimental-strip-types
```

The distributed endpoints (`/api/cluster`, `/api/distributed/jobs*`) run
against `InMemoryDistributedStore` — the same rules the C++ layer pins,
no network and no Supabase required. See `docs/distributed/api.md` and
`docs/platform/local-development.md` for the Phase 11 surface this
extends.
