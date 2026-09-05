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

# Phase 12: Distributed / Multi-GPU Device System.
# - Distributed tests run on EVERY system (no GPU required): device state
#   machine, resource invariants, registry (idempotent registration,
#   leases, expiry, revision), scheduling policies, shard partition
#   invariants, job/retry/aggregation semantics, worker slicing (bit-exact
#   against the full-range reference), the local multi-device simulator,
#   the orchestrator end-to-end scenarios (single-device, multi-device,
#   failure/retry, partial failure, cancellation, platform integration)
#   and the distributed wire contract. All time-dependent behavior is
#   tested with an injected FakeClock — no sleeps, no flaky timing.
if(VORTYX_ENABLE_DISTRIBUTED)
    add_executable(test_distributed test_distributed.cpp)
    target_link_libraries(test_distributed PRIVATE vortyx_distributed)
    add_test(NAME DistributedTest COMMAND test_distributed)

    add_executable(test_distributed_scheduler test_distributed_scheduler.cpp)
    target_link_libraries(test_distributed_scheduler PRIVATE vortyx_distributed)
    add_test(NAME DistributedSchedulerTest COMMAND test_distributed_scheduler)

    add_executable(test_distributed_jobs test_distributed_jobs.cpp)
    target_link_libraries(test_distributed_jobs PRIVATE vortyx_distributed)
    add_test(NAME DistributedJobsTest COMMAND test_distributed_jobs)

    add_executable(test_distributed_worker test_distributed_worker.cpp)
    target_link_libraries(test_distributed_worker PRIVATE vortyx_distributed)
    add_test(NAME DistributedWorkerTest COMMAND test_distributed_worker)

    add_executable(test_distributed_orchestrator test_distributed_orchestrator.cpp)
    target_link_libraries(test_distributed_orchestrator PRIVATE vortyx_distributed)
    add_test(NAME DistributedOrchestratorTest COMMAND test_distributed_orchestrator)

    add_executable(test_distributed_contract test_distributed_contract.cpp)
    target_link_libraries(test_distributed_contract PRIVATE vortyx_distributed)
    add_test(NAME DistributedContractTest COMMAND test_distributed_contract)
endif()

# Phase 13: AI/ML Acceleration + Tensor Layer.
# - Tensor tests run on EVERY system (no GPU required — the reference
#   kernels are host implementations): dtype/shape/layout/broadcast/storage
#   (through the Phase 4 resource system), the full op surface against
#   hand-verified values and FP32 baselines (fp16/bf16 via the documented
#   promote-compute-round semantics), graph construction/validation/
#   deterministic planning/memory-planner reuse safety/end-to-end graph
#   execution, capability-based dispatch (including the runtime adapter
#   routing int32 elementwise into the REAL engine), and the Phase 12
#   integration (capability-based placement over a real registry,
#   offline-mid-flight failover, retry determinism, transfer honesty).
# - All limits and errors are explicit: unsupported ops/dtypes/devices are
#   precise refusals, never skipped or guessed.
if(VORTYX_ENABLE_TENSOR)
    add_executable(test_tensor_core test_tensor_core.cpp)
    target_link_libraries(test_tensor_core PRIVATE vortyx_tensor)
    add_test(NAME TensorCoreTest COMMAND test_tensor_core)

    add_executable(test_tensor_ops test_tensor_ops.cpp)
    target_link_libraries(test_tensor_ops PRIVATE vortyx_tensor)
    add_test(NAME TensorOpsTest COMMAND test_tensor_ops)

    add_executable(test_tensor_graph test_tensor_graph.cpp)
    target_link_libraries(test_tensor_graph PRIVATE vortyx_tensor)
    add_test(NAME TensorGraphTest COMMAND test_tensor_graph)

    add_executable(test_tensor_dispatch test_tensor_dispatch.cpp)
    target_link_libraries(test_tensor_dispatch PRIVATE vortyx_tensor)
    add_test(NAME TensorDispatchTest COMMAND test_tensor_dispatch)

    add_executable(test_tensor_distributed test_tensor_distributed.cpp)
    target_link_libraries(test_tensor_distributed PRIVATE vortyx_tensor)
    add_test(NAME TensorDistributedTest COMMAND test_tensor_distributed)
endif()
