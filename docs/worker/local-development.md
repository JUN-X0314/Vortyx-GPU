# The Native Worker — Local Development

## Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# the agent binary lands next to vortyx: build/vortyx_worker_agent
# (Windows: build\Release\vortyx_worker_agent.exe)
```

`VORTYX_ENABLE_WORKER=ON` is the default; with `VORTYX_ENABLE_DISTRIBUTED=OFF`
(or `VORTYX_ENABLE_PLATFORM=OFF`) the worker layer disables itself with the
distributed layer and the build reproduces Phase 14 exactly.

## Configuration (environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `VORTYX_WORKER_ENDPOINT` | (required) | The control-plane base URL, `http://host:port[/prefix]`. `https` is refused — the C++ core has no TLS (see architecture.md). |
| `VORTYX_WORKER_TOKEN` | (required) | The worker bearer token; must equal the API's `VORTYX_WORKER_TOKEN`. |
| `VORTYX_WORKER_ID` | `vortyx-worker-<pid>` | The stable claim-ownership label. |
| `VORTYX_WORKER_POLL_MS` | 2000 | Idle poll cadence. |
| `VORTYX_WORKER_LEASE_MS` | 60000 | The requested claim lease (API-valid range 1000..600000; must be ≥ 2 × heartbeat). |
| `VORTYX_WORKER_HEARTBEAT_MS` | 15000 | Lease-renewal cadence (the first beat fires immediately at execution start). |
| `VORTYX_WORKER_DEVICES` | 2 | The number of local simulated devices (1..64). |
| `VORTYX_WORKER_DEVICE_MEMORY_MB` | 256 | Each simulated device's self-reported memory capacity. |

## Run against the local control plane

```bash
# Terminal 1 — control plane (memory mode)
cd platform/api
VORTYX_WORKER_TOKEN=dev-token node dev-server.mjs        # :3000

# Terminal 2 — the native worker
cd <repo root>
VORTYX_WORKER_ENDPOINT=http://localhost:3000 \
VORTYX_WORKER_TOKEN=dev-token \
VORTYX_WORKER_ID=dev-worker-1 \
  ./build/vortyx_worker_agent
```

Expected honest output (a machine without a Vulkan device):

```
Vortyx Worker (Phase 15 native execution agent, config Release)
version 0.15.1
control plane probe: HTTP 200
[idle] no queued work
[claimed] job <id> -> completed, recorded=true
```

With a worker connected, submitted jobs reach `completed` with the real
shard counts and result metadata. With NO worker running, submitted jobs
stay `queued` — that is the honest state, not a bug.

## Drive the flow by hand

```bash
API=http://localhost:3000
AUTH="Authorization: Bearer local:me"      # the documented MOCK scheme

PROJECT=$(curl -s -X POST $API/api/projects -H "$AUTH" \
  -d '{"name":"demo"}' | python3 -c "import sys,json;print(json.load(sys.stdin)['project_id'])")

curl -s -X POST $API/api/projects/$PROJECT/jobs -H "$AUTH" \
  -d '{"job_id":"job-1","operation":"vector_add","element_count":10000,"requested_shard_count":2}'

curl -s $API/api/service/jobs/job-1 -H "$AUTH" | python3 -m json.tool
# status=completed, total_shards=2, succeeded_shards=2, result_element_count=10000
```

## Tests

`tests/test_worker.cpp` (CTest name `WorkerTest`) runs on every system with
no GPU and no network: synthesis determinism, protocol strictness, a REAL
2-device distributed execution verified bit-exact against the host
reference, and the agent loop over a scripted transport (claim/execute/
report, cancel relay, failure reporting).
