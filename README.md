# Vortyx GPU

Vortyx GPU is an independent open-source project that researches and develops **GPU and GPU computing technology itself**, not AI services. AI is merely one of many application domains where Vortyx GPU can be utilized.

The long-term goal is to build a software-based GPU computing system, then evolve through Virtual GPU, multi-device computing, distributed computing, and FPGA prototypes, ultimately researching and developing Vortyx's own GPU hardware architecture.

## Current Phase: Phase 1 (v0.1.0)

Phase 1's goal is to establish a **stable development foundation**. GPU computing features are **not yet implemented**. Only the following infrastructure is in place:

- CMake-based build system
- Version information and basic logging utility
- Runnable minimal program (`vortyx`)
- Basic test structure

## Supported Environment

- **OS**: Windows 10/11 (primary), Linux (future)
- **Build Tools**: CMake 3.16+, C++17 compatible compiler
- **Cost**: 0 KRW (free/open-source tools only)

## Build and Run

```bash
# Windows (Developer Command Prompt or PowerShell)
cmake -B build -S .
cmake --build build --config Release
.\build\Release\vortyx.exe

# Run tests
ctest --test-dir build -C Release
```

```bash
# Linux / macOS
cmake -B build -S .
cmake --build build
./build/vortyx

# Run tests
ctest --test-dir build
```

## Project Structure

```
Vortyx-GPU/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   └── core/
│       ├── version.hpp
│       ├── logger.hpp
│       └── logger.cpp
├── tests/
│   ├── CMakeLists.txt
│   └── test_version.cpp
├── README.md
└── LICENSE
```

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
