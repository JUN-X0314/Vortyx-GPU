// Task Queue GPU-path tests (Phase 6).
//
// Design rules (per project requirements, mirroring the Phase 3/4/5 GPU
// test policy):
//  - No hardcoded hardware expectations (no vendor names, no device count).
//    Tests pass on AMD, Intel, NVIDIA, software Vulkan implementations
//    (lavapipe/llvmpipe) and on machines with NO GPU at all.
//  - When the Vulkan backend is unavailable, the test exits successfully
//    with an explicit, visible note saying it did NOT run. GPU Queue
//    execution is never reported as a success that did not happen.
//  - When the backend IS available, several vector additions are queued on
//    a Vulkan Virtual GPU and must match the independently executed CPU
//    reference bit-exactly, in FIFO order.
//  - Shutdown order contract: the queue shuts down BEFORE its Virtual GPU.

#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/compute/task.hpp"
#include "core/queue/task_queue.hpp"
#include "core/vgpu/virtual_gpu.hpp"

using vortyx::compute::Status;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;
using vortyx::queue::EnqueueResult;
using vortyx::queue::kInvalidTaskId;
using vortyx::queue::QueuedTask;
using vortyx::queue::QueueState;
using vortyx::queue::TaskId;
using vortyx::queue::TaskQueue;
using vortyx::queue::TaskSnapshot;
using vortyx::queue::TaskState;
using vortyx::vgpu::VirtualGpu;
using vortyx::vgpu::VirtualGpuDesc;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

VectorAddTask make_task(std::size_t count) {
    VectorAddTask task;
    task.a.resize(count);
    task.b.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        // Deterministic values well inside int32 range (no overflow).
        task.a[i] = static_cast<std::int32_t>(i % 1000) - 300;
        task.b[i] = static_cast<std::int32_t>((i * 7) % 500) + 11;
    }
    return task;
}

bool result_matches(const VectorAddResult& result, const VectorAddTask& task) {
    if (result.status != Status::Ok) return false;
    if (result.data.size() != task.a.size()) return false;
    for (std::size_t i = 0; i < task.a.size(); ++i) {
        if (result.data[i] != task.a[i] + task.b[i]) return false;
    }
    return true;
}

// Minimal order-recording work item (same mechanism as the CPU test):
// with a single worker thread, the recorded execution order must equal the
// enqueue order if the queue is FIFO — on any backend.
class OrderRecordingTask final : public QueuedTask {
public:
    OrderRecordingTask(int key, std::mutex& order_mutex, std::vector<int>& order)
        : key_(key), order_mutex_(order_mutex), order_(order) {}

    VectorAddResult execute(VirtualGpu& /*gpu*/) override {
        {
            std::lock_guard<std::mutex> lock(order_mutex_);
            order_.push_back(key_);
        }
        return VectorAddResult{Status::Ok, std::string{}, {}};
    }

private:
    int key_;
    std::mutex& order_mutex_;
    std::vector<int>& order_;
};

}  // namespace

int main() {
    // The reference CPU Virtual GPU exists in every environment (created
    // first so the GPU queue results below are always compared against a
    // real, independently executed CPU result).
    VirtualGpu cpu_gpu;
    VirtualGpuDesc cpu_desc;
    cpu_desc.backend = "cpu";
    if (cpu_gpu.initialize(cpu_desc) != Status::Ok) {
        std::cerr << "FAIL: cpu VirtualGpu initialize() failed\n";
        return 1;
    }

    // The Vulkan Virtual GPU under test.
    VirtualGpu vulkan_gpu;
    VirtualGpuDesc vulkan_desc;
    vulkan_desc.backend = "vulkan";
    if (vulkan_gpu.initialize(vulkan_desc) != Status::Ok) {
        std::cerr << "FAIL: vulkan VirtualGpu initialize() must not fail for a known backend\n";
        return 1;
    }

    if (!vulkan_gpu.backend_available()) {
        // Environment without a usable Vulkan device: a normal, non-fatal
        // condition. Report it clearly; do NOT fake success.
        std::cout << "SKIPPED (environment): Vulkan backend unavailable - "
                  << vulkan_gpu.backend_unavailable_reason() << "\n";
        std::cout << "Note: GPU Queue execution was NOT tested on this machine. "
                  << "The Task Queue CPU path is covered by TaskQueueTest.\n";
        vulkan_gpu.shutdown();
        cpu_gpu.shutdown();
        return 0;
    }

    // --- Backend is available: run REAL Task Queue GPU tests -----------------
    const vortyx::device::DeviceInfo device = vulkan_gpu.device_info();
    std::cout << "Vulkan Virtual GPU available on: "
              << (device.name.empty() ? std::string("unknown device") : device.name)
              << " | vendor: " << (device.vendor.empty() ? "unknown" : device.vendor) << "\n";

    TaskQueue queue;
    check(queue.initialize(vulkan_gpu) == Status::Ok,
          "the Task Queue must initialize on the vulkan Virtual GPU");
    check(queue.state() == QueueState::Ready, "the GPU queue must be Ready");

    // --- 1. Several VectorAddTasks queued and executed on the real GPU -------
    {
        const std::vector<std::size_t> sizes = {4, 16, 64, 1024, 5000};
        std::vector<TaskId> ids;
        std::vector<VectorAddTask> tasks;
        std::vector<VectorAddResult> cpu_refs;

        for (const std::size_t size : sizes) {
            tasks.push_back(make_task(size));
            // Independent CPU reference, computed BEFORE the GPU queue runs.
            cpu_refs.push_back(cpu_gpu.execute(tasks.back()));
            check(cpu_refs.back().status == Status::Ok,
                  "the cpu reference must succeed for size " + std::to_string(size));
        }

        for (std::size_t i = 0; i < tasks.size(); ++i) {
            const EnqueueResult r = queue.enqueue(tasks[i]);
            check(r.status == Status::Ok,
                  "GPU queue enqueue must succeed for size " + std::to_string(sizes[i]));
            check(r.id != kInvalidTaskId, "GPU queue must return a valid id");
            ids.push_back(r.id);
        }

        for (std::size_t i = 0; i < ids.size(); ++i) {
            check(queue.wait(ids[i]) == TaskState::Completed,
                  "GPU queue task " + std::to_string(i) + " must complete");
            const TaskSnapshot snap = queue.task_snapshot(ids[i]);
            check(snap.state == TaskState::Completed, "snapshot must show Completed");
            check(result_matches(snap.result, tasks[i]),
                  "GPU queue result must equal A+B for size " + std::to_string(sizes[i]));
            check(snap.result.data == cpu_refs[i].data,
                  "GPU queue result must match the cpu reference bit-exactly for size " +
                      std::to_string(sizes[i]));
        }
    }

    // --- 2. FIFO execution order on the GPU queue ----------------------------
    {
        std::mutex order_mutex;
        std::vector<int> order;
        std::vector<TaskId> ids;
        const int count = 6;
        for (int i = 0; i < count; ++i) {
            const EnqueueResult r = queue.enqueue(std::unique_ptr<QueuedTask>(
                std::make_unique<OrderRecordingTask>(i, order_mutex, order)));
            check(r.status == Status::Ok, "GPU FIFO enqueue must succeed");
            ids.push_back(r.id);
        }
        for (const TaskId id : ids) {
            check(queue.wait(id) == TaskState::Completed, "GPU FIFO task must complete");
        }
        check(order.size() == static_cast<std::size_t>(count),
              "every GPU FIFO task must have executed exactly once");
        bool fifo = true;
        for (int i = 0; i < count; ++i) {
            if (order[static_cast<std::size_t>(i)] != i) fifo = false;
        }
        check(fifo, "GPU queue execution order must equal enqueue order (FIFO)");
    }

    // --- 3. Determinism: the same batch twice gives identical results --------
    {
        const VectorAddTask task = make_task(1024);
        const VectorAddResult cpu_ref = cpu_gpu.execute(task);

        const EnqueueResult r1 = queue.enqueue(task);
        check(r1.status == Status::Ok, "determinism run 1 enqueue");
        check(queue.wait(r1.id) == TaskState::Completed, "determinism run 1 must complete");

        const EnqueueResult r2 = queue.enqueue(task);
        check(r2.status == Status::Ok, "determinism run 2 enqueue");
        check(queue.wait(r2.id) == TaskState::Completed, "determinism run 2 must complete");

        const VectorAddResult first = queue.task_snapshot(r1.id).result;
        const VectorAddResult second = queue.task_snapshot(r2.id).result;
        check(first.status == Status::Ok && second.status == Status::Ok,
              "determinism runs must succeed");
        check(first.data == second.data, "repeated GPU queue runs must be deterministic");
        check(first.data == cpu_ref.data,
              "GPU queue determinism runs must match the cpu reference");
    }

    // --- 4. Shutdown order: queue first, Virtual GPU second ------------------
    {
        // The documented contract: the queue shuts down BEFORE its Virtual
        // GPU, so the worker is always gone before its execution target dies.
        queue.shutdown();
        check(queue.state() == QueueState::ShutDown, "the GPU queue must reach ShutDown");

        const EnqueueResult after = queue.enqueue(make_task(8));
        check(after.status == Status::NotInitialized,
              "enqueue() after shutdown must be refused (the gpu is still Ready)");
        check(after.id == kInvalidTaskId, "a refused enqueue must return no id");

        queue.shutdown();  // double shutdown: safe no-op
        check(queue.state() == QueueState::ShutDown, "double shutdown must stay ShutDown");
    }

    vulkan_gpu.shutdown();
    cpu_gpu.shutdown();

    if (failures == 0) {
        std::cout << "Task Queue GPU tests passed.\n";
        return 0;
    }
    return 1;
}
