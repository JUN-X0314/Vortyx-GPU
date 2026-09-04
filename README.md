# Vortyx GPU

[![CI](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml/badge.svg)](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml)

Vortyx GPU is an independent open-source project that researches and develops **GPU and GPU computing technology itself**, not AI services. AI is merely one of many application domains where Vortyx GPU can be utilized.

The long-term goal is to build a software-based GPU computing system, then evolve through Virtual GPU, multi-device computing, distributed computing, and FPGA prototypes, ultimately researching and developing Vortyx's own GPU hardware architecture.

## Current Phase: Phase 6 (v0.6.0) — Task Queue & Async Execution

Phase 6 adds a **real Task Queue** on top of the Virtual GPU: an application can submit several compute tasks up front, and a dedicated worker execution context processes them one at a time, asynchronously and strictly in FIFO order. Each task gets a unique id, an observable lifecycle (`Queued → Running → Completed/Failed`), and a queryable result. The Task Queue is **not a scheduler**: it never compares backends, never picks devices, and always uses exactly the one Virtual GPU it was initialized with.

```
Application → Task Queue → Virtual GPU → Compute Runtime → Resource Manager → Backend → Physical Device
```

**Implemented in Phase 6:**

- **`vortyx::queue::TaskQueue`** (`src/core/queue/`): FIFO task submission + asynchronous execution on one worker thread. `enqueue()` returns immediately with a `TaskId` — it never waits for execution. The worker waits on a condition variable (no busy-waiting), pops tasks strictly in FIFO order, executes them through the bound Virtual GPU, and records state + result per task.
- **Task model**: `QueuedTask` is the minimal "work the bound Virtual GPU can execute" abstraction; Phase 6 ships exactly one implementation (`VectorAddQueuedTask`, wrapping the Phase 3 `VectorAddTask` unchanged). Future task kinds derive from it without changing the queue.
- **Task identity**: `TaskId` follows the Phase 4 `ResourceId` pattern — 64-bit monotonic, 0 reserved as invalid, **never reused**, so a stale id can never alias a newer task (even across queue re-initialization).
- **Observable lifecycle**: `task_state(id)` / `task_snapshot(id)` (state + recorded result, returned by value) / `wait(id)` (blocks until terminal) / `wait_for(id, timeout)`. There is deliberately no `Cancelled` state: cancelling queued work is not implemented in Phase 6, and claiming it would be a lie.
- **Thread safety by structure**: one mutex guards all bookkeeping; the worker releases the mutex before executing (a long compute run never blocks submission or queries, and nothing below can deadlock against the queue's lock). Copying AND moving are deleted — the worker and every waiter hold `this`, so a queue has exactly one address for its whole life.
- **Shutdown policy A (drain)**: `shutdown()` refuses new tasks, lets the worker finish every accepted task FIFO, joins the thread, then returns. Safe to call multiple times and concurrently; the destructor always joins the worker (no leaked thread, no `std::terminate`). Documented order: **queue shuts down before its Virtual GPU** — and if the caller violates that order, pending tasks fail honestly instead of faking success.
- **Honest failures**: a task that cannot run (unavailable backend, shut-down Virtual GPU, invalid data caught by the Runtime) ends `Failed` with the real `Status` and reason — the queue never reports a fake `Completed`, never falls back to another backend.

**Phase 5 (unchanged)**: the Virtual GPU remains the single logical compute device per explicitly chosen backend (`"cpu"`, `"vulkan"`), with no automatic selection and no silent fallback; its API and tests are exactly as delivered in v0.5.0.

**Not implemented yet** (later phases): Scheduler (Phase 7), Multi-GPU, memory pooling / suballocation, network workers, distributed computing, benchmarks, FPGA/own hardware.

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

## Task Queue concepts (Phase 6)

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

Example output — **actual devices depend on the machine** (example: Linux box, CPU only, Vulkan provided by the Mesa software implementation):

```
========================================
  Vortyx GPU
  Version: 0.6.0
  Phase:   6 (Task Queue & Async Execution)
  Build:   Release
========================================
[INFO] Vortyx started.
[INFO] Discovered 1 device(s): 1 CPU, 0 GPU.
[INFO]   Device 0: CPU: Intel(R) Xeon(R) Processor | vendor: Intel | 2 logical processors | 2 physical cores | RAM 3.9 GiB (via linux-procfs)
[INFO] Virtual GPU initialized (backend: cpu, state: Ready, device: Intel(R) Xeon(R) Processor)
[INFO] CPU Virtual GPU ready: backend='cpu', state=Ready, device: CPU: Intel(R) Xeon(R) Processor | vendor: Intel | 2 logical processors | 2 physical cores | RAM 3.9 GiB
[INFO] Virtual GPU (cpu) task execution success: C = A + B (11 22 33 44 55 66 77 88)
[INFO] Virtual GPU (cpu) resource execution success: C = A + B (11 22 33 44 55 66 77 88)
[INFO] CPU Virtual GPU resource stats: 0 live buffer(s), 0 live byte(s), 6 total allocation(s).
[INFO] Virtual GPU shut down.
[INFO] Virtual GPU initialized (backend: vulkan, state: Ready, device: llvmpipe (LLVM 19.1.7, 256 bits))
[INFO] Vulkan Virtual GPU ready: backend='vulkan', device: llvmpipe (LLVM 19.1.7, 256 bits) (software/CPU implementation - not a hardware GPU)
[INFO] Virtual GPU (vulkan) task execution success: C = A + B (11 22 33 44 55 66 77 88)
[INFO] Result verification: Virtual GPU (vulkan) output matches Virtual GPU (cpu) output.
[INFO] Virtual GPU (vulkan) resource execution success: C = A + B (11 22 33 44 55 66 77 88)
[INFO] Resource verification: vulkan buffer output matches cpu buffer output.
[INFO] Virtual GPU shut down.
[INFO] Post-shutdown execute() refused as expected: NotInitialized - Virtual GPU is shut down (call initialize() again before execute())
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
[INFO] Hardware discovery: implemented (Phase 2).
[INFO] Compute Runtime: implemented (Phase 3) - CPU backend always available, Vulkan GPU backend when a Vulkan device is present.
[INFO] Compute Resource Manager: implemented (Phase 4) - Buffer resources with explicit host/device memory, upload/download, RAII ownership and safe shutdown.
[INFO] Virtual GPU: implemented (Phase 5) - one logical compute device per explicitly chosen backend; no automatic backend choice, no silent fallback.
[INFO] Task Queue: implemented (Phase 6) - FIFO task submission, one worker thread, asynchronous execution, per-task id/state/result, drain-on-shutdown.
[INFO] Not implemented yet: Scheduler (Phase 7), Multi-GPU, Distributed Computing.
```

On a machine without any Vulkan device (or in a CPU-only build), the program instead prints `Vulkan Virtual GPU is not usable on this system: <reason>` and `No fallback was attempted: the CPU Virtual GPU above ran on the explicitly configured cpu backend.` — and continues with the CPU results.

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
│   ├── main.cpp                    # Discovery + Virtual GPU demo (cpu + vulkan paths)
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
│       └── queue/                  # Phase 6 Task Queue & Async Execution
│           └── task_queue.hpp/.cpp   # TaskQueue (FIFO + 1 worker), QueuedTask, TaskId, TaskState
└── tests/
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
| VersionTest | Version constants match 0.6.0 |
| LoggerTest | Logger output format |
| DeviceDiscoveryTest | Phase 2 device discovery (unchanged, still passing) |
| ComputeCpuTest | Runtime lifecycle, CPU vector addition (sizes 4/16/1024/10007), invalid input handling, unknown/unavailable backends, shutdown/re-init — through the resource layer |
| ComputeGpuTest | When a Vulkan device exists: real GPU vector addition, bit-exact CPU-vs-GPU verification, repeated-run determinism, resource cleanup via re-init. Without a device: explicit SKIP note (never faked success) |
| ResourceTest | Full Buffer lifecycle on the CPU path: creation/info, write/read round-trips (full + partial), oversized/null/zero transfer rejection, zero-element/access/overflow/safety-cap rejection, unknown provider errors, invalid handles, move semantics (copy deleted, exactly-once ownership), RAII leak checks via stats, resource-based vector addition + validation errors (access roles, counts, element size, mixed/invalid handles), shutdown with live resources, handle outliving its Runtime, re-initialization |
| ResourceGpuTest | When a Vulkan device exists: real `VkBuffer`/`VkDeviceMemory` allocation through the resource layer, `memory_location() == Device` honesty, full create→write→execute→read→release cycles, bit-exact CPU-vs-GPU resource results, oversized-transfer rejection, mixed-backend rejection, shutdown with live GPU buffers, re-initialization. Without a device: explicit SKIP note (never faked success) |
| VirtualGpuTest | Virtual GPU CPU path (every system): fresh-object state and refused operations, CPU initialization, vector addition vs reference, invalid tasks, unknown-backend early failure + recovery, idempotent re-init / refused reconfiguration, resource-based execution through `resources()`, dead-buffer rejection, honest known-but-unavailable backend behavior (no silent fallback), shutdown/re-init cycles, move semantics (exactly-once ownership, inert moved-from), buffer handles outliving their Virtual GPU |
| VirtualGpuGpuTest | When a Vulkan device exists: real execution through a Vulkan Virtual GPU, bit-exact match against an independently executed CPU Virtual GPU, `Device` memory honesty, cross-backend buffer rejection in both directions, determinism, shutdown with live resources + re-initialization. Without a device: explicit SKIP note (never faked success) |
| TaskQueueTest | Task Queue CPU path (every system): fresh-object refusals, init with non-Ready Virtual GPU refused, double-init refused, enqueue/wait/result correctness, id uniqueness/monotonicity, deterministic async proof (gated work item: `Running` observable, `enqueue()` non-blocking while the worker is busy, `wait_for` timeout path), FIFO order via order-recording work items, honest failure propagation (custom failing task, Runtime-rejected data, enqueue-time validation, null task), queue on an unavailable backend (adaptive: completes on real Vulkan, fails `BackendUnavailable` without one), shutdown drain policy, enqueue-after-shutdown refusal, records queryable after shutdown, double shutdown, Virtual-GPU-shutdown-first safety, destructor join, concurrent enqueue from 3 threads with distinct results, re-initialization cycles with ids never reused |
| TaskQueueGpuTest | When a Vulkan device exists: multiple VectorAddTasks queued on a Vulkan Virtual GPU, bit-exact match against independently executed CPU references, FIFO execution order on the GPU queue, determinism, queue-before-gpu shutdown order, enqueue refusal after shutdown. Without a device: explicit SKIP note (never faked success) |

No test requires a specific GPU vendor or a GPU at all; machines with zero GPUs pass the full suite.

## Roadmap

| Version | Planned Feature | Status |
|---------|-----------------|--------|
| 0.2 | Hardware Discovery (CPU/GPU detection) | Implemented |
| 0.3 | Basic Compute Runtime (CPU backend + Vulkan GPU backend, Vector Addition) | Implemented |
| 0.4 | Compute Resource & Memory Management (Buffer resources, Resource Manager, RAII ownership) | Implemented |
| 0.5 | Virtual GPU Interface (logical device over explicit backends) | Implemented |
| 0.6 | Task Queue and Async Execution (FIFO queue, one worker thread) | **Implemented (current)** |
| 0.7 | Basic Scheduler | Planned |
| 1.0 | Local GPU Computing Platform | Planned |

## License

MIT License
