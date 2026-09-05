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

# Phase 6: Task Queue & Async Execution.
# - CPU queue tests must pass on every system (including GPU-less machines
#   and CPU-only builds): lifecycle, FIFO, async execution, state
#   transitions, failure propagation, shutdown/drain, thread safety.
# - GPU queue tests run for real when a Vulkan device is available; on
#   systems without one they exit with an explicit SKIP note (never a fake
#   success).
add_executable(test_taskqueue test_taskqueue.cpp)
target_link_libraries(test_taskqueue PRIVATE vortyx_core)
add_test(NAME TaskQueueTest COMMAND test_taskqueue)

add_executable(test_taskqueue_gpu test_taskqueue_gpu.cpp)
target_link_libraries(test_taskqueue_gpu PRIVATE vortyx_core)
add_test(NAME TaskQueueGpuTest COMMAND test_taskqueue_gpu)

# Phase 7: Basic Scheduler.
# - CPU scheduler tests must pass on every system (including GPU-less
#   machines and CPU-only builds): lifecycle, request validation, the pure
#   selection policy over synthetic candidates, adaptive real selections,
#   determinism, shutdown/re-init, TaskQueue integration, concurrency.
# - GPU scheduler tests run for real when a Vulkan device is available (the
#   automatic policy must pick 'vulkan' there); on systems without one they
#   exit with an explicit SKIP note (never a fake success).
add_executable(test_scheduler test_scheduler.cpp)
target_link_libraries(test_scheduler PRIVATE vortyx_core)
add_test(NAME SchedulerTest COMMAND test_scheduler)

add_executable(test_scheduler_gpu test_scheduler_gpu.cpp)
target_link_libraries(test_scheduler_gpu PRIVATE vortyx_core)
add_test(NAME SchedulerGpuTest COMMAND test_scheduler_gpu)

# Phase 8: Benchmark + Resource Monitoring.
# - Benchmark/monitor CPU tests must pass on every system (including
#   GPU-less machines and CPU-only builds): config/task validation,
#   real-path measurement invariants (structural only — never timing
#   values), correctness verdicts, honest backend handling, the pure
#   statistics algorithm, snapshot consistency with the Runtime's own
#   answers, value semantics, Scheduler independence, concurrent read-only
#   snapshots.
# - Benchmark/monitor GPU tests run for real when a Vulkan device is
#   available; on systems without one they exit with an explicit SKIP note
#   (never a fake success).
add_executable(test_benchmark test_benchmark.cpp)
target_link_libraries(test_benchmark PRIVATE vortyx_core)
add_test(NAME BenchmarkTest COMMAND test_benchmark)

add_executable(test_benchmark_gpu test_benchmark_gpu.cpp)
target_link_libraries(test_benchmark_gpu PRIVATE vortyx_core)
add_test(NAME BenchmarkGpuTest COMMAND test_benchmark_gpu)

add_executable(test_monitor test_monitor.cpp)
target_link_libraries(test_monitor PRIVATE vortyx_core)
add_test(NAME MonitorTest COMMAND test_monitor)

add_executable(test_monitor_gpu test_monitor_gpu.cpp)
target_link_libraries(test_monitor_gpu PRIVATE vortyx_core)
add_test(NAME MonitorGpuTest COMMAND test_monitor_gpu)

# Phase 10: Compute Engine (generic ComputeTask layer).
# - CPU tests must pass on every system (including GPU-less machines and
#   CPU-only builds): strict validation, every op vs the host reference,
#   modular int32 semantics, legacy/generic identity, parallel-CPU
#   determinism, batch semantics (per-item honesty, partial success,
#   wholesale refusals), VirtualGpu gating, TaskQueue integration.
# - GPU tests run for real when a Vulkan device is available; on systems
#   without one they exit with an explicit SKIP note (never a fake success).
add_executable(test_compute_tasks test_compute_tasks.cpp)
target_link_libraries(test_compute_tasks PRIVATE vortyx_core)
add_test(NAME ComputeTasksTest COMMAND test_compute_tasks)

add_executable(test_compute_tasks_gpu test_compute_tasks_gpu.cpp)
target_link_libraries(test_compute_tasks_gpu PRIVATE vortyx_core)
add_test(NAME ComputeTasksGpuTest COMMAND test_compute_tasks_gpu)

# Phase 11: Platform / Cloud Layer Foundation.
# - Platform tests run on EVERY system: identity/metadata/job models, the
#   auth boundary (owner vs foreign vs anonymous), the full
#   InMemoryPlatformStore contract (registration, ownership, idempotency,
#   lifecycle transitions, results, concurrency). No network, no cloud
#   account, no secrets — the local/mock store IS the reference
#   implementation of the control-plane rules.
# - Contract tests pin the wire contract the Vercel API layer mirrors:
#   strict JSON, the unified error schema, the HTTP status mapping, request
#   parsing and response serialization.
if(VORTYX_ENABLE_PLATFORM)
    add_executable(test_platform test_platform.cpp)
    target_link_libraries(test_platform PRIVATE vortyx_platform)
    add_test(NAME PlatformTest COMMAND test_platform)

    add_executable(test_platform_contract test_platform_contract.cpp)
    target_link_libraries(test_platform_contract PRIVATE vortyx_platform)
    add_test(NAME PlatformContractTest COMMAND test_platform_contract)
endif()
