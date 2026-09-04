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

# Phase 4: Compute Resource & Memory Management.
# - Resource tests cover the full Buffer lifecycle (create/write/read/release),
#   error policies, move semantics, stats accounting and shutdown safety on
#   the CPU (host memory) path; they must pass everywhere.
# - GPU resource tests allocate REAL Vulkan buffers when a device exists;
#   on systems without one they exit with an explicit SKIP note (never a
#   fake success).
add_executable(test_resource test_resource.cpp)
target_link_libraries(test_resource PRIVATE vortyx_core)
add_test(NAME ResourceTest COMMAND test_resource)

add_executable(test_resource_gpu test_resource_gpu.cpp)
target_link_libraries(test_resource_gpu PRIVATE vortyx_core)
add_test(NAME ResourceGpuTest COMMAND test_resource_gpu)

# Phase 5: Virtual GPU Interface.
# - CPU Virtual GPU tests must pass on every system (including GPU-less
#   machines and CPU-only builds): lifecycle, state errors, move semantics,
#   honest backend-unavailability reporting.
# - Vulkan Virtual GPU tests run for real when a Vulkan device is available;
#   on systems without one they exit with an explicit SKIP note (never a
#   fake success).
add_executable(test_vgpu test_vgpu.cpp)
target_link_libraries(test_vgpu PRIVATE vortyx_core)
add_test(NAME VirtualGpuTest COMMAND test_vgpu)

add_executable(test_vgpu_gpu test_vgpu_gpu.cpp)
target_link_libraries(test_vgpu_gpu PRIVATE vortyx_core)
add_test(NAME VirtualGpuGpuTest COMMAND test_vgpu_gpu)
