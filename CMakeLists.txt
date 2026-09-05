cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(VortyxGPU
    VERSION 0.11.0
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
