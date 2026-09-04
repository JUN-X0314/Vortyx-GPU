# Vortyx GPU

[![CI](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml/badge.svg)](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml)

Vortyx GPU is an independent open-source project that researches and develops **GPU and GPU computing technology itself**, not AI services. AI is merely one of many application domains where Vortyx GPU can be utilized.

The long-term goal is to build a software-based GPU computing system, then evolve through Virtual GPU, multi-device computing, distributed computing, and FPGA prototypes, ultimately researching and developing Vortyx's own GPU hardware architecture.

## Current Phase: Phase 3 (v0.3.0) — Compute Runtime

Phase 3 turns Vortyx from a device-detection program into a program that can **actually execute computation**:

```
Compute Task 생성 → Runtime 전달 → Backend 선택(명시적) → 실제 CPU/GPU 계산 → 결과 반환 → CPU 참조 결과와 검증
```

**Implemented in Phase 3:**

- **Compute Task**: Vector Addition (`C[i] = A[i] + B[i]`, int32). Chosen because it runs identically on CPU and GPU with bit-exact comparable results.
- **CPU Backend**: reference implementation, always available, computes the baseline result.
- **GPU Backend (Vulkan)**: real Vulkan compute pipeline — instance → physical device selection → logical device + compute queue → storage buffers → upload → dispatch (embedded SPIR-V) → readback. Compute-only: no graphics, no windowing.
- **Runtime**: owns backends, explicit lifecycle (`initialize` / `execute` / `shutdown`), explicit backend selection (`"cpu"`, `"vulkan"`). No automatic scheduling.
- **Verification**: on systems where the GPU backend works, the same task is executed on CPU and GPU and results must match exactly.
- **Graceful degradation**: no Vulkan driver, no Vulkan device, or a CPU-only build never crashes the program — the reason is reported and the CPU path keeps working. Software Vulkan implementations (e.g. Mesa lavapipe) are honestly reported as "Software GPU", not as real GPUs.

**Not implemented yet** (later phases): Scheduler, Virtual GPU, Task Queue / async execution, Multi-GPU, network workers, distributed computing, benchmarks, FPGA/own hardware.

## Detection & Compute Methods

| Target | Windows (primary) | Linux |
|--------|-------------------|-------|
| Device discovery (Phase 2) | Win32 (`GetNativeSystemInfo`, ...) + CPUID / DXGI adapter enumeration | `/proc/cpuinfo`, `/proc/meminfo` / sysfs PCI scan |
| GPU compute (Phase 3) | Vulkan (compute pipeline) | Vulkan (compute pipeline) |

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
- If Vulkan is not found, CMake prints `Vortyx: Vulkan not found - building CPU-only` and everything still builds and passes tests.

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
  Version: 0.3.0
  Phase:   3 (Compute Runtime)
  Build:   Release
========================================
[INFO] Vortyx started.
[INFO] GPU discovery ran successfully, found 0 GPU device(s).
[INFO] Discovered 1 device(s): 1 CPU, 0 GPU.
[INFO]   Device 0: CPU: Intel(R) Xeon(R) Processor | vendor: Intel | 2 logical processors | 2 physical cores | RAM 4.1 GiB (via linux-procfs)
[INFO] Vulkan backend ready: physical device 'llvmpipe (LLVM 19.1.7, 256 bits)' (software/CPU implementation, Vulkan API 1.4)
[INFO] Compute Runtime initialized. Available backends: cpu, vulkan
[INFO] CPU execution success: C = A + B (11 22 33 44 55 66 77 88)
[INFO] GPU (Vulkan) execution success on 'llvmpipe (LLVM 19.1.7, 256 bits)': C = A + B (11 22 33 44 55 66 77 88)
[INFO] Result verification: GPU output matches CPU reference.
```

On a machine without any Vulkan device, the program instead prints
`GPU (Vulkan) backend unavailable on this system: <reason>` and continues with CPU results.

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
│   ├── main.cpp                    # Discovery + compute runtime demo
│   └── core/
│       ├── version.hpp / logger.*  # Phase 1 utilities
│       ├── device/                 # Phase 2 Hardware Discovery (unchanged core)
│       └── compute/                # Phase 3 Compute Runtime
│           ├── task.hpp / .cpp     # VectorAddTask, Status, results, validation
│           ├── backend.hpp         # IComputeBackend interface (Runtime ↔ Backend boundary)
│           ├── cpu_backend.*       # CPU reference implementation
│           ├── vulkan_backend.*    # Vulkan compute backend (stub without Vulkan)
│           ├── runtime.*           # Lifecycle + explicit backend selection
│           └── vector_add_spv.hpp  # Embedded SPIR-V (generated, committed)
└── tests/
    ├── test_compute_cpu.cpp        # CPU path: must pass everywhere
    ├── test_compute_gpu.cpp        # GPU path: real tests when Vulkan available
    └── ... (Phase 1/2 tests unchanged)
```

## Testing

```bash
ctest --test-dir build -C Release --output-on-failure
```

| Test | What it verifies |
|------|------------------|
| VersionTest | Version constants match 0.3.0 |
| LoggerTest | Logger output format |
| DeviceDiscoveryTest | Phase 2 device discovery (unchanged, still passing) |
| ComputeCpuTest | Runtime lifecycle, CPU vector addition (sizes 4/16/1024/10007), invalid input handling, unknown/unavailable backends, shutdown/re-init |
| ComputeGpuTest | When a Vulkan device exists: real GPU vector addition (sizes 4/16/64/1024/5000 incl. non-workgroup multiples), bit-exact CPU-vs-GPU verification, repeated-run determinism, resource cleanup via re-init. Without a device: exits with an explicit SKIP note (never faked success) |

No test requires a specific GPU vendor or a GPU at all; machines with zero GPUs pass the full suite.

## Roadmap

| Version | Planned Feature | Status |
|---------|-----------------|--------|
| 0.2 | Hardware Discovery (CPU/GPU detection) | Implemented |
| 0.3 | Basic Compute Runtime (CPU backend + Vulkan GPU backend, Vector Addition) | **Implemented (current)** |
| 0.4 | Extended GPU compute (more task kinds, deeper Vulkan usage) | Planned |
| 0.5 | Virtual GPU Interface | Planned |
| 0.6 | Task Queue and Async Execution | Planned |
| 0.7 | Basic Scheduler | Planned |
| 1.0 | Local GPU Computing Platform | Planned |

## License

MIT License
