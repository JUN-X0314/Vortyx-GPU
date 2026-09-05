# Vortyx GPU

[![CI](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml/badge.svg)](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml)

Vortyx GPU is an independent open-source project that researches and develops **GPU and GPU computing technology itself**, not AI services. AI is merely one of many application domains where Vortyx GPU can be utilized.

The long-term goal is to build a software-based GPU computing system, then evolve through Virtual GPU, multi-device computing, distributed computing, and FPGA prototypes, ultimately researching and developing Vortyx's own GPU hardware architecture.

## Current Phase: Phase 9 (v0.9.0) — Stabilization

Phase 9 adds **no new large feature**. It is a full stability audit of the Phase 1~8 codebase — memory safety, lifetime, thread safety, error handling, API consistency, build system, tests, CI, documentation — and ships only the fixes that make the existing implementation more trustworthy:

- **Foreign-buffer handles are now rejected instead of silently mis-executed (stability fix)**: resource ids are unique *per* ResourceManager, not globally. Executing valid `Buffer` handles from a *different* Runtime/Virtual GPU used to resolve them against the executing Runtime's own registry by raw id — if an id collided (easy: every registry starts at 1) and the access layout matched, the execution silently computed on the **wrong buffers** and returned `Status::Ok`, violating the backend contract "foreign buffers are rejected with an error, never accessed". `Runtime::execute(a, b, c)` now verifies ownership of all three handles first (`ResourceManager::owns_handle()`) and fails with a descriptive `Status::InvalidInput`. Covered by regression tests in both `ResourceTest` and `VirtualGpuTest`.
- **Vulkan execution failures preserve their real cause**: `vkEndCommandBuffer`'s result is now checked before submitting (an invalid command buffer previously surfaced later as a misleading "vkQueueSubmit failed"), and `vkResetCommandBuffer` / `vkBeginCommandBuffer` / `vkEndCommandBuffer` / `vkQueueSubmit` / `vkQueueWaitIdle` failures all report the failing call **with its `VkResult`**.
- **Lifecycle/threading contracts documented where they were only implied**: `Runtime` and `VirtualGpu` now state their external-serialization contract explicitly (one executor at a time; the TaskQueue worker becomes the only executor while a queue owns the Virtual GPU), and `Buffer::valid()` semantics after `shutdown()` are precise (handle-level check; operations decide liveness).
- **CI now verifies the CPU-only build explicitly** (new `build-cpu-only` job: Windows + Ubuntu, `VORTYX_ENABLE_VULKAN=OFF`, full test suite) — the "building and running Vortyx never requires the Vulkan SDK" guarantee is now CI-enforced. The GPU-enabled job is unchanged.
- **Everything else is unchanged**: the Phase 1~8 public APIs, the Scheduler policy (`vulkan` > `cpu`), the TaskQueue shutdown policy, the benchmark measurement semantics and the monitoring truthfulness rules are exactly as delivered in v0.8.0 and verified by the unchanged (plus two regression) test suites.

```
Application → Scheduler (select the execution target)
            → Task Queue → Virtual GPU → Compute Runtime → Resource Manager → Backend → Physical Device

Benchmark    ── measures the Virtual GPU path (end-to-end execute() samples)
Resource Monitor ── observes the Runtime / Device / allocation state (read-only)
```

## Benchmark + Resource Monitoring (Phase 8)

The Phase 8 layers are unchanged in v0.9.0; this section documents what they are.

**Implemented in Phase 8:**

- **`vortyx::benchmark`** (`src/core/benchmark/`): real-path measurement. `benchmark_vector_add(gpu, task, config)` runs `config.warmup_iterations` warmup calls (verified, excluded from statistics) and `config.iterations` measured calls of `gpu.execute(task)`, each timed with `std::chrono::steady_clock` around exactly one call. The workload is the caller's task — the module never computes anything itself.
- **Honest measured scope (identical for both backends)**: one sample = one `execute()` end to end — allocation + upload + execution + readback + release — as CPU wall-clock time. The Runtime API does not expose GPU-internal execution boundaries, so no number is ever labeled "GPU time". Setup (initialization, inputs, reference computation, verification) happens outside the timed window.
- **Correctness before performance**: every iteration's output (warmup AND measured) is verified against a host-computed `C[i] == A[i] + B[i]` reference outside the timed window. A failed or wrong iteration fails the whole benchmark with its real `Status` and the iteration index — successful iterations are never cherry-picked around a failure, and a warmup failure aborts the run (reported as such).
- **Real statistics with units in the names**: `min`/`max` (exact nanoseconds), `average`/`median`/`stddev` (nanoseconds, deterministic algorithm — median is the mean of the two middle samples for even counts, stddev is the population stddev), `throughput_elements_per_second` — all computed by the pure, unit-tested `compute_timing_stats()`. No unit-less numbers anywhere; `to_key_values()` exports the same values under stable keys (`min_ns`, `average_ns`, …) with no external serialization dependency.
- **`vortyx::monitor`** (`src/core/monitor/`): stateless snapshot observer. `snapshot(runtime)` reads the Runtime's real backend state (`backend_names()` / `has_backend()` / `backend_unavailable_reason()` / `backend_device()` — the same source of truth the Scheduler probe and every executing Virtual GPU see) plus the ResourceManager's Phase 4 statistics; `snapshot()` (no Runtime) reports only what the standard library can honestly say (`hardware_threads`), everything else explicitly unobserved.
- **No fake metrics, ever**: a snapshot carries real values and explicit unavailable markers (`std::optional` = nullopt, validity flags) — never a 0 standing in for "unknown". Metrics that do not exist in the current stack (GPU utilization, temperature, power draw, instantaneous CPU utilization, current VRAM usage, fan speed, PCIe bandwidth) have **no field at all**: their absence is the honest representation. The monitor contains no platform-specific code and no second discovery path.
- **Snapshot value semantics**: `ResourceSnapshot` is a plain value (strings, vectors, DeviceInfo copies) — it stays valid forever after the call, holds no reference into any object, and the monitor owns nothing (no lifecycle, no shutdown, never mutates or retains any Runtime resource).
- **Scheduler independence**: monitoring and benchmarking are observation only. CPU usage, timings and allocation counts never influence selection — connecting measurements to scheduling is future work that this phase deliberately does not implement.
- **`vortyx_bench`** (`src/benchmark_main.cpp`): a standalone, manually-run benchmark tool over a workload-size ladder (1K/16K/256K/1M elements — all far below the Phase 4 1 GiB per-buffer safety cap), CPU always and Vulkan when a device exists. Deliberately **not** registered as a CTest test: timing runs do not belong in the pass/fail CI suite; the test suite pins benchmark *invariants* instead.

**Phase 7 (unchanged)**: the Basic Scheduler remains the deterministic execution-target selection layer (explicit requests honored verbatim, automatic `vulkan` > `cpu` policy over real availability); its API, policy and tests are exactly as delivered in v0.7.0. Phase 8 data does not reach it.

**Phase 6 (unchanged)**: the Task Queue remains the asynchronous FIFO execution layer bound to exactly ONE Virtual GPU; its API and tests are exactly as delivered in v0.6.0.

**Phase 5 (unchanged)**: the Virtual GPU remains the single logical compute device per explicitly chosen backend (`"cpu"`, `"vulkan"`), with no automatic selection and no silent fallback inside the Virtual GPU itself; its API and tests are exactly as delivered in v0.5.0.

**Not implemented yet** (later phases): Multi-GPU, load balancing, work stealing, priority scheduling, task graphs, memory pooling / suballocation, network workers, distributed computing, performance-based / resource-aware scheduling (no hardware metrics are consumed — the codebase has none, and Phase 8 deliberately does not connect its measurements to the Scheduler), advanced schedulers of any kind, FPGA/own hardware.

## Benchmark concepts (Phase 8)

| Concept | Meaning |
|---------|---------|
| Benchmark | The measurement layer (`vortyx::benchmark`). It measures the real path; it never computes, never allocates device memory of its own, never touches backends |
| `benchmark_vector_add(gpu, task, config)` | Times `config.warmup_iterations + config.iterations` calls of `gpu.execute(task)` on the CALLER-owned Virtual GPU. The GPU must be Ready and stay alive during the call; the benchmark never initializes, reconfigures, switches or shuts it down |
| Workload | The caller's `VectorAddTask` — its element count IS the workload size. `BenchmarkConfig` only controls repetition (no separate size field that could disagree with the task) |
| Warmup | Executed first, verified for correctness, NEVER included in statistics. `0` is allowed. A warmup failure aborts the benchmark (named as the abort point in the error) |
| Sample | One `steady_clock`-bracketed `execute()` call: allocation + upload + execution + readback + release, end to end, CPU wall-clock. Not "GPU time" — the Runtime does not expose internal boundaries |
| Statistics | `min`/`max` (exact ns), `average`/`median`/`stddev` (ns, deterministic), `throughput_elements_per_second` — from real samples only; computed by the pure `compute_timing_stats()` (empty samples / zero size refused) |
| Correctness | Every iteration (warmup + measured) verified against the host reference outside the timed window. Any failure ⇒ failed benchmark with the real Status and iteration index; verdict recorded in `correctness_verified` |
| Result | `BenchmarkResult`: status + error + backend + `DeviceInfo` + workload/iteration counts + `TimingStats` + verdict. `describe()` renders it for humans; `to_key_values()` exports a stable key=value schema (units in the keys) |
| Backend honesty | The measured backend is exactly the Virtual GPU's configured one. An unavailable backend ⇒ failed benchmark (`BackendUnavailable`, warmup abort) — the failed result still names THAT backend and is never rerouted to `cpu` |
| Determinism | Workload, iteration/warmup counts, statistics algorithm, correctness verdict and output schema are deterministic. Timing VALUES are not (they are measurements) |

## Resource Monitoring concepts (Phase 8)

| Concept | Meaning |
|---------|---------|
| Resource Monitor | The observation layer (`vortyx::monitor`). Stateless, no lifecycle (a stateless collector gets no fake `initialize()`/`shutdown()`), owns nothing, mutates nothing |
| `ResourceSnapshot` | One moment's observation, returned BY VALUE (all data copied): system facts + per-backend observations + ResourceManager accounting. Stays valid forever, no dangling references, no mutexes for the caller |
| `snapshot()` | System-only: `hardware_threads` from the standard library (nullopt when undeterminable). Vortyx sections explicitly unobserved (flags false, no invented values) |
| `snapshot(runtime)` | Full: system facts + every registered backend's real availability, its own unavailable reason and its own `DeviceInfo`, + the Phase 4 `ResourceStats` (live buffers/bytes, total allocations). Re-queries the Runtime — the same source of truth the Scheduler probe and executing Virtual GPUs use; no second discovery path, no platform-specific code |
| Unavailable values | Explicitly marked: `std::optional` = nullopt, validity flags = false. NEVER a fake 0 (0 can be a real measurement; "unknown" cannot) |
| Unsupported metrics | GPU utilization, temperature, power, instantaneous CPU utilization, current VRAM usage, fan speed, PCIe bandwidth: no field exists at all — absence is the honest representation |
| Scheduler independence | The monitor never feeds the Scheduler. Phase 7 policy stays `vulkan` > `cpu` over availability only; resource-aware scheduling is future work not implemented in Phase 8 |

Example — the Phase 8 application flow (this is what `main.cpp` and the Phase 8 tests exercise):

```cpp
// --- Observe: a point-in-time snapshot of what is really known ---------
vortyx::monitor::ResourceMonitor monitor;
vortyx::compute::Runtime runtime;
runtime.initialize();
vortyx::monitor::ResourceSnapshot snap = monitor.snapshot(runtime);
// snap.hardware_threads, snap.backends[i].available / .device /
// .unavailable_reason, snap.live_buffers / .live_bytes / .total_allocations

// --- Measure: the REAL execution path, end to end ----------------------
vortyx::vgpu::VirtualGpu gpu;            // caller owns the execution context
gpu.initialize();                         // explicit backend ("cpu" default)

vortyx::compute::VectorAddTask task;      // the workload (its size = workload size)
vortyx::benchmark::BenchmarkConfig config;
config.iterations = 10;                   // measured iterations (> 0)
config.warmup_iterations = 1;             // excluded from statistics

vortyx::benchmark::BenchmarkResult r =
    vortyx::benchmark::benchmark_vector_add(gpu, task, config);
if (r.status == vortyx::compute::Status::Ok) {
    // r.timing.min/average_ns/median_ns/max_ns, r.timing.throughput_...
    // r.correctness_verified == true (every iteration checked)
}
std::string human = vortyx::benchmark::describe(r);       // for logs/screens
auto machine = vortyx::benchmark::to_key_values(r);        // stable key=value

gpu.shutdown();
runtime.shutdown();                       // monitor owns neither; any order
```

## Virtual GPU concepts (Phase 5, unchanged and still in force)

| Concept | Meaning |
|---------|---------|
| Virtual GPU | The logical compute device an application talks to (`vortyx::vgpu::VirtualGpu`). Not a physical GPU, not new compute power — one explicit backend presented as a single object |
| VirtualGpuDesc | Configuration: the explicit backend name (`"cpu"`, `"vulkan"`). Default `"cpu"`. Deliberately nothing else in Phase 5 |
| State | `Uninitialized` (not usable yet) → `Ready` (usable) → `ShutDown` (usable again only after `initialize()`) |
| `backend_available()` | True only when the configured backend exists AND is usable on this system. Never faked; unavailable reasons are exposed verbatim |
| `execute(task)` / `execute(a, b, c)` | Synchronous vector addition on the configured backend — task-based or on explicit Buffer resources |
| `resources()` | The owned Runtime's Resource Manager (`nullptr` while not Ready). Buffer lifecycle rules from Phase 4 apply unchanged |

Software Vulkan implementations (e.g. Mesa lavapipe) are still honestly reported as **software GPUs**, not hardware GPUs — the Virtual GPU reports whatever its backend reports, never more.

Example — the Phase 5 application flow (this is what `main.cpp` and the Virtual GPU tests exercise):

```cpp
vortyx::vgpu::VirtualGpu gpu;
vortyx::vgpu::VirtualGpuDesc desc;
desc.backend = "cpu";                 // explicit choice; no automatic selection
gpu.initialize(desc);

vortyx::compute::VectorAddTask task;
task.a = {1, 2, 3, 4};
task.b = {10, 20, 30, 40};

auto result = gpu.execute(task);      // synchronous; no Vulkan types anywhere
if (result.status == vortyx::compute::Status::Ok) {
    // result.data == {11, 22, 33, 44}
}

gpu.shutdown();
```

Requesting `"vulkan"` on a GPU-less machine works the same way: `initialize()` succeeds (a known backend is a valid configuration), `backend_available()` returns `false` with the honest reason, and `execute()` returns `Status::BackendUnavailable` — it never pretends to run on a GPU and never silently falls back to the CPU. Want CPU compute? Create a CPU Virtual GPU explicitly.

## Scheduler concepts (Phase 7)

| Concept | Meaning |
|---------|---------|
| Scheduler | The execution-target selection layer (`vortyx::scheduler::Scheduler`). Selects WHERE to run; never computes, never executes, never replaces the TaskQueue worker |
| Probe Runtime | The Scheduler's private, read-only Compute Runtime — the honest source of backend availability. Independent of every application Virtual GPU; no shared lifecycle |
| SelectionMode | `Automatic` (documented fixed policy) or `ExplicitBackend` (one named backend, honored verbatim) |
| SelectionRequest | `mode` + `backend`. In Automatic mode `backend` must stay empty (a conflicting request is refused with `InvalidInput`, not guessed about) |
| SelectionResult | `status` + `error` + the chosen `backend` + the concrete `device` (`DeviceInfo`) + the decision `reason` — the selected execution context, not a computation result |
| Automatic priority | Fixed, documented order `vulkan` > `cpu` (`Scheduler::automatic_priority()`); the first candidate that is REALLY available wins. Functional offload rule, not a performance claim |
| Unavailable backend | Never a success target. Explicit requests fail with the backend's real reason (no silent fallback); automatic mode simply skips unusable candidates (that fallback IS the documented policy) |
| Determinism | The policy is a pure function of the probed candidates: same system state → same selection, with the same explanation |
| Not selected | Load, VRAM, temperature, latency, priorities, multi-GPU — none of it exists in Phase 7 and none of it is invented |

Example — the Phase 7 application flow (this is what `main.cpp` and the Scheduler tests exercise):

```cpp
vortyx::scheduler::Scheduler scheduler;
scheduler.initialize();

// Automatic: documented policy over real availability (vulkan > cpu).
vortyx::scheduler::SelectionResult selection =
    scheduler.select(vortyx::scheduler::SelectionRequest{});
// selection.backend == "vulkan" when a Vulkan device is really usable,
// "cpu" otherwise; selection.reason explains the decision.

// The selection feeds the UNCHANGED execution path:
vortyx::vgpu::VirtualGpuDesc desc;
desc.backend = selection.backend;          // the Scheduler's choice, verbatim
gpu.initialize(desc);
// ... TaskQueue / execute as before ...

scheduler.shutdown();   // shares nothing with the Virtual GPUs: any order is safe
```

Requesting a specific backend explicitly is just as direct — and just as honest:

```cpp
vortyx::scheduler::SelectionRequest request;
request.mode = vortyx::scheduler::SelectionMode::ExplicitBackend;
request.backend = "vulkan";   // a canonical Runtime backend name
const auto result = scheduler.select(request);
// Available device: Status::Ok, result.backend == "vulkan".
// No device: Status::BackendUnavailable with the REAL reason —
// the request is never silently remapped to "cpu".
```

## Task Queue concepts (Phase 6, unchanged and still in force)

| Concept | Meaning |
|---------|---------|
| TaskQueue | One FIFO queue bound to exactly ONE Virtual GPU (`initialize(gpu)`). Not a scheduler: no device choice, no priorities, no reordering — ever |
| TaskId | Unique, monotonically increasing, never reused (0 = invalid). Stays safe across shutdown/re-initialization |
| TaskState | `Queued` (accepted, waiting) → `Running` (worker executing it now) → `Completed` / `Failed` (terminal). No `Cancelled`: not implemented in Phase 6 |
| `enqueue(task)` | Validates the task, appends it to the FIFO, returns `EnqueueResult{ id, status, error }` immediately — never blocks on execution |
| `task_state(id)` / `task_snapshot(id)` | Non-blocking state query; snapshot returns state + recorded result by value (result is meaningful only in a terminal state) |
| `wait(id)` / `wait_for(id, timeout)` | Block until the task is terminal (or at most `timeout`); already-terminal tasks return immediately; unknown ids return `Invalid` |
| Worker | Exactly ONE worker thread per queue: condition-variable wait (no busy-wait), pop front, execute **without holding the queue lock**, record result, repeat |
| Shutdown | Policy A: refuse new tasks → drain every accepted task FIFO → join worker. Double/concurrent shutdown safe; destructor joins the worker |
| Ownership | The queue REFERENCES its Virtual GPU (non-owning). Contract: shut the queue down before the Virtual GPU; violating it fails tasks honestly instead of crashing |

Example — the Phase 6 application flow (this is what `main.cpp` and the Task Queue tests exercise):

```cpp
vortyx::vgpu::VirtualGpu gpu;
gpu.initialize();                      // explicit cpu backend (default)

vortyx::queue::TaskQueue queue;
queue.initialize(gpu);                 // binds the queue to THIS Virtual GPU

vortyx::queue::EnqueueResult r = queue.enqueue(task);   // returns immediately
vortyx::queue::TaskState state = queue.wait(r.id);      // blocks until terminal
auto snap = queue.task_snapshot(r.id);
if (snap.state == vortyx::queue::TaskState::Completed) {
    // snap.result.data == A+B
}

queue.shutdown();                      // worker joined, all tasks processed
gpu.shutdown();                        // Virtual GPU shuts down AFTER the queue
```

## Resource concepts (Phase 4, unchanged and still in force)

| Concept | Meaning |
|---------|---------|
| Buffer resource | The single resource kind: an array of N elements × M bytes with a declared access role |
| BufferDesc | Creation description: element count, element size, `ResourceAccess` (`Read` and/or `Write`) |
| MemoryLocation | `Host` (plain CPU memory) or `Device` (e.g. Vulkan `VkDeviceMemory`). CPU and GPU memory are never treated as the same thing |
| Upload / download | `Buffer::write()` (host → resource) and `Buffer::read()` (resource → host) — the only data-movement primitives, with size/null/zero validation |
| Ownership | The Resource Manager registry owns the real storage; `Buffer` is a move-only RAII handle that triggers exactly one release; `Runtime::shutdown()` purges everything while devices still exist |

- A **CPU** buffer is ordinary host memory. `memory_location()` reports `Host`.
- A **GPU** (Vulkan) buffer is a `VkBuffer` bound to a `VkDeviceMemory` allocation. `memory_location()` reports `Device`; the host can only move data in/out through `write()`/`read()` (currently backed by one persistent mapping of host-visible coherent memory — staging transfers are a future optimization, not an implemented feature).

## Detection & Compute Methods

| Target | Windows (primary) | Linux |
|--------|-------------------|-------|
| Device discovery (Phase 2) | Win32 (`GetNativeSystemInfo`, ...) + CPUID / DXGI adapter enumeration | `/proc/cpuinfo`, `/proc/meminfo` / sysfs PCI scan |
| GPU compute (Phase 3) | Vulkan (compute pipeline) | Vulkan (compute pipeline) |
| Resource management (Phase 4) | Shared abstraction over host memory and Vulkan device memory | same |
| Virtual GPU (Phase 5) | Backend-agnostic logical device over the Runtime (`cpu` / `vulkan`) | same |
| Task Queue (Phase 6) | FIFO queue + one worker thread over any explicitly chosen Virtual GPU | same |
| Basic Scheduler (Phase 7) | Execution-target selection (explicit request or automatic `vulkan` > `cpu` policy) from real backend availability | same |
| Benchmark (Phase 8) | `steady_clock` timing around real `VirtualGpu::execute()` calls (end-to-end samples), warmup + measured iterations, statistics, per-iteration correctness | same |
| Resource Monitoring (Phase 8) | Point-in-time `ResourceSnapshot` over the Runtime's real backend/device/allocation state (no platform-specific code, no second discovery path) | same |

- DXGI is **discovery-only**; GPU computation goes through the Vulkan backend. The Virtual GPU never touches DXGI or Vulkan.
- Vulkan was chosen because it is free/open-source, Windows-first friendly, compute-capable without any windowing system, and aligns with the long-term Vortyx roadmap.
- The SPIR-V kernel (`shaders/vector_add.comp`) is **pre-compiled and embedded** into the binary (`src/core/compute/vector_add_spv.hpp`). Building requires no shader compiler, and nothing is downloaded at runtime.

## Requirements

| Configuration | Build-time | Runtime |
|---------------|-----------|---------|
| CPU-only (default fallback) | CMake 3.16+, C++17 compiler | nothing special |
| GPU compute enabled | CMake + **Vulkan headers/loader** (Vulkan SDK, or `libvulkan-dev` on Linux) | a Vulkan device: GPU driver, or a software implementation (e.g. `mesa-vulkan-drivers` / lavapipe) |

- Windows: install the [LunarG Vulkan SDK](https://vulkan.lunarg.com/) (free). CMake finds it automatically via the `VULKAN_SDK` environment variable.
- Linux: `sudo apt install libvulkan-dev mesa-vulkan-drivers` (headers + software Vulkan device for testing).
- If Vulkan is not found, CMake prints `Vortyx: Vulkan not found - building CPU-only` and everything still builds and passes tests. The CPU Virtual GPU works identically in a CPU-only build; a Vulkan Virtual GPU then reports its backend as unavailable with the exact reason.

## Build and Run

```powershell
# Windows (Developer Command Prompt or PowerShell, Visual Studio Build Tools required)
cmake -B build -S .
cmake --build build --config Release
.\build\Release\vortyx.exe

# CPU-only build (no Vulkan SDK needed)
cmake -B build -S . -DVORTYX_ENABLE_VULKAN=OFF

# Run tests
ctest --test-dir build -C Release
```

```bash
# Linux / macOS
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/vortyx

# CPU-only build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DVORTYX_ENABLE_VULKAN=OFF

# Run tests
ctest --test-dir build -C Release
```

Example output — **actual devices and timing numbers depend on the machine; timings below are one real run on the example box and are measurements, not claims** (example: Linux box, CPU only, Vulkan provided by the Mesa software implementation):

```
========================================
  Vortyx GPU
  Version: 0.9.0
  Phase:   9 (Stabilization)
  Build:   Release
========================================
[INFO] Vortyx started.
[INFO] GPU discovery ran successfully, found 0 GPU device(s).
[INFO] Discovered 1 device(s): 1 CPU, 0 GPU.
[INFO]   Device 0: CPU: Intel(R) Xeon(R) Processor | vendor: Intel | 2 logical processors | 2 physical cores | RAM 3.9 GiB (via linux-procfs)
[INFO] Vulkan backend ready: physical device 'llvmpipe (LLVM 19.1.7, 256 bits)' (software/CPU implementation, Vulkan API 1.4)
[INFO] Vulkan buffer resource provider registered (Phase 4 resource layer)
[INFO] Compute Runtime initialized. Available backends: cpu, vulkan
[INFO] Resource Manager ready. Buffer providers: cpu, vulkan
[INFO] Virtual GPU initialized (backend: cpu, state: Ready, device: Intel(R) Xeon(R) Processor)
[INFO] CPU Virtual GPU ready: backend='cpu', state=Ready, device: CPU: Intel(R) Xeon(R) Processor | vendor: Intel | 2 logical processors | 2 physical cores | RAM 3.9 GiB
[INFO] Virtual GPU (cpu) task execution success: C = A + B (11 22 33 44 55 66 77 88)
[INFO] Virtual GPU (cpu) resource execution success: C = A + B (11 22 33 44 55 66 77 88)
[INFO] CPU Virtual GPU resource stats: 0 live buffer(s), 0 live byte(s), 6 total allocation(s).
[INFO] Virtual GPU shut down.
[INFO] Vulkan backend ready: physical device 'llvmpipe (LLVM 19.1.7, 256 bits)' (software/CPU implementation, Vulkan API 1.4)
[INFO] Vulkan buffer resource provider registered (Phase 4 resource layer)
[INFO] Compute Runtime initialized. Available backends: cpu, vulkan
[INFO] Resource Manager ready. Buffer providers: cpu, vulkan
[INFO] Virtual GPU initialized (backend: vulkan, state: Ready, device: llvmpipe (LLVM 19.1.7, 256 bits))
[INFO] Vulkan Virtual GPU ready: backend='vulkan', device: llvmpipe (LLVM 19.1.7, 256 bits) (software/CPU implementation - not a hardware GPU)
[INFO] Device details: Software GPU: llvmpipe (LLVM 19.1.7, 256 bits) | vendor: Mesa | VRAM 3.9 GiB | id: vulkan-vendor0x10005-device0x0000-api1.4
[INFO] Virtual GPU (vulkan) task execution success: C = A + B (11 22 33 44 55 66 77 88)
[INFO] Result verification: Virtual GPU (vulkan) output matches Virtual GPU (cpu) output.
[INFO] Virtual GPU (vulkan) resource execution success: C = A + B (11 22 33 44 55 66 77 88)
[INFO] Resource verification: vulkan buffer output matches cpu buffer output.
[INFO] Virtual GPU shut down.
[INFO] Post-shutdown execute() refused as expected: NotInitialized - Virtual GPU is shut down (call initialize() again before execute())
[INFO] Vulkan backend ready: physical device 'llvmpipe (LLVM 19.1.7, 256 bits)' (software/CPU implementation, Vulkan API 1.4)
[INFO] Vulkan buffer resource provider registered (Phase 4 resource layer)
[INFO] Compute Runtime initialized. Available backends: cpu, vulkan
[INFO] Resource Manager ready. Buffer providers: cpu, vulkan
[INFO] Virtual GPU initialized (backend: cpu, state: Ready, device: Intel(R) Xeon(R) Processor)
[INFO] Task Queue initialized (worker: 1 thread, execution target: 'cpu', FIFO).
[INFO] Task Queue ready: state=Ready, execution target='cpu', policy: FIFO, worker: 1 thread.
[INFO] Enqueued task 1 (4 elements, worker executes FIFO in the background).
[INFO] Enqueued task 2 (4 elements, worker executes FIFO in the background).
[INFO] Enqueued task 3 (4 elements, worker executes FIFO in the background).
[INFO] Enqueued task 4 (4 elements, worker executes FIFO in the background).
[INFO] Task 1: state=Completed, C = A + B (11 22 33 44) [verified]
[INFO] Task 2: state=Completed, C = A + B (12 24 36 48) [verified]
[INFO] Task 3: state=Completed, C = A + B (13 26 39 52) [verified]
[INFO] Task 4: state=Completed, C = A + B (14 28 42 56) [verified]
[INFO] Task Queue verification: all queued tasks completed FIFO with correct results.
[INFO] Task Queue shut down (worker joined, all accepted tasks processed).
[INFO] Queue shut down: state=ShutDown, task states remain queryable until the queue is destroyed.
[INFO] Virtual GPU shut down.
[INFO] Vulkan backend ready: physical device 'llvmpipe (LLVM 19.1.7, 256 bits)' (software/CPU implementation, Vulkan API 1.4)
[INFO] Vulkan buffer resource provider registered (Phase 4 resource layer)
[INFO] Compute Runtime initialized. Available backends: cpu, vulkan
[INFO] Resource Manager ready. Buffer providers: cpu, vulkan
[INFO] Scheduler initialized (probe Runtime ready, available backends: vulkan, cpu).
[INFO] Scheduler selected backend 'vulkan' (automatic policy: 'vulkan' is the highest-priority available backend (priority order: 'vulkan' > 'cpu')).
[INFO] Automatic selection: backend='vulkan', device: llvmpipe (LLVM 19.1.7, 256 bits)
[INFO] Selection reason: automatic policy: 'vulkan' is the highest-priority available backend (priority order: 'vulkan' > 'cpu')
[INFO] Vulkan backend ready: physical device 'llvmpipe (LLVM 19.1.7, 256 bits)' (software/CPU implementation, Vulkan API 1.4)
[INFO] Vulkan buffer resource provider registered (Phase 4 resource layer)
[INFO] Compute Runtime initialized. Available backends: cpu, vulkan
[INFO] Resource Manager ready. Buffer providers: cpu, vulkan
[INFO] Virtual GPU initialized (backend: vulkan, state: Ready, device: llvmpipe (LLVM 19.1.7, 256 bits))
[INFO] Execution on the selected backend ('vulkan'): C = A + B (11 22 33 44 55 66 77 88)
[INFO] Virtual GPU shut down.
[INFO] Scheduler selected backend 'cpu' (explicit request honored: backend 'cpu' is registered and available on this system).
[INFO] Explicit selection: backend='cpu' (explicit request honored: backend 'cpu' is registered and available on this system)
[INFO] Vulkan backend ready: physical device 'llvmpipe (LLVM 19.1.7, 256 bits)' (software/CPU implementation, Vulkan API 1.4)
[INFO] Vulkan buffer resource provider registered (Phase 4 resource layer)
[INFO] Compute Runtime initialized. Available backends: cpu, vulkan
[INFO] Resource Manager ready. Buffer providers: cpu, vulkan
[INFO] Virtual GPU initialized (backend: cpu, state: Ready, device: Intel(R) Xeon(R) Processor)
[INFO] Verification: explicit-cpu execution matches the automatically selected backend's output.
[INFO] Virtual GPU shut down.
[INFO] Scheduler selected backend 'vulkan' (explicit request honored: backend 'vulkan' is registered and available on this system).
[INFO] Explicit selection: backend='vulkan', device: llvmpipe (LLVM 19.1.7, 256 bits)
[INFO] Scheduler shut down.
[INFO] Vulkan backend ready: physical device 'llvmpipe (LLVM 19.1.7, 256 bits)' (software/CPU implementation, Vulkan API 1.4)
[INFO] Vulkan buffer resource provider registered (Phase 4 resource layer)
[INFO] Compute Runtime initialized. Available backends: cpu, vulkan
[INFO] Resource Manager ready. Buffer providers: cpu, vulkan
[INFO] Environment observation:
[INFO] Resource snapshot: hardware threads: 2; backends observed: 2 (2 available)
[INFO]   backend 'cpu': available, device: Intel(R) Xeon(R) Processor
[INFO]   backend 'vulkan': available, device: llvmpipe (LLVM 19.1.7, 256 bits)
[INFO]   resources: 0 live buffer(s), 0 live byte(s), 0 total allocation(s)
[INFO] Vulkan backend ready: physical device 'llvmpipe (LLVM 19.1.7, 256 bits)' (software/CPU implementation, Vulkan API 1.4)
[INFO] Vulkan buffer resource provider registered (Phase 4 resource layer)
[INFO] Compute Runtime initialized. Available backends: cpu, vulkan
[INFO] Resource Manager ready. Buffer providers: cpu, vulkan
[INFO] Virtual GPU initialized (backend: cpu, state: Ready, device: Intel(R) Xeon(R) Processor)
[INFO] Benchmark 'vector_add' on 'cpu': 8192 elements x 10 iterations (1 warmup, excluded): avg 3.794 us, correctness verified.
[INFO] Benchmark 'vector_add' on backend 'cpu' (device: Intel(R) Xeon(R) Processor): 8192 elements, 10 measured iterations (1 warmup, excluded): min 3.708 us | avg 3.794 us | median 3.779 us | max 4.060 us | stddev 95 ns | throughput 2.16 Gelem/s | correctness: PASS
[INFO] Benchmark key=value export: workload=vector_add status=Ok backend=cpu device_type=Cpu device_name=Intel(R) Xeon(R) Processor element_count=8192 warmup_iterations=1 iterations=10 min_ns=3708 average_ns=3793.600 median_ns=3779.000 max_ns=4060 stddev_ns=95.396 throughput_elements_per_second=2159426402.362 correctness_verified=true
[INFO] Post-benchmark resource stats: 0 live buffer(s), 0 live byte(s), 33 total allocation(s) (live must be 0: benchmark buffers are RAII).
[INFO] Virtual GPU shut down.
[INFO] Vulkan backend ready: physical device 'llvmpipe (LLVM 19.1.7, 256 bits)' (software/CPU implementation, Vulkan API 1.4)
[INFO] Vulkan buffer resource provider registered (Phase 4 resource layer)
[INFO] Compute Runtime initialized. Available backends: cpu, vulkan
[INFO] Resource Manager ready. Buffer providers: cpu, vulkan
[INFO] Virtual GPU initialized (backend: vulkan, state: Ready, device: llvmpipe (LLVM 19.1.7, 256 bits))
[INFO] Benchmark 'vector_add' on 'vulkan': 8192 elements x 10 iterations (1 warmup, excluded): avg 150.583 us, correctness verified.
[INFO] Benchmark 'vector_add' on backend 'vulkan' (device: llvmpipe (LLVM 19.1.7, 256 bits)): 8192 elements, 10 measured iterations (1 warmup, excluded): min 140.741 us | avg 150.583 us | median 147.536 us | max 173.845 us | stddev 9.637 us | throughput 54.40 Melem/s | correctness: PASS
[INFO] Verification: vulkan benchmark target output matches the host reference (bit-exact).
[INFO] Virtual GPU shut down.
[INFO] Vulkan backend ready: physical device 'llvmpipe (LLVM 19.1.7, 256 bits)' (software/CPU implementation, Vulkan API 1.4)
[INFO] Vulkan buffer resource provider registered (Phase 4 resource layer)
[INFO] Compute Runtime initialized. Available backends: cpu, vulkan
[INFO] Resource Manager ready. Buffer providers: cpu, vulkan
[INFO] Post-benchmark observation: hardware threads: 2, backends available: 2/2.
[INFO] Hardware discovery: implemented (Phase 2).
[INFO] Compute Runtime: implemented (Phase 3) - CPU backend always available, Vulkan GPU backend when a Vulkan device is present.
[INFO] Compute Resource Manager: implemented (Phase 4) - Buffer resources with explicit host/device memory, upload/download, RAII ownership and safe shutdown.
[INFO] Virtual GPU: implemented (Phase 5) - one logical compute device per explicitly chosen backend; no automatic backend choice, no silent fallback.
[INFO] Task Queue: implemented (Phase 6) - FIFO task submission, one worker thread, asynchronous execution, per-task id/state/result, drain-on-shutdown.
[INFO] Basic Scheduler: implemented (Phase 7) - deterministic execution-target selection (explicit request or automatic vulkan>cpu policy) from real backend availability; selection only, execution stays in the Virtual GPU path.
[INFO] Benchmark: implemented (Phase 8) - real-path measurement (VirtualGpu::execute end to end) with warmup, repeated iterations, min/average/median/max statistics, throughput and per-iteration correctness verification; measurements only, no performance claims.
[INFO] Resource Monitoring: implemented (Phase 8) - point-in-time ResourceSnapshots over the Runtime's real backend/device/allocation state; unsupported metrics have no representation instead of fake values; informationally independent of the Scheduler.
[INFO] Stabilization: implemented (Phase 9) - full stability audit of Phase 1~8; foreign Resource/Buffer handles from another Runtime are now rejected explicitly instead of being silently resolved by colliding per-manager ids; Vulkan execution failures preserve the failing Vulkan call and its VkResult; Runtime/VirtualGpu threading contracts and Buffer::valid() semantics documented; CI verifies the CPU-only build explicitly.
[INFO] Not implemented yet: Multi-GPU, load balancing, work stealing, priority scheduling, Distributed Computing, Advanced/Resource-Aware Scheduling (benchmark and monitoring data deliberately do NOT influence the Scheduler).
```

On a machine without any Vulkan device (or in a CPU-only build), the program instead prints `Vulkan Virtual GPU is not usable on this system: <reason>`, the automatic Scheduler selection becomes `backend='cpu'` with the real reason in its explanation (`'vulkan' is not usable on this system (...)`), the explicit `'vulkan'` selection is honestly refused with `No fallback was attempted` — never silently rerouted — and the Vulkan benchmark prints an explicit skip line (`Vulkan benchmark skipped: backend not usable on this system (...)`). The monitoring snapshot on such a machine shows the vulkan backend as `unavailable` with its real reason. The standalone `vortyx_bench` tool behaves the same way (`-> SKIP`; no fallback, nothing faked).

## Project Structure

```
Vortyx-GPU/
├── .github/workflows/ci.yml        # CI (Windows + Ubuntu, GPU tests where possible)
├── CMakeLists.txt                  # Root build (VORTYX_ENABLE_VULKAN option)
├── shaders/
│   └── vector_add.comp             # Vector addition compute kernel (GLSL source)
├── scripts/
│   └── embed_spv.py                # Embeds compiled SPIR-V into a C++ header
├── src/
│   ├── main.cpp                    # Discovery + Virtual GPU + Queue + Scheduler demo + Phase 8 benchmark/monitoring demo
│   ├── benchmark_main.cpp          # vortyx_bench: standalone benchmark tool (manual run, not a CI test)
│   └── core/
│       ├── version.hpp / logger.*  # Phase 1 utilities
│       ├── device/                 # Phase 2 Hardware Discovery (unchanged core)
│       ├── compute/                # Phase 3/4 Compute Runtime
│       │   ├── task.hpp / .cpp     # VectorAddTask, Status, ComputeResult, validations
│       │   ├── backend.hpp         # IComputeBackend (buffer-based execution interface)
│       │   ├── cpu_backend.*       # CPU reference implementation (on host buffers)
│       │   ├── vulkan_backend.*    # Vulkan compute backend (stub without Vulkan)
│       │   ├── runtime.*           # Lifecycle + backend selection + task→buffer translation
│       │   └── vector_add_spv.hpp  # Embedded SPIR-V (generated, committed)
│       ├── resource/               # Phase 4 Compute Resource & Memory Management
│       │   ├── resource.hpp/.cpp   # BufferDesc, MemoryLocation, ResourceAccess, size validation
│       │   ├── backend_buffer.hpp  # IBufferImpl (real storage) + IBufferProvider (factory)
│       │   ├── buffer.hpp/.cpp     # Buffer move-only RAII handle, BufferResult
│       │   ├── resource_manager.*  # Registry, providers, stats, shutdown purge
│       │   ├── cpu_buffer.*        # Host-memory buffer implementation
│       │   └── vulkan_buffer.*     # VkBuffer + VkDeviceMemory implementation (Vulkan builds)
│       ├── vgpu/                   # Phase 5 Virtual GPU Interface
│       │   └── virtual_gpu.hpp/.cpp  # VirtualGpu (logical device), VirtualGpuDesc, State
│       ├── queue/                  # Phase 6 Task Queue & Async Execution
│       │   └── task_queue.hpp/.cpp   # TaskQueue (FIFO + 1 worker), QueuedTask, TaskId, TaskState
│       └── scheduler/              # Phase 7 Basic Scheduler
│       │   └── scheduler.hpp/.cpp    # Scheduler (selection only), SelectionRequest/Result, pure policy
│       ├── benchmark/              # Phase 8 Benchmark
│       │   └── benchmark.hpp/.cpp    # Real-path measurement, BenchmarkConfig/Result, TimingStats, pure statistics
│       └── monitor/                # Phase 8 Resource Monitoring
│           └── monitor.hpp/.cpp      # ResourceMonitor (stateless), ResourceSnapshot, honest unavailable markers
└── tests/
    ├── test_benchmark.cpp         # Benchmark CPU path: must pass everywhere
    ├── test_benchmark_gpu.cpp     # Benchmark GPU path: real tests when Vulkan available
    ├── test_monitor.cpp           # Resource Monitoring CPU path: must pass everywhere
    ├── test_monitor_gpu.cpp       # Resource Monitoring GPU path: real tests when Vulkan available
    ├── test_scheduler.cpp         # Basic Scheduler CPU path: must pass everywhere
    ├── test_scheduler_gpu.cpp     # Basic Scheduler GPU path: real tests when Vulkan available
    ├── test_taskqueue.cpp         # Task Queue CPU path: must pass everywhere
    ├── test_taskqueue_gpu.cpp     # Task Queue GPU path: real tests when Vulkan available
    ├── test_vgpu.cpp              # Virtual GPU CPU path: must pass everywhere
    ├── test_vgpu_gpu.cpp          # Virtual GPU GPU path: real tests when Vulkan available
    └── ... (Phase 1/2/3/4 tests unchanged)
```

## Testing

```bash
ctest --test-dir build -C Release --output-on-failure
```

| Test | What it verifies |
|------|------------------|
| VersionTest | Version constants match 0.9.0 |
| LoggerTest | Logger output format |
| DeviceDiscoveryTest | Phase 2 device discovery (unchanged, still passing) |
| ComputeCpuTest | Runtime lifecycle, CPU vector addition (sizes 4/16/1024/10007), invalid input handling, unknown/unavailable backends, shutdown/re-init — through the resource layer |
| ComputeGpuTest | When a Vulkan device exists: real GPU vector addition, bit-exact CPU-vs-GPU verification, repeated-run determinism, resource cleanup via re-init. Without a device: explicit SKIP note (never faked success) |
| ResourceTest | Full Buffer lifecycle on the CPU path: creation/info, write/read round-trips (full + partial), oversized/null/zero transfer rejection, zero-element/access/overflow/safety-cap rejection, unknown provider errors, invalid handles, move semantics (copy deleted, exactly-once ownership), RAII leak checks via stats, resource-based vector addition + validation errors (access roles, counts, element size, mixed/invalid handles), shutdown with live resources, handle outliving its Runtime, re-initialization, **foreign handles from another Runtime rejected with colliding ids (Phase 9 regression, both directions, no writes anywhere)** |
| ResourceGpuTest | When a Vulkan device exists: real `VkBuffer`/`VkDeviceMemory` allocation through the resource layer, `memory_location() == Device` honesty, full create→write→execute→read→release cycles, bit-exact CPU-vs-GPU resource results, oversized-transfer rejection, mixed-backend rejection, shutdown with live GPU buffers, re-initialization. Without a device: explicit SKIP note (never faked success) |
| VirtualGpuTest | Virtual GPU CPU path (every system): fresh-object state and refused operations, CPU initialization, vector addition vs reference, invalid tasks, unknown-backend early failure + recovery, idempotent re-init / refused reconfiguration, resource-based execution through `resources()`, dead-buffer rejection, **buffers of another Virtual GPU rejected via ownership verification (Phase 9 regression)**, honest known-but-unavailable backend behavior (no silent fallback), shutdown/re-init cycles, move semantics (exactly-once ownership, inert moved-from), buffer handles outliving their Virtual GPU |
| VirtualGpuGpuTest | When a Vulkan device exists: real execution through a Vulkan Virtual GPU, bit-exact match against an independently executed CPU Virtual GPU, `Device` memory honesty, cross-backend buffer rejection in both directions, determinism, shutdown with live resources + re-initialization. Without a device: explicit SKIP note (never faked success) |
| TaskQueueTest | Task Queue CPU path (every system): fresh-object refusals, init with non-Ready Virtual GPU refused, double-init refused, enqueue/wait/result correctness, id uniqueness/monotonicity, deterministic async proof (gated work item: `Running` observable, `enqueue()` non-blocking while the worker is busy, `wait_for` timeout path), FIFO order via order-recording work items, honest failure propagation (custom failing task, Runtime-rejected data, enqueue-time validation, null task), queue on an unavailable backend (adaptive: completes on real Vulkan, fails `BackendUnavailable` without one), shutdown drain policy, enqueue-after-shutdown refusal, records queryable after shutdown, double shutdown, Virtual-GPU-shutdown-first safety, destructor join, concurrent enqueue from 3 threads with distinct results, re-initialization cycles with ids never reused |
| TaskQueueGpuTest | When a Vulkan device exists: multiple VectorAddTasks queued on a Vulkan Virtual GPU, bit-exact match against independently executed CPU references, FIFO execution order on the GPU queue, determinism, queue-before-gpu shutdown order, enqueue refusal after shutdown. Without a device: explicit SKIP note (never faked success) |
| SchedulerTest | Basic Scheduler CPU path (every system): fresh-object refusals (`select()` before `initialize()` = `NotInitialized`), no-op shutdown, idempotent re-init, documented priority order pinned (`vulkan` > `cpu`), the pure policy over synthetic candidates (explicit available/unavailable/unknown/empty, automatic both-available/skipped-fallback/none-available/empty-list, determinism), adaptive real selections against an independently probed Virtual GPU (automatic matches reality, explicit `cpu` everywhere, explicit `vulkan` honored or honestly refused without remapping), unknown/malformed request refusals (`cuda`, empty explicit name, Automatic-with-backend conflict), selection device == executing Virtual GPU device, shutdown/re-init cycles, TaskQueue integration (selection → Virtual GPU → queue → bit-exact cpu-reference result), concurrent `select()` from 4 threads with consistent results |
| SchedulerGpuTest | When a Vulkan device exists: the automatic policy must select `vulkan` on the real device, the selection's device matches the executing Virtual GPU's device, explicit `vulkan`/`cpu` requests are honored verbatim, full integration (selection → Virtual GPU → TaskQueue → real GPU vector addition at sizes 4/64/1024/5000, bit-exact vs the cpu reference), repeated-selection determinism, Virtual GPUs keep working after the Scheduler shuts down. Without a device: explicit SKIP note (never faked success) |
| BenchmarkTest | Benchmark CPU path (every system): zero-iteration / size-mismatch / empty-task config validation, non-Ready Virtual GPU refusal, real-path CPU benchmark (iterations == request, min <= average <= max, median/stddev invariants, real non-zero samples), throughput consistency with element count / average, independent-execution agreement, machine-readable key=value export (backend, min_ns, correctness), human-readable `describe()`, repeated benchmarks (deterministic verdict; timings never compared), zero-warmup policy, honest unavailable-backend handling (adaptive: fails `BackendUnavailable` naming the vulkan target and the warmup abort point on GPU-less systems, succeeds on real devices), the pure statistics algorithm over hand-computed samples (exact min/max/average/median/stddev/throughput, empty/zero-size refusals, single sample, odd/even median), TaskQueue interference check (queue still FIFO-correct after benchmarking another Virtual GPU) |
| BenchmarkGpuTest | When a Vulkan device exists: real Vulkan-path benchmark (backend == 'vulkan', iterations == request, correctness verdict, timing invariants), device info matches the backend's own report (software implementations stay `SoftwareGpu`), bit-exact cross-check against an independent CPU reference execution, repeated benchmark determinism of the verdict, machine-readable export. Without a device: explicit SKIP note (never faked success) |
| MonitorTest | Monitoring CPU path (every system): system-only snapshot honesty (hardware threads positive when known; Vortyx sections explicitly unobserved, never fake values), full snapshot consistency with the Runtime's own answers (one observation per registered backend, availability/unavailable-reason/DeviceInfo equality, resource stats equality with the manager), shutdown Runtime reported unobserved, value semantics (earlier snapshots unchanged by later system mutations), exact live-buffer tracking across create/release (no leak), repeated snapshots deterministic and non-corrupting, Scheduler selection unchanged by monitoring (identical backend/reason before/after), read-only concurrent snapshots from 4 threads agree, no fabricated metric keys in the export, unavailable backends carry their real reason |
| MonitorGpuTest | When a Vulkan device exists: the snapshot observes the vulkan backend exactly as the Runtime and the executing Virtual GPU report it (availability, DeviceInfo equality, honest `Gpu`/`SoftwareGpu` kind), real Vulkan execution between snapshots changes no observation except allocation accounting (which returns to zero live buffers — RAII intact). Without a device: explicit SKIP note (never faked success) |

No test requires a specific GPU vendor or a GPU at all; machines with zero GPUs pass the full suite.

## Benchmark tool (vortyx_bench)

The `vortyx_bench` executable (built alongside `vortyx`, from `src/benchmark_main.cpp`) runs the measurement ladder manually — 1K/16K/256K/1M int32 elements on the CPU backend always and the Vulkan backend when a device is really available, with warmup and per-result correctness verification, printing both the human-readable form and the stable key=value export. It is deliberately **not** part of the CTest suite: the CI verifies benchmark *invariants* (via `BenchmarkTest`/`BenchmarkGpuTest`), not timing numbers, so CI stays fast and flake-free.

Run it yourself when you want actual measurements; treat every number it prints as a measurement of that run on that machine, not as a performance claim. In particular, the measured scope is the end-to-end `execute()` call (allocation + upload + execution + readback + release) for BOTH backends — comparable in scope, but these numbers say nothing about which backend a scheduler "should" choose, and Phase 8 deliberately draws no such conclusion.

## Roadmap

| Version | Planned Feature | Status |
|---------|-----------------|--------|
| 0.2 | Hardware Discovery (CPU/GPU detection) | Implemented |
| 0.3 | Basic Compute Runtime (CPU backend + Vulkan GPU backend, Vector Addition) | Implemented |
| 0.4 | Compute Resource & Memory Management (Buffer resources, Resource Manager, RAII ownership) | Implemented |
| 0.5 | Virtual GPU Interface (logical device over explicit backends) | Implemented |
| 0.6 | Task Queue and Async Execution (FIFO queue, one worker thread) | Implemented |
| 0.7 | Basic Scheduler (deterministic execution-target selection: explicit request or automatic `vulkan` > `cpu` policy) | Implemented |
| 0.8 | Benchmark + Resource Monitoring (real-path measurement with warmup/statistics/correctness; point-in-time resource snapshots over real state) | Implemented |
| 0.9 | Stabilization (full Phase 1~8 audit: foreign-buffer ownership enforcement, Vulkan error-cause preservation, lifecycle/threading contract documentation, CPU-only CI verification, regression tests) | **Implemented (current)** |
| 1.0 | Local GPU Computing Platform | Planned |

## License

MIT License
