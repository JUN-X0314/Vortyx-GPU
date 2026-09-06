# The Fabric — Local Development (Phase 16)

## Build and run the fabric tests

The fabric layer needs no GPU, no network, no cloud account:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -C Release --output-on-failure -R Fabric
```

The five fabric test executables (registered as `FabricCoreTest`,
`FabricGraphTest`, `FabricPlannerTest`, `FabricReplanTest`,
`FabricE2ETest`) cover, respectively: descriptor/cost/feedback honesty;
graph structure; the deterministic planner; lineage and replanning; and
the end-to-end acceptance scenarios over the real local simulator.

The TensorGraph adapter test (`TensorFabricTest`) and the opt-in service
integration test (`ServiceFabricTest`) run in the full suite:

```bash
ctest --test-dir build -C Release --output-on-failure
```

## Feature-off builds

```bash
# The fabric layer OFF — reproduces the Phase 15 build exactly
# (the tensor layer disables itself with the fabric):
cmake -B build-no-fabric -S . -DCMAKE_BUILD_TYPE=Release -DVORTYX_ENABLE_FABRIC=OFF
cmake --build build-no-fabric -j
ctest --test-dir build-no-fabric -C Release --output-on-failure
```

`PLATFORM=OFF` / `DISTRIBUTED=OFF` continue to disable the fabric
automatically (and the tensor/service layers with it), exactly as before.

## A five-minute tour

Build a fabric-planned cluster the way the tests do
(`tests/test_fabric_e2e.cpp` is the complete reference):

1. Create the world: `FakeClock` (determinism by injection), a
   `LocalDeviceRegistry`, a `LocalInProcessTransport`, and a
   `LocalMultiDeviceSimulator` — then add devices with HONEST
   capabilities (the simulator probes its own Runtime for the backend
   claim; you configure the capacity and the operation claims).
2. Create the policy: `auto policy = make_fabric_policy(config);` and set
   `Deps::policy_override = policy;` before
   `DistributedOrchestrator::create`.
3. Submit through the UNCHANGED orchestrator API
   (`submit(auth, request, record, created)`). Execution, retries,
   leases, cancellation — all Phase 12, untouched.
4. Read back the plan: `policy->last_plan_for(job_id)` (the latest plan),
   `policy->lineage_for(job_id)` (the version history), and
   `policy->counters()` (the real invocation/publication/rejection
   counters).

## Local verification performed for Phase 16 (real commands)

- Release build + full CTest (47 tests), Debug + ASan/UBSan
  (`-DVORTYX_ENABLE_SANITIZERS=ON`, Vulkan OFF), CPU-only, `PLATFORM=OFF`,
  `TENSOR=OFF`, `SERVICE=OFF`, `FABRIC=OFF` builds.
- TypeScript suite (`platform/api`: `node --test
  --experimental-strip-types`) and web logic suite (`platform/web`:
  `node --test`).
- PostgreSQL 17 integration (`python3 scripts/pg_integration_test.py`
  against a real local server) — unchanged by Phase 16 (no schema
  change) and re-run to prove it.

CI runs the same commands (plus Windows MSVC and Clang builds); see
`.github/workflows/ci.yml`.
