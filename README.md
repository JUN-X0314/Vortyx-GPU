# Vortyx GPU

[![CI](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml/badge.svg)](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml)

Vortyx GPU is an independent open-source project that researches and develops **GPU and GPU computing technology itself**, not AI services. AI is merely one of many application domains where Vortyx GPU can be utilized.

The long-term goal is to build a software-based GPU computing system, then evolve through Virtual GPU, multi-device computing, distributed computing, and FPGA prototypes, ultimately researching and developing Vortyx's own GPU hardware architecture.

## Current Phase: Phase 2 (v0.2.0) — Hardware Discovery

Phase 2 implements **Hardware Discovery**: Vortyx can now detect the computing devices of the machine it runs on and represent them internally as Device objects.

**Implemented in Phase 2:**

- CPU discovery: name, vendor, logical processors, physical cores, system RAM
- GPU discovery: adapter name, vendor, dedicated video memory, shared memory
- Device abstraction (`vortyx::device::DeviceInfo`) usable by future Runtime/Scheduler layers
- Graceful handling of GPU-less systems and GPU API failures (logged, program keeps running)

**Not yet implemented** (do not confuse detection with compute):

- GPU Compute: running any task/kernel on a GPU is NOT implemented. Phase 2 only *discovers* GPU devices and their properties.
- Compute Runtime, Virtual GPU, Task Queue, Scheduler, Multi-GPU distribution, Network Workers, FPGA

## Detection Methods

| Target | Windows (primary) | Linux |
|--------|-------------------|-------|
| CPU | Win32 (`GetNativeSystemInfo`, `GetLogicalProcessorInformation`, `GlobalMemoryStatusEx`) + CPUID brand/vendor string | `/proc/cpuinfo`, `/proc/meminfo` (fallback: `std::thread::hardware_concurrency`) |
| GPU | DXGI adapter enumeration (`CreateDXGIFactory1`, part of Windows itself) | sysfs PCI scan (`/sys/bus/pci/devices`, display controller classes) |

- No external dependencies or SDKs are required. DXGI ships with Windows; sysfs is part of Linux.
- Vulkan is intentionally **not** used in Phase 2. It is planned together with the actual GPU compute backend (v0.4), where device enumeration via Vulkan will make sense.
- Information that a platform cannot reliably provide (e.g. GPU product name on Linux without a pci.ids database) is reported as `unknown` rather than guessed.

## Supported Environment

- **OS**: Windows 10/11 (primary), Linux (build + discovery verified via CI and local builds)
- **Build Tools**: CMake 3.16+, C++17-compatible compiler (MSVC, GCC, Clang)
- **Cost**: 0 KRW (free/open-source tools only, no external dependencies)

## Build and Run

```powershell
# Windows (Developer Command Prompt or PowerShell, Visual Studio Build Tools required)
cmake -B build -S .
cmake --build build --config Release
.\build\Release\vortyx.exe

# Run tests
ctest --test-dir build -C Release
```

```bash
# Linux / macOS
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/vortyx

# Run tests
ctest --test-dir build -C Release
```

Example output — **actual values depend on the machine Vortyx runs on** (example below was produced on a Linux system with one CPU and no GPU):

```
========================================
  Vortyx GPU
  Version: 0.2.0
  Phase:   2 (Hardware Discovery)
  Build:   Release
========================================
[INFO] Vortyx started.
[INFO] GPU discovery ran successfully, found 0 GPU device(s).
[INFO] Discovered 1 device(s): 1 CPU, 0 GPU.
[INFO]   Device 0: CPU: Intel(R) Xeon(R) Processor | vendor: Intel | 2 logical processors | 2 physical cores | RAM 4.1 GiB (via linux-procfs)
[INFO] Hardware discovery: implemented in Phase 2.
[INFO] GPU compute: not implemented yet (detection only; compute is a later phase).
[INFO] Phase 2 status: hardware discovery ready.
```

On a Windows machine with a GPU, each detected adapter is listed as, for example:
`GPU: <adapter name> | vendor: <NVIDIA/AMD/Intel/...> | VRAM <n> GiB | shared <n> GiB | id: luid-...`

## Project Structure

```
Vortyx-GPU/
├── .github/
│   └── workflows/
│       └── ci.yml                  # GitHub Actions CI (Windows + Ubuntu)
├── CMakeLists.txt                  # Root build configuration
├── src/
│   ├── main.cpp                    # Entry point: prints version + discovered devices
│   └── core/
│       ├── version.hpp             # Version constants
│       ├── logger.hpp / logger.cpp # Logging utility
│       └── device/                 # Hardware Discovery module (vortyx_core)
│           ├── device.hpp / .cpp   # DeviceType, DeviceInfo, human-readable describe()
│           ├── discovery.hpp / .cpp# Public API: discover_cpus/gpus/devices()
│           ├── platform_discovery.hpp  # Internal platform backend contract
│           ├── vendor_names.hpp    # PCI/CPUID vendor-id to name mapping
│           ├── platform_windows.cpp# Win32 CPU + DXGI GPU discovery
│           ├── platform_linux.cpp  # procfs CPU + sysfs PCI GPU discovery
│           └── platform_fallback.cpp # Generic fallback (CPU only)
├── tests/
│   ├── CMakeLists.txt              # Test target registration (CTest)
│   ├── test_version.cpp            # Version constant checks
│   ├── test_logger.cpp             # Logger output checks
│   └── test_device.cpp             # Device abstraction + discovery checks
├── README.md
├── LICENSE
└── .gitignore
```

`vortyx::device::discover_cpus() / discover_gpus() / discover_devices()` are designed to be consumed by the future Runtime and Scheduler layers (device lists, device kinds, memory and processor counts).

## Testing

```bash
ctest --test-dir build -C Release --output-on-failure
```

| Test | Target | What it verifies |
|------|--------|------------------|
| VersionTest | `test_version` | Version constants match 0.2.0 |
| LoggerTest | `test_logger` | `vortyx::log()` emits correct `[LEVEL] message` output |
| DeviceDiscoveryTest | `test_device` | DeviceInfo safety/copy semantics, honest `unknown` rendering, CPU discovery validity, GPU discovery handling of zero-GPU systems, collection consistency |

Hardware discovery tests are deliberately hardware-independent: they never require a specific number of GPUs and pass on machines with no GPU at all.

## Roadmap (Planned, Not Yet Implemented)

| Version | Planned Feature | Status |
|---------|-----------------|--------|
| 0.2 | Hardware Discovery (CPU/GPU detection) | **Implemented (current)** |
| 0.3 | Basic Compute Runtime | Planned |
| 0.4 | GPU Compute Backend (Vulkan, etc.) | Planned |
| 0.5 | Virtual GPU Interface | Planned |
| 0.6 | Task Queue and Async Execution | Planned |
| 0.7 | Basic Scheduler | Planned |
| 1.0 | Local GPU Computing Platform | Planned |

## License

MIT License
