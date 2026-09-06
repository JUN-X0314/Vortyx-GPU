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
# distributed scheduler and virtual-device transport).
option(VORTYX_ENABLE_DISTRIBUTED "Build the Vortyx distributed / multi-device layer" ON)
