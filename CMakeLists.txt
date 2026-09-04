cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(VortyxGPU
    VERSION 0.1.0
    DESCRIPTION "Vortyx GPU - Open-source GPU computing platform"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VORTYX_BUILD_TESTS "Build Vortyx tests" ON)

# Compiler warnings (applied to all targets in this directory and below)
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Core library
# Foundation for future Vortyx modules (Runtime, Scheduler, Device, ...).
# New modules should be added to this library (or new libraries linked by the app).
add_library(vortyx_core STATIC
    src/core/logger.cpp
)
target_include_directories(vortyx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

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
