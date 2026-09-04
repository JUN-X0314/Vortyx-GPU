add_executable(test_version test_version.cpp)
target_link_libraries(test_version PRIVATE vortyx_core)
add_test(NAME VersionTest COMMAND test_version)

add_executable(test_logger test_logger.cpp)
target_link_libraries(test_logger PRIVATE vortyx_core)
add_test(NAME LoggerTest COMMAND test_logger)

add_executable(test_device test_device.cpp)
target_link_libraries(test_device PRIVATE vortyx_core)
add_test(NAME DeviceDiscoveryTest COMMAND test_device)

# Phase 3: CPU and GPU compute paths are tested separately.
# - CPU tests must pass on every system (including GPU-less machines and
#   CPU-only builds).
# - GPU tests run for real when a Vulkan device is available; on systems
#   without one they exit with an explicit SKIP note (never a fake success).
add_executable(test_compute_cpu test_compute_cpu.cpp)
target_link_libraries(test_compute_cpu PRIVATE vortyx_core)
add_test(NAME ComputeCpuTest COMMAND test_compute_cpu)

add_executable(test_compute_gpu test_compute_gpu.cpp)
target_link_libraries(test_compute_gpu PRIVATE vortyx_core)
add_test(NAME ComputeGpuTest COMMAND test_compute_gpu)
