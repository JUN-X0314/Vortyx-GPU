cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(VortyxGPU
    VERSION 0.16.1
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
# fabric layer (its Phase 16 adapter converts TensorGraph nodes into fabric
# workload nodes) — and through it the distributed layer (it reads cluster
# snapshots and reuses the platform identity vocabulary) and the Phase 4
# resource system (the ONLY allocation path — no second memory world). OFF —
# or FABRIC/DISTRIBUTED/PLATFORM=OFF, which disable it automatically —
# reproduces the Phase 12 build (plus the fabric layer when it is ON).
option(VORTYX_ENABLE_TENSOR "Build the Vortyx tensor (AI/ML acceleration) layer" ON)

# Phase 14: the service layer (Production GPU Platform / Serviceization).
# Projects, authorization, quota, rate limiting, queue and the job service
# that wraps the existing platform -> distributed -> tensor stack. OFF — or
# TENSOR/PLATFORM=OFF, which disable it automatically — reproduces the
# Phase 13 build exactly.
option(VORTYX_ENABLE_SERVICE "Build the Vortyx service (platform serviceization) layer" ON)

# Phase 15: the native execution agent (the execution-plane boundary).
# The worker layer speaks the control plane's worker protocol and executes
# claimed jobs on the UNCHANGED Phase 12 distributed stack. OFF — or
# DISTRIBUTED/PLATFORM=OFF, which disable it automatically — reproduces the
# Phase 14 build exactly (the agent is an ADJACENT process boundary, not a
# core dependency of any layer below it).
option(VORTYX_ENABLE_WORKER "Build the Vortyx native worker (execution agent) layer" ON)

# Phase 16: the Adaptive Compute Fabric — the adaptive planning layer ON
# TOP of the unchanged distributed layer (workload descriptors, dependency
# graphs, the deterministic planner, plan lineage and the policy bridge
# into the existing orchestrator). OFF — or DISTRIBUTED/PLATFORM=OFF,
# which disable it automatically — reproduces the Phase 15 build exactly.
# The tensor layer builds ON the fabric (its Phase 16 adapter), so
# FABRIC=OFF disables the tensor layer with it.
option(VORTYX_ENABLE_FABRIC "Build the Vortyx adaptive compute fabric (planning) layer" ON)

# Verification-only option (Phase 15.0.1): build every target with
# AddressSanitizer + UndefinedBehaviorSanitizer so CTest failures surface
# memory errors and UB instead of silently continuing. GCC/Clang only —
# with MSVC the option is an honest configure-time error, not a silent no-op.
# UBSan is configured with -fno-sanitize-recover=all so a runtime UB report
# aborts the test (a failing sanitizer run must fail CI, not just log).
option(VORTYX_ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
if(VORTYX_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "VORTYX_ENABLE_SANITIZERS is GCC/Clang-only; MSVC sanitizers are not wired up (use the GitHub Actions ubuntu sanitizer job)")
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

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

# Fabric (Adaptive Compute Fabric) layer (Phase 16).
# A SEPARATE static library layered ON TOP of vortyx_distributed: the build
# graph enforces the direction (fabric -> distributed -> platform -> core).
# It is the adaptive PLANNING layer: workload descriptors, dependency
# graphs, the deterministic planner + cost model, plan lineage and the
# policy bridge that carries plans into the UNCHANGED orchestrator seam.
# It reads cluster snapshots, mutates nothing, and owns no execution. When
# the distributed layer is OFF — including the automatic PLATFORM=OFF case
# — this layer is OFF with it and the build is exactly Phase 15's.
if(VORTYX_ENABLE_FABRIC)
    if(NOT VORTYX_ENABLE_DISTRIBUTED)
        message(STATUS "Vortyx: the distributed layer is OFF - the fabric layer is disabled with it")
        set(VORTYX_ENABLE_FABRIC OFF)
    else()
        add_library(vortyx_fabric STATIC
            src/fabric/workload.cpp          # WorkloadDescriptor + derivation
            src/fabric/graph.cpp             # WorkloadGraph + topo validation
            src/fabric/cost.cpp              # deterministic heuristic scoring
            src/fabric/plan.cpp              # ComputePlan + stale rule + JSON
            src/fabric/planner.cpp           # the planning core (pure)
            src/fabric/replan.cpp            # feedback, lineage, safe replanning
            src/fabric/policy_bridge.cpp     # FabricPolicy -> orchestrator seam
            src/fabric/contract_fabric.cpp   # plan summary JSON (API-facing)
        )
        target_link_libraries(vortyx_fabric PUBLIC vortyx_distributed)
    endif()
endif()

# Tensor (AI/ML acceleration) layer (Phase 13).
# A SEPARATE static library layered ON TOP of vortyx_fabric (its Phase 16
# adapter converts TensorGraph nodes into fabric workload nodes) — and
# through it the distributed/platform/core chain the build graph enforces
# (tensor -> fabric -> distributed -> platform -> core; nothing below
# includes anything above). The tensor storage allocates EXCLUSIVELY
# through the Phase 4 resource system, and the placement integration reads
# Phase 12 snapshots read-only. No network code, no cloud SDK, no
# fabricated hardware capability — the reference kernels are honest CPU
# implementations and the runtime adapter routes into the REAL existing
# engine. When the distributed or fabric layer is OFF — including the
# automatic PLATFORM=OFF case — this layer is OFF with it.
if(VORTYX_ENABLE_TENSOR)
    if(NOT VORTYX_ENABLE_DISTRIBUTED)
        message(STATUS "Vortyx: the distributed layer is OFF - the tensor layer is disabled with it")
        set(VORTYX_ENABLE_TENSOR OFF)
    elseif(NOT VORTYX_ENABLE_FABRIC)
        message(STATUS "Vortyx: the fabric layer is OFF - the tensor layer is disabled with it (its Phase 16 adapter builds on the fabric)")
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
            src/tensor/fabric_adapter.cpp         # Phase 16 TensorGraph -> fabric workload adapter
            src/tensor/serialize.cpp              # metadata-only JSON (Phase 11 strict JSON)
        )
        target_link_libraries(vortyx_tensor PUBLIC vortyx_fabric)
    endif()
endif()

# Service library (Phase 14): the production-oriented control plane. It
# WRAPS the existing layers — projects/authz/quota/rate-limit/queue/audit/
# metrics/health over the Phase 11 store contracts, job dispatch through
# the UNCHANGED Phase 12 orchestrator. When the tensor layer is OFF —
# including the automatic DISTRIBUTED/PLATFORM=OFF cases — this layer is
# OFF with it and the build is exactly Phase 13's.
if(VORTYX_ENABLE_SERVICE)
    if(NOT VORTYX_ENABLE_TENSOR)
        message(STATUS "Vortyx: the tensor layer is OFF - the service layer is disabled with it")
        set(VORTYX_ENABLE_SERVICE OFF)
    else()
        add_library(vortyx_service STATIC
            src/service/service_status.cpp       # the service result vocabulary
            src/service/authz.cpp                # the pure role/action table
            src/service/project.cpp              # projects + memberships (provider-neutral)
            src/service/quota.cpp                # project quota ledger (exactly-once release)
            src/service/ratelimit.cpp            # deterministic fixed-window limiter
            src/service/queue.cpp                # provider-neutral job queue
            src/service/audit.cpp                # bounded audit events
            src/service/metrics.cpp              # real counters only
            src/service/health.cpp               # honest health reporting
            src/service/artifact.cpp             # artifact metadata registry
            src/service/platform_service.cpp     # the submission flow + dispatchers
            src/service/contract_service.cpp     # JSON views (strict platform JSON)
        )
        target_link_libraries(vortyx_service PUBLIC vortyx_tensor)
    endif()
endif()

# Worker layer (Phase 15): the native execution agent. It sits BESIDE the
# layers above (nothing includes it; it links the distributed layer and the
# platform JSON) and owns the worker-protocol boundary: claim/heartbeat/
# complete over HTTP, execution through the UNCHANGED Phase 12 orchestrator
# over the local simulated devices. When the distributed layer is OFF —
# including the automatic PLATFORM=OFF case — this layer is OFF with it and
# the build is exactly Phase 14's.
if(VORTYX_ENABLE_WORKER)
    if(NOT VORTYX_ENABLE_DISTRIBUTED)
        message(STATUS "Vortyx: the distributed layer is OFF - the worker layer is disabled with it")
        set(VORTYX_ENABLE_WORKER OFF)
    else()
        add_library(vortyx_worker STATIC
            src/worker/native_executor.hpp    # the execution seam (INativeExecutor)
            src/worker/native_executor.cpp    # Phase 12 stack execution + payload synthesis
            src/worker/worker_protocol.hpp    # the wire contract (claim/heartbeat/complete)
            src/worker/worker_protocol.cpp    # strict JSON codec (the platform JSON)
            src/worker/worker_transport.hpp   # the transport interface
            src/worker/http_transport.hpp     # the real HTTP/1.1 client
            src/worker/http_transport.cpp     # POSIX/Winsock, plain http (documented scope)
            src/worker/worker_agent.hpp       # the agent loop
            src/worker/worker_agent.cpp       # claim -> execute -> report
        )
        target_link_libraries(vortyx_worker PUBLIC vortyx_distributed)

        # The agent executable (Phase 15): claims jobs from the control
        # plane and executes them on the real local stack. See
        # src/worker_main.cpp for the configuration contract.
        add_executable(vortyx_worker_agent
            src/worker_main.cpp
        )
        target_link_libraries(vortyx_worker_agent PRIVATE vortyx_worker)
        target_compile_definitions(vortyx_worker_agent PRIVATE VORTYX_BUILD_CONFIG="$<CONFIG>")
    endif()
endif()

# GPU discovery on Windows uses DXGI (part of the Windows OS/SDK, no external
# dependency). Only linked on Windows targets.
if(WIN32)
    target_link_libraries(vortyx_core PRIVATE dxgi)
    # The worker's HTTP transport uses Winsock (part of the OS). Guarded by
    # TARGET: with the worker layer disabled (PLATFORM/DISTRIBUTED=OFF) the
    # target does not exist and must not be referenced.
    if(TARGET vortyx_worker)
        target_link_libraries(vortyx_worker PUBLIC ws2_32)
    endif()
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
