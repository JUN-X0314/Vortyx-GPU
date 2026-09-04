cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(VortyxGPU
    VERSION 0.4.0
    DESCRIPTION "Vortyx GPU - Open-source GPU computing platform"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VORTYX_BUILD_TESTS "Build Vortyx tests" ON)
option(VORTYX_ENABLE_VULKAN "Enable the Vulkan GPU compute backend (requires Vulkan headers + loader)" ON)

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
)
target_include_directories(vortyx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

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

# Tests
if(VORTYX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
