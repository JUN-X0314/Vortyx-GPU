# Vortyx GPU

[![CI](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml/badge.svg)](https://github.com/JUN-X0314/Vortyx-GPU/actions/workflows/ci.yml)

Vortyx GPU is an independent open-source project that researches and develops **GPU and GPU computing technology itself**, not AI services. AI is merely one of many application domains where Vortyx GPU can be utilized.

The long-term goal is to build a software-based GPU computing system, then evolve through Virtual GPU, multi-device computing, distributed computing, and FPGA prototypes, ultimately researching and developing Vortyx's own GPU hardware architecture.

## Current Phase: Phase 1 (v0.1.0)

Phase 1's goal is to establish a **stable development foundation**. GPU computing features are **not yet implemented**. Only the following infrastructure is in place:

- CMake-based build system (C++17, `vortyx_core` library + `vortyx` executable)
- Version information and basic logging utility
- Runnable minimal program (`vortyx`)
- Basic tests (version, logger) via CTest
- GitHub Actions CI (Windows + Ubuntu build and test)

No GPU detection, GPU compute, Runtime, Scheduler, or Virtual GPU functionality exists yet. The `vortyx` program prints the project name, version, and Phase 1 status only, and explicitly reports that GPU features are not implemented.

## Supported Environment

- **OS**: Windows 10/11 (primary), Linux (build verified via CI and local GCC build)
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

Expected output (Release build):

```
========================================
  Vortyx GPU
  Version: 0.1.0
  Phase:   1 (Development Foundation)
  Build:   Release
========================================
[INFO] Vortyx started.
[INFO] Runtime: not implemented yet (planned for a future phase).
[INFO] GPU compute: not implemented yet (no GPU API is used in Phase 1).
[INFO] Phase 1 status: development environment ready.
```

## Project Structure

```
Vortyx-GPU/
├── .github/
│   └── workflows/
│       └── ci.yml              # GitHub Actions CI (Windows + Ubuntu)
├── CMakeLists.txt              # Root build configuration
├── src/
│   ├── main.cpp                # Entry point of the vortyx executable
│   └── core/                   # Core library (vortyx_core)
│       ├── version.hpp         # Version constants
│       ├── logger.hpp          # Logging interface
│       └── logger.cpp          # Logging implementation
├── tests/
│   ├── CMakeLists.txt          # Test target registration (CTest)
│   ├── test_version.cpp        # Version constant checks
│   └── test_logger.cpp         # Logger output checks
├── README.md
├── LICENSE
└── .gitignore
```

`vortyx_core` is the foundation library where future Vortyx modules (Runtime, Device, Compute Task, Scheduler, Worker, GPU Backend, ...) will be added as they are actually implemented.

## Testing

```bash
ctest --test-dir build -C Release --output-on-failure
```

| Test | Target | What it verifies |
|------|--------|------------------|
| VersionTest | `test_version` | Version constants match 0.1.0 |
| LoggerTest | `test_logger` | `vortyx::log()` emits correct `[LEVEL] message` output |

## Roadmap (Planned, Not Yet Implemented)

| Version | Planned Feature |
|---------|-----------------|
| 0.2 | Hardware Discovery (CPU/GPU detection) |
| 0.3 | Basic Compute Runtime |
| 0.4 | GPU Compute Backend (Vulkan, etc.) |
| 0.5 | Virtual GPU Interface |
| 0.6 | Task Queue and Async Execution |
| 0.7 | Basic Scheduler |
| 1.0 | Local GPU Computing Platform |

## License

MIT License
