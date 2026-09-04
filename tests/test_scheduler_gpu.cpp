// Basic Scheduler GPU-path tests (Phase 7).
//
// Design rules (per project requirements):
//  - When the Vulkan backend is unavailable, the test exits successfully with
//    an explicit, visible note saying it did NOT run. It is never reported as
//    a GPU selection success.
//  - When the backend IS available, the documented automatic policy must be
//    proven against the real device: automatic selection picks 'vulkan',
//    explicit requests reach it, and tasks queued through a Virtual GPU
//    built FROM the selection produce bit-exact correct results.
//  - No hardcoded hardware expectations (no vendor names, no device counts).

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "core/compute/task.hpp"
#include "core/device/device.hpp"
#include "core/queue/task_queue.hpp"
#include "core/scheduler/scheduler.hpp"
#include "core/vgpu/virtual_gpu.hpp"

using vortyx::compute::Status;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;
using vortyx::queue::EnqueueResult;
using vortyx::queue::TaskQueue;
using vortyx::queue::TaskState;
using vortyx::scheduler::Scheduler;
using vortyx::scheduler::SelectionMode;
using vortyx::scheduler::SelectionRequest;
using vortyx::scheduler::SelectionResult;
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

}  // namespace

int main() {
    // The reference CPU Virtual GPU exists in every environment (created
    // first so every GPU result below is compared against a real,
    // independently executed CPU result).
    VirtualGpu cpu_gpu;
    VirtualGpuDesc cpu_desc;
    cpu_desc.backend = "cpu";
    if (cpu_gpu.initialize(cpu_desc) != Status::Ok) {
        std::cerr << "FAIL: cpu VirtualGpu initialize() failed\n";
        return 1;
    }

    // An independent Vulkan Virtual GPU probe decides whether this machine
    // can run the GPU-path assertions at all.
    VirtualGpu probe;
    VirtualGpuDesc probe_desc;
    probe_desc.backend = "vulkan";
    if (probe.initialize(probe_desc) != Status::Ok) {
        std::cerr << "FAIL: vulkan VirtualGpu probe initialize() must not fail for a known "
                     "backend\n";
        return 1;
    }

    if (!probe.backend_available()) {
        // Environment without a usable Vulkan device: a normal, non-fatal
        // condition. Report it clearly; do NOT fake success.
        std::cout << "SKIPPED (environment): Vulkan backend unavailable - "
                  << probe.backend_unavailable_reason() << "\n";
        std::cout << "Note: GPU selection and execution were NOT tested on this machine. "
                  << "The Scheduler CPU path is covered by SchedulerTest.\n";
        probe.shutdown();
        cpu_gpu.shutdown();
        return 0;
    }

    // =====================================================================
    // The Vulkan backend IS available on this machine: prove the policy
    // against the real device.
    // =====================================================================
    Scheduler scheduler;
    check(scheduler.initialize() == Status::Ok, "scheduler initialize()");

    // 1. Automatic selection must pick 'vulkan' (the documented policy:
    //    prefer the highest-priority backend that is REALLY available).
    SelectionRequest auto_req;  // Automatic, no backend named
    const SelectionResult selection = scheduler.select(auto_req);
    check(selection.status == Status::Ok, "automatic selection must succeed");
    check(selection.backend == "vulkan",
          "automatic policy with a usable Vulkan device must select 'vulkan' (got: '" +
              selection.backend + "')");
    check(!selection.reason.empty() &&
              selection.reason.find("automatic policy") != std::string::npos,
          "the automatic choice must be explainable");
    check(selection.device.type == vortyx::device::DeviceType::Gpu ||
              selection.device.type == vortyx::device::DeviceType::SoftwareGpu,
          "the selected device must be typed Gpu or SoftwareGpu (never Unknown)");
    check(!selection.device.name.empty(),
          "an available Vulkan device must report its name");
    // The selected device must be the SAME device an executing Virtual GPU
    // reports (the selection reflects reality, not a guess).
    check(selection.device.type == probe.device_info().type &&
              selection.device.name == probe.device_info().name,
          "the selection's device must match the executing Virtual GPU's device");

    // 2. Determinism: repeated selections on the same system are identical.
    {
        const SelectionResult again = scheduler.select(auto_req);
        check(again.backend == "vulkan" && again.reason == selection.reason,
              "repeated automatic selections must be identical on the same system");
    }

    // 3. Explicit 'vulkan' succeeds and names the real device.
    SelectionRequest vk_req;
    vk_req.mode = SelectionMode::ExplicitBackend;
    vk_req.backend = "vulkan";
    const SelectionResult vk_sel = scheduler.select(vk_req);
    check(vk_sel.status == Status::Ok, "explicit 'vulkan' must succeed here");
    check(vk_sel.backend == "vulkan", "explicit 'vulkan' must select exactly 'vulkan'");
    check(vk_sel.device.name == probe.device_info().name,
          "explicit selection must report the real Vulkan device name");

    // 4. Explicit 'cpu' still works and is never overridden by the automatic
    //    preference: an explicit request is honored verbatim.
    SelectionRequest cpu_req;
    cpu_req.mode = SelectionMode::ExplicitBackend;
    cpu_req.backend = "cpu";
    const SelectionResult cpu_sel = scheduler.select(cpu_req);
    check(cpu_sel.status == Status::Ok && cpu_sel.backend == "cpu",
          "explicit 'cpu' must be honored even when vulkan is available");
    check(cpu_sel.device.type == vortyx::device::DeviceType::Cpu,
          "explicit 'cpu' must report a Cpu device");

    // 5. Full integration: selection -> Virtual GPU -> TaskQueue -> real GPU
    //    execution -> bit-exact CPU comparison.
    {
        VirtualGpu gpu;
        VirtualGpuDesc desc;
        desc.backend = selection.backend;  // the Scheduler's choice, verbatim
        check(gpu.initialize(desc) == Status::Ok,
              "Virtual GPU from the selection must initialize");
        check(gpu.backend_name() == "vulkan",
              "the Virtual GPU must run on the selected 'vulkan' backend");

        TaskQueue queue;
        check(queue.initialize(gpu) == Status::Ok, "TaskQueue on the selected Virtual GPU");

        // Several sizes, including one that is not a multiple of the
        // 64-element workgroup (guards the shader bounds check).
        for (const std::size_t size : {std::size_t{4}, std::size_t{64}, std::size_t{1024},
                                       std::size_t{5000}}) {
            const VectorAddTask task = make_task(size);
            const VectorAddResult cpu_ref = cpu_gpu.execute(task);
            check(cpu_ref.status == Status::Ok, "cpu reference execution for size " +
                                                     std::to_string(size));

            const EnqueueResult enq = queue.enqueue(task);
            check(enq.status == Status::Ok, "enqueue for size " + std::to_string(size));
            check(queue.wait(enq.id) == TaskState::Completed,
                  "queued GPU task must complete for size " + std::to_string(size));

            const auto snap = queue.task_snapshot(enq.id);
            check(snap.result.status == Status::Ok && result_matches(snap.result, task),
                  "GPU result must equal A+B for size " + std::to_string(size));
            check(snap.result.data == cpu_ref.data,
                  "GPU result must match the cpu reference bit-exactly for size " +
                      std::to_string(size));
        }

        queue.shutdown();  // documented order: queue first ...
        gpu.shutdown();    // ... then its Virtual GPU
    }

    // 6. The Scheduler itself never executes: shutting it down (even before
    //    the Virtual GPUs) must not affect any of them.
    scheduler.shutdown();
    check(scheduler.state() == vortyx::scheduler::State::ShutDown, "scheduler shutdown");
    {
        const VectorAddTask task = make_task(16);
        const VectorAddResult still_works = cpu_gpu.execute(task);
        check(still_works.status == Status::Ok && result_matches(still_works, task),
              "a Virtual GPU must keep working after the Scheduler shut down "
              "(the Scheduler owns nothing it used)");
    }

    probe.shutdown();
    cpu_gpu.shutdown();

    if (failures == 0) {
        std::cout << "Basic Scheduler GPU tests passed.\n";
        return 0;
    }
    return 1;
}
