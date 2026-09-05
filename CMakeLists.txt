cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(VortyxGPU
    VERSION 0.13.0
    DESCRIPTION "Vortyx GPU - Open-source GPU computing platform"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VORTYX_BUILD_TESTS "Build Vortyx tests" ON)
option(VORTYX_ENABLE_VULKAN "Enable the Vulkan GPU compute backend (requires Vulkan headers + loader)" ON)
# Phase 11: the platform (control-plane) layer. Pure standard-library C++ —
# no cloud SDK, no HTTP client, no JSON dependency. OFF builds the project
# exactly like every release before Phase 11 (the core never depends on it).
option(VORTYX_ENABLE_PLATFORM "Build the Vortyx platform (control-plane) layer" ON)
# Phase 12: the distributed (multi-device) layer. Builds ON TOP of the
# platform layer (identity, auth, job contract, the provider-neutral
# store) and adds the device registry, scheduling policies, sharding,
# workers and the local multi-device simulator. OFF — or PLATFORM=OFF,
# which disables it automatically — reproduces the Phase 11 build exactly.
option(VORTYX_ENABLE_DISTRIBUTED "Build the Vortyx distributed (multi-device) layer" ON)
# Phase 13: the tensor (AI/ML acceleration) layer. Builds ON TOP of the
# distributed layer (it reads cluster snapshots and reuses the platform
# identity vocabulary) and the Phase 4 resource system (the ONLY allocation
# path — no second memory world). OFF — or PLATFORM/DISTRIBUTED=OFF, which
# disable it automatically — reproduces the Phase 12 build exactly.
option(VORTYX_ENABLE_TENSOR "Build the Vortyx tensor (AI/ML acceleration) layer" ON)

# Compiler warnings (applied to all targets in this directory and below)
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- Optional Vulkan GPU compute backend -----------------------------------
# When Vulkan is found, VORTYX_HAS_VULKAN is defined and the GPU backend is
# compiled in. When not found (or the option is OFF), the project builds
# CPU-only: the Vulkan backend compiles as a stub that reports itself
# unavailable. Building and running Vortyx never requires the Vulkan SDK.
set(VORTYX_HAS_VULKAN_COMPILED OFF)
if(VORTYX_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(VORTYX_HAS_VULKAN_COMPILED ON)
        message(STATUS "Vortyx: Vulkan found - GPU compute backend enabled")
    else()
        message(STATUS "Vortyx: Vulkan not found - building CPU-only (no GPU backend)")
    endif()
else()
    message(STATUS "Vortyx: VORTYX_ENABLE_VULKAN is OFF - building CPU-only")
endif()

# Core library
# Foundation for future Vortyx modules (Runtime, Scheduler, Device, ...).
# New modules should be added to this library (or new libraries linked by the app).
add_library(vortyx_core STATIC
    src/core/logger.cpp

    # Hardware Discovery (Phase 2)
    src/core/device/device.cpp
    src/core/device/discovery.cpp
    src/core/device/platform_windows.cpp
    src/core/device/platform_linux.cpp
    src/core/device/platform_fallback.cpp

    # Compute Runtime (Phase 3)
    src/core/compute/task.cpp
    src/core/compute/cpu_backend.cpp
    src/core/compute/vulkan_backend.cpp
    src/core/compute/runtime.cpp

    # Compute Resource & Memory Management (Phase 4)
    src/core/resource/resource.cpp
    src/core/resource/buffer.cpp
    src/core/resource/resource_manager.cpp
    src/core/resource/cpu_buffer.cpp
    src/core/resource/vulkan_buffer.cpp

    # Virtual GPU Interface (Phase 5)
    src/core/vgpu/virtual_gpu.cpp

    # Task Queue & Async Execution (Phase 6)
    src/core/queue/task_queue.cpp

    # Basic Scheduler (Phase 7)
    src/core/scheduler/scheduler.cpp

    # Benchmark (Phase 8)
    src/core/benchmark/benchmark.cpp

    # Resource Monitoring (Phase 8)
    src/core/monitor/monitor.cpp
)
target_include_directories(vortyx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Platform (control-plane) layer (Phase 11).
# A SEPARATE static library on purpose: the layering is enforced by the build
# graph. vortyx_core never links or includes vortyx_platform; platform
# depends on the core only for the operation vocabulary (ComputeOp labels)
# and the version string. Contains no cloud SDK and no network code — the
# real backend sits behind the provider-neutral IPlatformStore interface.
if(VORTYX_ENABLE_PLATFORM)
    add_library(vortyx_platform STATIC
        src/platform/status.cpp          # control-plane result vocabulary
        src/platform/identity.cpp        # DeviceId/JobId/UserId + UUID v4 generation
        src/platform/metadata.cpp        # DeviceMetadata + contract vocabularies
        src/platform/job.cpp             # JobStatus/JobEnvelope/ResultEnvelope + transitions
        src/platform/auth.cpp            # AuthContext + ownership rules (the RLS mirror)
        src/platform/json.cpp            # minimal strict JSON (adapter boundary)
        src/platform/contract.cpp        # API contract codec (requests/responses/status codes)
        src/platform/memory_store.cpp    # InMemoryPlatformStore (local/mock)
    )
    target_link_libraries(vortyx_platform PUBLIC vortyx_core)
endif()

# Distributed (multi-device) layer (Phase 12).
# A SEPARATE static library, layered ON TOP of vortyx_platform: the build
# graph enforces the direction (distributed -> platform -> core; the core
# never sees any of it). Provider-neutral by construction: no network
# code, no cloud SDK — the transport is an interface with an in-process
# loopback implementation, and the platform integration speaks only
# IPlatformStore. When the platform layer is OFF, this layer is OFF with
# it and the build is exactly Phase 11's.
if(VORTYX_ENABLE_DISTRIBUTED)
    if(NOT VORTYX_ENABLE_PLATFORM)
        message(STATUS "Vortyx: VORTYX_ENABLE_PLATFORM is OFF - the distributed layer is disabled with it")
        set(VORTYX_ENABLE_DISTRIBUTED OFF)
    else()
        add_library(vortyx_distributed STATIC
            # foundations
            src/distributed/clock.cpp                 # injectable time (determinism)
            src/distributed/resource.cpp              # ResourceVector invariants
            src/distributed/device.cpp                # device state machine + capabilities
            src/distributed/lease.cpp                 # lease guard (RAII)
            src/distributed/registry.cpp              # IDeviceRegistry + LocalDeviceRegistry
            src/distributed/cluster.cpp               # immutable snapshots + topology view
            src/distributed/heartbeat.cpp             # liveness judgments (clock-injected)
            # job machinery
            src/distributed/shard.cpp                 # WorkPartition + shard state machine
            src/distributed/job.cpp                   # job status derivation/mapping
            src/distributed/retry.cpp                 # FailureCode + RetryPolicy
            src/distributed/aggregator.cpp            # duplicate-safe aggregation
            # scheduling
            src/distributed/policy.cpp                # RoundRobin / LeastLoaded / CapabilityFit
            # execution
            src/distributed/worker.cpp                # LocalWorker -> existing Runtime
            src/distributed/transport.cpp             # IWorkerTransport + loopback
            src/distributed/orchestrator.cpp          # the whole flow
            # environment
            src/distributed/config.cpp                # VORTYX_DISTRIBUTED_* parsing
            src/distributed/simulator.cpp             # local multi-device rig
            src/distributed/debug.cpp                 # deterministic diagnostic dumps
            src/distributed/contract_distributed.cpp  # distributed wire contract (C++ side)
        )
        target_link_libraries(vortyx_distributed PUBLIC vortyx_platform)

        # Diagnostic tool (Phase 12): builds a local multi-device cluster,
        # runs a distributed job end to end through the REAL path (registry
        # -> placement -> lease -> workers -> runtime -> aggregation), and
        # prints the cluster/job state. Developer visibility for the
        # distributed layer — deliberately NOT a CTest test.
        add_executable(vortyx_cluster
            src/cluster_main.cpp
        )
        target_link_libraries(vortyx_cluster PRIVATE vortyx_distributed)
        target_compile_definitions(vortyx_cluster PRIVATE VORTYX_BUILD_CONFIG="$<CONFIG>")
    endif()
endif()

# Tensor (AI/ML acceleration) layer (Phase 13).
# A SEPARATE static library layered ON TOP of vortyx_distributed: the build
# graph enforces the direction (tensor -> distributed -> platform -> core;
# nothing below includes anything above). The tensor storage allocates
# EXCLUSIVELY through the Phase 4 resource system, and the placement
# integration reads Phase 12 snapshots read-only. No network code, no cloud
# SDK, no fabricated hardware capability — the reference kernels are honest
# CPU implementations and the runtime adapter routes into the REAL existing
# engine. When the distributed layer is OFF — including the automatic
# PLATFORM=OFF case — this layer is OFF with it and the build is exactly
# Phase 12's.
if(VORTYX_ENABLE_TENSOR)
    if(NOT VORTYX_ENABLE_DISTRIBUTED)
        message(STATUS "Vortyx: the distributed layer is OFF - the tensor layer is disabled with it")
        set(VORTYX_ENABLE_TENSOR OFF)
    else()
        add_library(vortyx_tensor STATIC
            src/tensor/status.cpp                 # tensor result vocabulary + mappings
            src/tensor/dtype.cpp                  # DataType (fp32/fp16/bf16/int32/int8)
            src/tensor/shape.cpp                  # shape/stride/broadcast (checked arithmetic)
            src/tensor/placement.cpp              # device placement (Phase 11 identity reuse)
            src/tensor/storage.cpp                # TensorStorage (Phase 4 resource system only)
            src/tensor/tensor_value.cpp           # the Tensor value type + views
            src/tensor/op.cpp                     # op vocabulary + the one rule set
            src/tensor/capability.cpp             # capabilities / requirements
            src/tensor/backend.cpp                # reference kernels + runtime adapter + dispatch
            src/tensor/executor.cpp               # single-op execution facade
            src/tensor/graph.cpp                  # TensorGraph + validation + topo order
            src/tensor/plan.cpp                   # deterministic planner + memory planner
            src/tensor/graph_executor.cpp         # planned graph execution
            src/tensor/placement_integration.cpp  # Phase 12 capability-based placement
            src/tensor/serialize.cpp              # metadata-only JSON (Phase 11 strict JSON)
        )
        target_link_libraries(vortyx_tensor PUBLIC vortyx_distributed)
    endif()
endif()

# GPU discovery on Windows uses DXGI (part of the Windows OS/SDK, no external
# dependency). Only linked on Windows targets.
if(WIN32)
    target_link_libraries(vortyx_core PRIVATE dxgi)
endif()

if(VORTYX_HAS_VULKAN_COMPILED)
    target_compile_definitions(vortyx_core PUBLIC VORTYX_HAS_VULKAN)
    target_link_libraries(vortyx_core PUBLIC Vulkan::Vulkan)
endif()

# Main executable
add_executable(vortyx
    src/main.cpp
)
target_link_libraries(vortyx PRIVATE vortyx_core)
# Build configuration (Debug/Release) exposed to the program at compile time.
target_compile_definitions(vortyx PRIVATE VORTYX_BUILD_CONFIG="$<CONFIG>")

# Benchmark tool (Phase 8): a standalone, manually-run executable that
# measures the real compute path over a ladder of workload sizes (CPU, and
# Vulkan when a device is available). It is deliberately NOT registered as a
# CTest test: timing runs do not belong in the pass/fail CI suite (the
# Phase 8 test suite pins benchmark INVARIANTS instead, which are fast and
# deterministic). Build it with the rest of the project; run it when you
# want actual measurements.
add_executable(vortyx_bench
    src/benchmark_main.cpp
)
target_link_libraries(vortyx_bench PRIVATE vortyx_core)
target_compile_definitions(vortyx_bench PRIVATE VORTYX_BUILD_CONFIG="$<CONFIG>")

# Tests
if(VORTYX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
