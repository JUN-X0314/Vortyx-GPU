cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(VortyxGPU
    VERSION 0.1.0
    DESCRIPTION "Vortyx GPU - Open-source GPU computing platform"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source files
set(VORTYX_SOURCES
    src/main.cpp
    src/core/logger.cpp
)

# Main executable
add_executable(vortyx ${VORTYX_SOURCES})

target_include_directories(vortyx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Tests
enable_testing()
add_subdirectory(tests)