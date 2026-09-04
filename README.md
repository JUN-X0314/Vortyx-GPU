# Vortyx GPU

[![CI](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml/badge.svg)](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml)

Vortyx GPU is an independent open-source project that researches and develops **GPU and GPU computing technology itself**, not AI services. AI is merely one of many application domains where Vortyx GPU can be utilized.

The long-term goal is to build a software-based GPU computing system, then evolve through Virtual GPU, multi-device computing, distributed computing, and FPGA prototypes, ultimately researching and developing Vortyx's own GPU hardware architecture.

## Current Phase: Phase 4 (v0.4.0) — Compute Resource & Memory Management

Phase 4 separates **the calculation** from **the resources the calculation needs**. Vortyx is no longer limited to running one hard-wired vector addition: there is now a resource layer that future Virtual GPU, Task Queue, Scheduler and Multi-GPU phases can build on.

```
Application → Runtime → Resource Manager → Backend → Device
```

**Implemented in Phase 4:**

- **Compute Resource abstraction** (`src/core/resource/`): a Buffer is described by a `BufferDesc` (element count × element size, byte size with overflow checking, declared `ResourceAccess` role: `Read` = device input, `Write` = device output) and reports honestly where its storage lives (`MemoryLocation::Host` vs `MemoryLocation::Device`).
- **Resource Manager**: allocates buffers through the explicitly requested backend's provider, tracks every live resource with monotonic never-reused ids, validates all host transfers, and exposes honest stats (live buffers/bytes, total allocations). It manages **lifecycle only** — it never selects devices, schedules or queues.
- **Buffer RAII handles**: move-only (copying a GPU-backed resource is forbidden), destroy exactly once, stale/moved-from/after-shutdown handles are inert (clean errors, safe no-op destruction, no double free).
- **CPU Backend on resources**: host-memory buffers (`CpuBuffer`), non-throwing allocation, plain-copy upload/download.
- **Vulkan Backend on resources**: `VulkanBuffer` owns a real `VkBuffer` + `VkDeviceMemory` (host-visible + host-coherent, mapped once) with full RAII; the backend binds the resource-owned buffers to the descriptor set and dispatches. The buffer creation code that lived inside the Phase 3 `execute()` was moved into this resource layer — there is exactly one buffer allocation path in the project.
- **Task API unchanged, now resource-backed**: `Runtime::execute(VectorAddTask)` still exists with the same signatures, but internally becomes: allocate 3 buffers via the Resource Manager → upload → dispatch → download → release (RAII). The Phase 3 behavior and results are unchanged.
- **Explicit resource lifecycle** (new API): `create_buffer → write → execute(a, b, c) → read → reset`, all validated: oversized/zero-byte/null transfers rejected, absurd allocation requests (overflow, > 1 GiB per buffer safety cap) fail cleanly, GPU allocation failures become errors instead of crashes.
- **Verification**: CPU and GPU vector addition through the identical resource API must match bit-exactly. Software Vulkan implementations (e.g. Mesa lavapipe) are still honestly reported as "Software GPU".

**Not implemented yet** (later phases): Virtual GPU (Phase 5), Task Queue / async execution (Phase 6), Scheduler (Phase 7), Multi-GPU, memory pooling / suballocation, network workers, distributed computing, benchmarks, FPGA/own hardware.

## Resource concepts

| Concept | Meaning |
|---------|---------|
| Buffer resource | The single resource kind in Phase 4: an array of N elements × M bytes with a declared access role |
| BufferDesc | Creation description: element count, element size, `ResourceAccess` (`Read` and/or `Write`) |
| MemoryLocation | `Host` (plain CPU memory) or `Device` (e.g. Vulkan `VkDeviceMemory`). CPU and GPU memory are never treated as the same thing |
| Upload / download | `Buffer::write()` (host → resource) and `Buffer::read()` (resource → host) — the only data-movement primitives, with size/null/zero validation |
| Ownership | The Resource Manager registry owns the real storage; `Buffer` is a move-only RAII handle that triggers exactly one release; `Runtime::shutdown()` purges everything while devices still exist |

CPU resource vs GPU resource, concretely:

- A **CPU** buffer is ordinary host memory. `memory_location()` reports `Host`.
- A **GPU** (Vulkan) buffer is a `VkBuffer` bound to a `VkDeviceMemory` allocation. `memory_location()` reports `Device`; the host can only move data in/out through `write()`/`read()` (currently backed by one persistent mapping of host-visible coherent memory — staging transfers are a future optimization, not a Phase 4 feature).

Example — the full resource lifecycle (this is what the CPU and GPU paths in `main.cpp` and the resource tests exercise):

```cpp
vortyx::compute::Runtime runtime;
runtime.initialize();

// 1. Create resources on an explicitly chosen backend.
const auto desc_in = vortyx::resource::BufferDesc::of<std::int32_t>(8, vortyx::resource::ResourceAccess::Read);
const auto desc_out = vortyx::resource::BufferDesc::of<std::int32_t>(8, vortyx::resource::ResourceAccess::Write);
auto a = runtime.resources().create_buffer(desc_in, "vulkan");   // or "cpu"
auto b = runtime.resources().create_buffer(desc_in, "vulkan");
auto c = runtime.resources().create_buffer(desc_out, "vulkan");

// 2. Upload input data.
const std::vector<std::int32_t> va = {1, 2, 3, 4, 5, 6, 7, 8};
const std::vector<std::int32_t> vb = {10, 20, 30, 40, 50, 60, 70, 80};
a.buffer.write(va.data(), va.size() * sizeof(std::int32_t));
b.buffer.write(vb.data(), vb.size() * sizeof(std::int32_t));

// 3. Compute directly on the resources (result stays inside c).
runtime.execute(a.buffer, b.buffer, c.buffer);

// 4. Download the result.
std::vector<std::int32_t> result(8);
c.buffer.read(result.data(), result.size() * sizeof(std::int32_t));

// 5. Release (RAII would also do this at scope exit).
a.buffer.reset();
b.buffer.reset();
c.buffer.reset();

runtime.shutdown();  // purges any resource still alive, then tears down backends
```

## Detection & Compute Methods

| Target | Windows (primary) | Linux |
|--------|-------------------|-------|
| Device discovery (Phase 2) | Win32 (`GetNativeSystemInfo`, ...) + CPUID / DXGI adapter enumeration | `/proc/cpuinfo`, `/proc/meminfo` / sysfs PCI scan |
| GPU compute (Phase 3) | Vulkan (compute pipeline) | Vulkan (compute pipeline) |
| Resource management (Phase 4) | Shared abstraction over host memory and Vulkan device memory | same |

- DXGI is **discovery-only**; GPU computation goes through the Vulkan backend.
- Vulkan was chosen because it is free/open-source, Windows-first friendly, compute-capable without any windowing system, and aligns with the long-term Vortyx roadmap.
- The SPIR-V kernel (`shaders/vector_add.comp`) is **pre-compiled and embedded** into the binary (`src/core/compute/vector_add_spv.hpp`). Building requires no shader compiler, and nothing is downloaded at runtime.

## Requirements

| Configuration | Build-time | Runtime |
|---------------|-----------|---------|
| CPU-only (default fallback) | CMake 3.16+, C++17 compiler | nothing special |
| GPU compute enabled | CMake + **Vulkan headers/loader** (Vulkan SDK, or `libvulkan-dev` on Linux) | a Vulkan device: GPU driver, or a software implementation (e.g. `mesa-vulkan-drivers` / lavapipe) |

- Windows: install the [LunarG Vulkan SDK](https://vulkan.lunarg.com/) (free). CMake finds it automatically via the `VULKAN_SDK` environment variable.
- Linux: `sudo apt install libvulkan-dev mesa-vulkan-drivers` (headers + software Vulkan device for testing).
- If Vulkan is not found, CMake prints `Vortyx: Vulkan not found - building CPU-only` and everything still builds and passes tests. The Resource Manager and the CPU resource path work identically in a CPU-only build; only the `vulkan` buffer provider is absent (requests for it fail with a descriptive `BackendUnavailable` error).

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
  Version: 0.4.0
  Phase:   4 (Compute Resource & Memory Management)
  Build:   Release
========================================
[INFO] Vortyx started.
[INFO] GPU discovery ran successfully, found 0 GPU device(s).
[INFO] Discovered 1 device(s): 1 CPU, 0 GPU.
[INFO]   Device 0: CPU: Intel(R) Xeon(R) Processor | vendor: Intel | 2 logical processors | 2 physical cores | RAM 4.1 GiB (via linux-procfs)
[INFO] Vulkan backend ready: physical device 'llvmpipe (LLVM 19.1.7, 256 bits)' (software/CPU implementation, Vulkan API 1.4)
[INFO] Vulkan buffer resource provider registered (Phase 4 resource layer)
[INFO] Compute Runtime initialized. Available backends: cpu, vulkan
[INFO] Resource Manager ready. Buffer providers: cpu, vulkan
[INFO] CPU execution success: C = A + B (11 22 33 44 55 66 77 88)
[INFO] GPU (Vulkan) execution success on 'llvmpipe (LLVM 19.1.7, 256 bits)': C = A + B (11 22 33 44 55 66 77 88)
[INFO] Result verification: GPU output matches CPU reference.
[INFO] Resource-based CPU execution success: C = A + B (11 22 33 44 55 66 77 88)
[INFO] Resource-based GPU (Vulkan) execution success on 'llvmpipe (LLVM 19.1.7, 256 bits)': C = A + B (11 22 33 44 55 66 77 88)
[INFO] Resource verification: GPU buffer output matches CPU output.
[INFO] Resource stats: 0 live buffer(s), 0 live byte(s), 12 total allocation(s) this session.
```

On a machine without any Vulkan device (or in a CPU-only build), the program instead prints `GPU (Vulkan) backend unavailable on this system: <reason>` and continues with CPU results.

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
│   ├── main.cpp                    # Discovery + compute + resource lifecycle demo
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
│       └── resource/               # Phase 4 Compute Resource & Memory Management
│           ├── resource.hpp/.cpp   # BufferDesc, MemoryLocation, ResourceAccess, size validation
│           ├── backend_buffer.hpp  # IBufferImpl (real storage) + IBufferProvider (factory)
│           ├── buffer.hpp/.cpp     # Buffer move-only RAII handle, BufferResult
│           ├── resource_manager.*  # Registry, providers, stats, shutdown purge
│           ├── cpu_buffer.*        # Host-memory buffer implementation
│           └── vulkan_buffer.*     # VkBuffer + VkDeviceMemory implementation (Vulkan builds)
└── tests/
    ├── test_resource.cpp           # Resource lifecycle, CPU path: must pass everywhere
    ├── test_resource_gpu.cpp       # Resource lifecycle, GPU path: real tests when Vulkan available
    └── ... (Phase 1/2/3 tests unchanged)
```

## Testing

```bash
ctest --test-dir build -C Release --output-on-failure
```

| Test | What it verifies |
|------|------------------|
| VersionTest | Version constants match 0.4.0 |
| LoggerTest | Logger output format |
| DeviceDiscoveryTest | Phase 2 device discovery (unchanged, still passing) |
| ComputeCpuTest | Runtime lifecycle, CPU vector addition (sizes 4/16/1024/10007), invalid input handling, unknown/unavailable backends, shutdown/re-init — now through the resource layer |
| ComputeGpuTest | When a Vulkan device exists: real GPU vector addition, bit-exact CPU-vs-GPU verification, repeated-run determinism, resource cleanup via re-init. Without a device: explicit SKIP note (never faked success) |
| ResourceTest | Full Buffer lifecycle on the CPU path: creation/info, write/read round-trips (full + partial), oversized/null/zero transfer rejection, zero-element/access/overflow/safety-cap rejection, unknown provider errors, invalid handles, move semantics (copy deleted, exactly-once ownership), RAII leak checks via stats, resource-based vector addition + validation errors (access roles, counts, element size, mixed/invalid handles), shutdown with live resources, handle outliving its Runtime, re-initialization |
| ResourceGpuTest | When a Vulkan device exists: real `VkBuffer`/`VkDeviceMemory` allocation through the resource layer, `memory_location() == Device` honesty, full create→write→execute→read→release cycles, bit-exact CPU-vs-GPU resource results, oversized-transfer rejection, mixed-backend rejection, shutdown with live GPU buffers, re-initialization. Without a device: explicit SKIP note (never faked success) |

No test requires a specific GPU vendor or a GPU at all; machines with zero GPUs pass the full suite.

## Roadmap

| Version | Planned Feature | Status |
|---------|-----------------|--------|
| 0.2 | Hardware Discovery (CPU/GPU detection) | Implemented |
| 0.3 | Basic Compute Runtime (CPU backend + Vulkan GPU backend, Vector Addition) | Implemented |
| 0.4 | Compute Resource & Memory Management (Buffer resources, Resource Manager, RAII ownership) | **Implemented (current)** |
| 0.5 | Virtual GPU Interface | Planned |
| 0.6 | Task Queue and Async Execution | Planned |
| 0.7 | Basic Scheduler | Planned |
| 1.0 | Local GPU Computing Platform | Planned |

## License

MIT License
