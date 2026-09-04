# Vortyx GPU

[![CI](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml/badge.svg)](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml)

Vortyx GPU is an independent open-source project that researches and develops **GPU and GPU computing technology itself**, not AI services. AI is merely one of many application domains where Vortyx GPU can be utilized.

The long-term goal is to build a software-based GPU computing system, then evolve through Virtual GPU, multi-device computing, distributed computing, and FPGA prototypes, ultimately researching and developing Vortyx's own GPU hardware architecture.

## Current Phase: Phase 5 (v0.5.0) — Virtual GPU Interface

Phase 5 gives applications **one logical compute device**: a Virtual GPU. An application executes compute tasks through Vortyx without knowing — or touching — Vulkan, DXGI, queue families, memory types or buffer internals. A Virtual GPU is **not a physical GPU** and does not create any new compute capability; it is an abstraction layer that represents one explicitly chosen execution backend as a single logical object.

```
Application → Virtual GPU → Compute Runtime → Resource Manager → Backend → Physical Device
```

**Implemented in Phase 5:**

- **`vortyx::vgpu::VirtualGpu`** (`src/core/vgpu/`): the application-facing logical GPU. It exclusively owns one Compute Runtime (`std::unique_ptr`), binds itself to ONE explicitly configured backend, and forwards execution to it. Contains no Vulkan, DXGI or platform types — backend independence is structural, not conventional.
- **Explicit backend selection, zero automatic choice**: the backend is fixed in the description (`VirtualGpuDesc::backend`, default `"cpu"`). There is NO scheduler, NO "GPU if available", NO silent fallback. If a Vulkan Virtual GPU is requested on a machine without a usable Vulkan device, its `execute()` honestly fails with `Status::BackendUnavailable` and the real reason — the CPU path stays available through a separately created CPU Virtual GPU.
- **Honest lifecycle states**: `Uninitialized → Ready → ShutDown`. `execute()` before `initialize()` or after `shutdown()` fails with a precise `Status::NotInitialized` message (the two cases are distinguishable). An *unknown* backend name fails early at `initialize()`; a *known-but-unavailable* backend initializes normally and its true condition is exposed through `backend_available()` / `backend_unavailable_reason()` / `device_info()`.
- **Task and resource execution**: `execute(VectorAddTask)` returns the computed vector; `execute(a, b, c)` runs directly on Buffer resources created through the Virtual GPU's Resource Manager. Buffers belonging to a different backend than the Virtual GPU's configuration are rejected with `Status::InvalidInput` — a Virtual GPU is one explicit execution target and never silently executes elsewhere.
- **Ownership and safety**: move-only (copying a live execution context is forbidden), the moved-from object is fully inert, `shutdown()` is safe to call multiple times, and re-initialization after `shutdown()` builds a fresh Runtime. Buffer handles that outlive their Virtual GPU fail cleanly and release as a no-op (Phase 4 contract).
- **Phase 2/3/4 APIs unchanged**: Hardware Discovery, `Runtime` and the Resource Manager keep their public APIs and all their tests; the Virtual GPU is a layer on top, not a replacement.

**Not implemented yet** (later phases): Task Queue / async execution (Phase 6), Scheduler (Phase 7), Multi-GPU, memory pooling / suballocation, network workers, distributed computing, benchmarks, FPGA/own hardware.

## Virtual GPU concepts

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
  Version: 0.5.0
  Phase:   5 (Virtual GPU Interface)
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
│       └── vgpu/                   # Phase 5 Virtual GPU Interface
│           └── virtual_gpu.hpp/.cpp  # VirtualGpu (logical device), VirtualGpuDesc, State
└── tests/
    ├── test_vgpu.cpp               # Virtual GPU CPU path: must pass everywhere
    ├── test_vgpu_gpu.cpp           # Virtual GPU GPU path: real tests when Vulkan available
    └── ... (Phase 1/2/3/4 tests unchanged)
```

## Testing

```bash
ctest --test-dir build -C Release --output-on-failure
```

| Test | What it verifies |
|------|------------------|
| VersionTest | Version constants match 0.5.0 |
| LoggerTest | Logger output format |
| DeviceDiscoveryTest | Phase 2 device discovery (unchanged, still passing) |
| ComputeCpuTest | Runtime lifecycle, CPU vector addition (sizes 4/16/1024/10007), invalid input handling, unknown/unavailable backends, shutdown/re-init — through the resource layer |
| ComputeGpuTest | When a Vulkan device exists: real GPU vector addition, bit-exact CPU-vs-GPU verification, repeated-run determinism, resource cleanup via re-init. Without a device: explicit SKIP note (never faked success) |
| ResourceTest | Full Buffer lifecycle on the CPU path: creation/info, write/read round-trips (full + partial), oversized/null/zero transfer rejection, zero-element/access/overflow/safety-cap rejection, unknown provider errors, invalid handles, move semantics (copy deleted, exactly-once ownership), RAII leak checks via stats, resource-based vector addition + validation errors (access roles, counts, element size, mixed/invalid handles), shutdown with live resources, handle outliving its Runtime, re-initialization |
| ResourceGpuTest | When a Vulkan device exists: real `VkBuffer`/`VkDeviceMemory` allocation through the resource layer, `memory_location() == Device` honesty, full create→write→execute→read→release cycles, bit-exact CPU-vs-GPU resource results, oversized-transfer rejection, mixed-backend rejection, shutdown with live GPU buffers, re-initialization. Without a device: explicit SKIP note (never faked success) |
| VirtualGpuTest | Virtual GPU CPU path (every system): fresh-object state and refused operations, CPU initialization, vector addition vs reference, invalid tasks, unknown-backend early failure + recovery, idempotent re-init / refused reconfiguration, resource-based execution through `resources()`, dead-buffer rejection, honest known-but-unavailable backend behavior (no silent fallback), shutdown/re-init cycles, move semantics (exactly-once ownership, inert moved-from), buffer handles outliving their Virtual GPU |
| VirtualGpuGpuTest | When a Vulkan device exists: real execution through a Vulkan Virtual GPU, bit-exact match against an independently executed CPU Virtual GPU, `Device` memory honesty, cross-backend buffer rejection in both directions, determinism, shutdown with live resources + re-initialization. Without a device: explicit SKIP note (never faked success) |

No test requires a specific GPU vendor or a GPU at all; machines with zero GPUs pass the full suite.

## Roadmap

| Version | Planned Feature | Status |
|---------|-----------------|--------|
| 0.2 | Hardware Discovery (CPU/GPU detection) | Implemented |
| 0.3 | Basic Compute Runtime (CPU backend + Vulkan GPU backend, Vector Addition) | Implemented |
| 0.4 | Compute Resource & Memory Management (Buffer resources, Resource Manager, RAII ownership) | Implemented |
| 0.5 | Virtual GPU Interface (logical device over explicit backends) | **Implemented (current)** |
| 0.6 | Task Queue and Async Execution | Planned |
| 0.7 | Basic Scheduler | Planned |
| 1.0 | Local GPU Computing Platform | Planned |

## License

MIT License
