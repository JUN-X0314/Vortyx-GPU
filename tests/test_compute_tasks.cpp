// Compute Engine tests (Phase 10) — CPU path.
//
// These tests MUST pass on every system, including machines without any GPU
// and CPU-only builds. They verify the generic compute task layer:
//   strict validation -> per-op execution vs host reference (add / multiply /
//   scale) -> modular int32 semantics -> legacy/generic identity ->
//   parallel-CPU determinism -> batch semantics (per-item honesty, partial
//   success, wholesale refusals) -> VirtualGpu gating -> lifecycle errors ->
//   no-leak accounting.
//
// Design rules honored here:
//   - No timing-based assertions: parallel execution is verified by
//     bit-exact correctness and determinism, never by speed.
//   - Integer results are bit-exact, so every comparison is exact.
//   - Failures are never faked: an unavailable backend must fail honestly.

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "core/compute/task.hpp"
#include "core/compute/cpu_backend.hpp"
#include "core/compute/runtime.hpp"
#include "core/queue/task_queue.hpp"
#include "core/vgpu/virtual_gpu.hpp"

using vortyx::compute::BatchResult;
using vortyx::compute::ComputeOp;
using vortyx::compute::ComputeTask;
using vortyx::compute::ComputeTaskResult;
using vortyx::compute::Runtime;
using vortyx::compute::Status;
using vortyx::compute::to_string;
using vortyx::compute::validate_compute_task;
using vortyx::compute::workload_label;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// The host reference for one op, computed independently of the engine
// (verification only, exactly like the benchmark's reference).
std::vector<std::int32_t> host_reference(const ComputeTask& task) {
    std::vector<std::int32_t> expected(task.a.size());
    switch (task.op) {
        case ComputeOp::VectorAdd:
            for (std::size_t i = 0; i < task.a.size(); ++i) expected[i] = task.a[i] + task.b[i];
            break;
        case ComputeOp::VectorMultiply:
            for (std::size_t i = 0; i < task.a.size(); ++i)
                expected[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(task.a[i]) *
                                                        static_cast<std::uint32_t>(task.b[i]));
            break;
        case ComputeOp::VectorScale:
            for (std::size_t i = 0; i < task.a.size(); ++i)
                expected[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(task.a[i]) *
                                                        static_cast<std::uint32_t>(task.scalar));
            break;
    }
    return expected;
}

bool matches(const ComputeTaskResult& result, const std::vector<std::int32_t>& expected) {
    return result.status == Status::Ok && result.data == expected;
}

ComputeTask make_add(std::size_t count, std::int32_t seed) {
    ComputeTask task;
    task.op = ComputeOp::VectorAdd;
    task.a.resize(count);
    task.b.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        task.a[i] = static_cast<std::int32_t>((i + static_cast<std::size_t>(seed)) % 1000) - 300;
        task.b[i] = static_cast<std::int32_t>(((i * 7) + static_cast<std::size_t>(seed)) % 500) + 11;
    }
    return task;
}

}  // namespace

int main() {
    // =====================================================================
    // 1. Strict task validation (unit level).
    // =====================================================================
    {
        std::string error;
        ComputeTask task = make_add(8, 0);
        check(validate_compute_task(task, error) == Status::Ok, "1a: valid add task accepted");

        task.b.resize(5);  // size mismatch (a has 8 elements)
        check(validate_compute_task(task, error) == Status::InvalidInput &&
                  error.find("mismatched input sizes") != std::string::npos,
              "1b: add size mismatch refused with reason");

        task.b.resize(task.a.size());
        task.scalar = 5;  // scalar is not an add operand
        check(validate_compute_task(task, error) == Status::InvalidInput &&
                  error.find("no scalar operand") != std::string::npos,
              "1c: add with non-zero scalar refused");

        ComputeTask empty;
        empty.op = ComputeOp::VectorMultiply;
        check(validate_compute_task(empty, error) == Status::InvalidInput &&
                  error.find("empty inputs") != std::string::npos,
              "1d: empty multiply refused");

        ComputeTask scale;
        scale.op = ComputeOp::VectorScale;
        scale.a = {1, 2, 3};
        scale.scalar = -4;
        check(validate_compute_task(scale, error) == Status::Ok, "1e: valid scale accepted");

        ComputeTask scale_with_b = scale;
        scale_with_b.b = {9, 9, 9};
        check(validate_compute_task(scale_with_b, error) == Status::InvalidInput &&
                  error.find("exactly one input") != std::string::npos,
              "1f: scale with a second input refused");
    }

    // =====================================================================
    // 2. Runtime execution of every op vs the host reference.
    // =====================================================================
    {
        Runtime runtime;
        check(runtime.initialize() == Status::Ok, "2: Runtime initializes");

        const std::size_t sizes[] = {1, 5, 1000};
        for (const std::size_t count : sizes) {
            ComputeTask add = make_add(count, 1);
            check(matches(runtime.execute(add), host_reference(add)),
                  "2a: VectorAdd matches the host reference (count=" +
                      std::to_string(count) + ")");

            ComputeTask mul;
            mul.op = ComputeOp::VectorMultiply;
            mul.a = add.a;
            mul.b = add.b;
            check(matches(runtime.execute(mul), host_reference(mul)),
                  "2b: VectorMultiply matches the host reference (count=" +
                      std::to_string(count) + ")");

            for (const std::int32_t scalar : {3, -7, 0}) {
                ComputeTask scale;
                scale.op = ComputeOp::VectorScale;
                scale.a = add.a;
                scale.scalar = scalar;
                check(matches(runtime.execute(scale), host_reference(scale)),
                      "2c: VectorScale matches the host reference (count=" +
                          std::to_string(count) + ", scalar=" + std::to_string(scalar) + ")");
            }
        }

        // Repeated execution of the same task is deterministic.
        ComputeTask add = make_add(64, 2);
        const ComputeTaskResult first = runtime.execute(add);
        const ComputeTaskResult second = runtime.execute(add);
        check(first.status == Status::Ok && first.data == second.data,
              "2d: repeated execution is deterministic");

        // Modular int32 semantics (two's complement), pinned on the CPU
        // baseline; the GPU test proves the same values bit-exact on Vulkan.
        {
            ComputeTask mul;
            mul.op = ComputeOp::VectorMultiply;
            mul.a = {std::numeric_limits<std::int32_t>::max(), -1,
                     std::numeric_limits<std::int32_t>::min() / 2};
            mul.b = {2, std::numeric_limits<std::int32_t>::min(), 2};
            const ComputeTaskResult r = runtime.execute(mul);
            check(r.status == Status::Ok && r.data.size() == 3 &&
                      r.data[0] == -2 &&  // INT32_MAX*2 wraps to -2
                  r.data[1] == std::numeric_limits<std::int32_t>::min() &&  // -1*INT32_MIN wraps
                  r.data[2] == std::numeric_limits<std::int32_t>::min(),  // MIN/2*2 == MIN
              "2e: int32 modular multiply semantics (overflow wraps, never UB)");
        }

        // The legacy Phase 3 API and the generic engine are the same path:
        // identical results, bit for bit.
        {
            vortyx::compute::VectorAddTask legacy;
            legacy.a = add.a;
            legacy.b = add.b;
            const vortyx::compute::VectorAddResult via_legacy = runtime.execute(legacy);
            const ComputeTaskResult via_generic = runtime.execute(add);
            check(via_legacy.status == Status::Ok && via_generic.status == Status::Ok &&
                      via_legacy.data == via_generic.data,
                  "2f: legacy VectorAddTask and generic ComputeTask are identical");
        }

        // No resource leaks after all executions.
        check(runtime.resources().stats().live_buffers == 0,
              "2g: engine executions leave no live buffers behind");

        runtime.shutdown();
    }

    // =====================================================================
    // 3. Parallel CPU execution: correctness and determinism on large
    //    workloads (above kParallelThreshold, so the fork-join path runs).
    // =====================================================================
    {
        Runtime runtime;
        check(runtime.initialize() == Status::Ok, "3: Runtime initializes");

        const std::size_t big = 300000;  // > CpuBackend::kParallelThreshold
        for (const ComputeOp op : {ComputeOp::VectorAdd, ComputeOp::VectorMultiply,
                                   ComputeOp::VectorScale}) {
            ComputeTask task = make_add(big, 3);
            task.op = op;
            if (op == ComputeOp::VectorScale) {
                task.b.clear();
                task.scalar = -13;
            }
            const ComputeTaskResult r1 = runtime.execute(task);
            const ComputeTaskResult r2 = runtime.execute(task);
            check(matches(r1, host_reference(task)),
                  "3a: large-workload result matches the host reference (op=" +
                      std::string(to_string(op)) + ")");
            check(r1.status == Status::Ok && r1.data == r2.data,
                  "3b: large-workload repeated execution is deterministic (op=" +
                      std::string(to_string(op)) + ")");
        }

        // Boundary size just below the default threshold (65536) stays
        // correct too — this is the sequential path in the default build.
        ComputeTask task = make_add(65535, 4);
        check(matches(runtime.execute(task), host_reference(task)),
              "3c: sub-threshold workload is correct");
        check(runtime.resources().stats().live_buffers == 0,
              "3d: no live buffers after parallel-path executions");

        runtime.shutdown();
    }

    // =====================================================================
    // 4. Runtime-level error policies for the generic path.
    // =====================================================================
    {
        Runtime runtime;
        check(runtime.initialize() == Status::Ok, "4: Runtime initializes");

        ComputeTask bad = make_add(8, 5);
        bad.b.resize(3);  // mismatch
        const ComputeTaskResult refused = runtime.execute(bad);
        check(refused.status == Status::InvalidInput && refused.data.empty() &&
                  !refused.error.empty(),
              "4a: invalid task is refused before any execution");

        const ComputeTaskResult unknown = runtime.execute(make_add(8, 6), "cuda");
        check(unknown.status == Status::BackendUnavailable &&
                  unknown.error.find("cuda") != std::string::npos,
              "4b: unknown backend refused with the real reason");

        runtime.shutdown();
        const ComputeTaskResult after = runtime.execute(make_add(8, 7));
        check(after.status == Status::NotInitialized,
              "4c: execute after shutdown fails with NotInitialized");
    }

    // =====================================================================
    // 5. Batch semantics: per-item honesty, partial success, order.
    // =====================================================================
    {
        Runtime runtime;
        check(runtime.initialize() == Status::Ok, "5: Runtime initializes");

        // 5a. Mixed-ops batch: every item Ok, results in submission order.
        {
            std::vector<ComputeTask> tasks;
            tasks.push_back(make_add(16, 10));
            ComputeTask scale;
            scale.op = ComputeOp::VectorScale;
            scale.a = tasks[0].a;
            scale.scalar = 2;
            tasks.push_back(scale);
            ComputeTask mul;
            mul.op = ComputeOp::VectorMultiply;
            mul.a = tasks[0].a;
            mul.b = tasks[0].b;
            tasks.push_back(mul);

            const BatchResult batch = runtime.execute_batch(tasks, "cpu");
            check(batch.status == Status::Ok && batch.error.empty(),
                  "5a: all-Ok batch succeeds");
            check(batch.succeeded == 3 && batch.failed == 0 && batch.results.size() == 3,
                  "5a: counts and per-item results are exact");
            for (std::size_t i = 0; i < tasks.size(); ++i) {
                check(matches(batch.results[i], host_reference(tasks[i])),
                      "5a: item " + std::to_string(i) + " matches its own task's reference");
            }
        }

        // 5b. Partial success: an invalid task fails alone; the others run.
        {
            std::vector<ComputeTask> tasks;
            tasks.push_back(make_add(8, 11));      // 0: valid
            tasks.push_back(make_add(8, 12));      // 1: invalid (b resized)
            tasks[1].b.resize(2);                  // size mismatch
            tasks.push_back(make_add(8, 13));      // 2: valid

            const BatchResult batch = runtime.execute_batch(tasks, "cpu");
            check(batch.status == Status::InvalidInput,
                  "5b: batch status is the FIRST failing item's own status");
            check(!batch.error.empty() &&
                      batch.error.find("first failure at index 1") != std::string::npos,
                  "5b: aggregate error names the first failure index");
            check(batch.succeeded == 2 && batch.failed == 1 && batch.results.size() == 3,
                  "5b: counts are exact");
            check(batch.results[0].status == Status::Ok &&
                      batch.results[2].status == Status::Ok,
                  "5b: valid tasks executed despite the earlier failure");
            check(batch.results[1].status == Status::InvalidInput &&
                      batch.results[1].data.empty(),
                  "5b: the invalid task failed as its own item without executing");
            check(matches(batch.results[0], host_reference(tasks[0])) &&
                      matches(batch.results[2], host_reference(tasks[2])),
                  "5b: successful results were never discarded");
        }

        // 5c. Wholesale refusals (no item runs).
        {
            const BatchResult empty = runtime.execute_batch({}, "cpu");
            check(empty.status == Status::InvalidInput && empty.results.empty(),
                  "5c: empty batch refused wholesale");

            const BatchResult unknown = runtime.execute_batch({make_add(4, 14)}, "cuda");
            check(unknown.status == Status::BackendUnavailable && unknown.results.empty() &&
                      unknown.error.find("cuda") != std::string::npos,
                  "5c: unknown backend refuses the whole batch");
        }

        runtime.shutdown();
        const BatchResult after = runtime.execute_batch({make_add(4, 15)}, "cpu");
        check(after.status == Status::NotInitialized && after.results.empty(),
              "5d: batch after shutdown fails with NotInitialized");
    }

    // =====================================================================
    // 6. VirtualGpu level: generic execute + batch + lifecycle gating.
    // =====================================================================
    {
        vortyx::vgpu::VirtualGpu gpu;  // explicit cpu
        ComputeTask before_init = make_add(4, 20);
        check(gpu.execute(before_init).status == Status::NotInitialized,
              "6a: execute(ComputeTask) before initialize() fails cleanly");

        check(gpu.initialize() == Status::Ok, "6b: cpu VirtualGpu initializes");
        const ComputeTaskResult run = gpu.execute(before_init);
        check(matches(run, host_reference(before_init)),
              "6c: VirtualGpu generic execution matches the reference");

        std::vector<ComputeTask> tasks = {make_add(8, 21), make_add(8, 22)};
        const BatchResult batch = gpu.execute_batch(tasks);
        check(batch.status == Status::Ok && batch.succeeded == 2 &&
                  matches(batch.results[1], host_reference(tasks[1])),
              "6d: VirtualGpu batch executes on the configured backend");

        gpu.shutdown();
        check(gpu.execute(before_init).status == Status::NotInitialized,
              "6e: generic execute after shutdown fails with NotInitialized");
        check(gpu.execute_batch(tasks).status == Status::NotInitialized,
              "6f: batch after shutdown fails with NotInitialized");
    }

    // =====================================================================
    // 7. Known-but-unavailable backend through the generic path: honest,
    //    adaptive, no silent fallback (same policy as the legacy path).
    // =====================================================================
    {
        vortyx::vgpu::VirtualGpu gpu;
        vortyx::vgpu::VirtualGpuDesc desc;
        desc.backend = "vulkan";
        check(gpu.initialize(desc) == Status::Ok,
              "7: vulkan VirtualGpu initializes (known backend)");

        ComputeTask task = make_add(64, 30);
        task.op = ComputeOp::VectorMultiply;
        const ComputeTaskResult r = gpu.execute(task);
        if (gpu.backend_available()) {
            check(matches(r, host_reference(task)),
                  "7a: with a real device the generic op really executes on vulkan");
        } else {
            check(r.status == Status::BackendUnavailable && !r.error.empty(),
                  "7b: without a device the generic op fails honestly (never CPU fallback)");
            std::cout << "Note: vulkan backend unavailable in this environment - "
                      << gpu.backend_unavailable_reason() << "\n";
        }
        gpu.shutdown();
    }

    // =====================================================================
    // 8. TaskQueue integration: the generic queued work item runs FIFO on
    //    the queue's single worker and records honest results.
    // =====================================================================
    {
        vortyx::vgpu::VirtualGpu gpu;
        check(gpu.initialize() == Status::Ok, "8: VirtualGpu initializes");
        vortyx::queue::TaskQueue queue;
        check(queue.initialize(gpu) == Status::Ok, "8: TaskQueue initializes");

        ComputeTask scale;
        scale.op = ComputeOp::VectorScale;
        scale.a = {1, 2, 3, 4};
        scale.scalar = -3;
        ComputeTask mul;
        mul.op = ComputeOp::VectorMultiply;
        mul.a = {2, 4, 6, 8};
        mul.b = {10, 20, 30, 40};

        const vortyx::queue::EnqueueResult r1 = queue.enqueue(
            std::make_unique<vortyx::queue::ComputeTaskQueuedTask>(scale));
        const vortyx::queue::EnqueueResult r2 = queue.enqueue(
            std::make_unique<vortyx::queue::ComputeTaskQueuedTask>(mul));
        check(r1.status == Status::Ok && r2.status == Status::Ok && r1.id != r2.id,
              "8a: generic queued tasks are accepted with unique ids");

        check(queue.wait(r1.id) == vortyx::queue::TaskState::Completed &&
                  queue.wait(r2.id) == vortyx::queue::TaskState::Completed,
              "8b: both generic tasks complete FIFO");
        const vortyx::queue::TaskSnapshot s1 = queue.task_snapshot(r1.id);
        const vortyx::queue::TaskSnapshot s2 = queue.task_snapshot(r2.id);
        check(s1.result.status == Status::Ok &&
                  s1.result.data == std::vector<std::int32_t>{-3, -6, -9, -12},
              "8c: scale result is exact through the queue");
        check(s2.result.status == Status::Ok &&
                  s2.result.data == std::vector<std::int32_t>{20, 80, 180, 320},
              "8d: multiply result is exact through the queue");

        // An invalid generic task fails honestly at execution time through
        // the generic enqueue path (content validation happens there).
        ComputeTask invalid;
        invalid.op = ComputeOp::VectorScale;
        invalid.a = {1};
        invalid.b = {1, 2};  // scale must not carry a second input
        const vortyx::queue::EnqueueResult r3 = queue.enqueue(
            std::make_unique<vortyx::queue::ComputeTaskQueuedTask>(invalid));
        check(r3.status == Status::Ok, "8e: generic enqueue accepts (validation at execution)");
        check(queue.wait(r3.id) == vortyx::queue::TaskState::Failed &&
                  queue.task_snapshot(r3.id).result.status == Status::InvalidInput,
              "8f: an invalid generic task fails with InvalidInput (honest)");

        queue.shutdown();
        gpu.shutdown();
    }

    if (failures == 0) {
        std::cout << "Compute Engine CPU-path tests passed.\n";
        return 0;
    }
    std::cerr << failures << " compute engine CPU-path check(s) FAILED.\n";
    return 1;
}
