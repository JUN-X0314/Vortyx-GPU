#include <algorithm>
#include <iostream>
#include <string>
#include "core/version.hpp"
#include "core/logger.hpp"
#include "core/device/discovery.hpp"
#include "core/device/device.hpp"
#include "core/compute/task.hpp"
#include "core/benchmark/benchmark.hpp"
#include "core/monitor/monitor.hpp"
#include "core/compute/runtime.hpp"
#include "core/resource/buffer.hpp"
#include "core/resource/resource.hpp"
#include "core/resource/resource_manager.hpp"
#include "core/vgpu/virtual_gpu.hpp"
#include "core/queue/task_queue.hpp"
#include "core/scheduler/scheduler.hpp"

#ifndef VORTYX_BUILD_CONFIG
#define VORTYX_BUILD_CONFIG "Unknown"
#endif

namespace {

std::string join_values(const std::vector<std::int32_t>& values, std::size_t max_count) {
    std::string out;
    const std::size_t limit = std::min(values.size(), max_count);
    for (std::size_t i = 0; i < limit; ++i) {
        if (i > 0) out += " ";
        out += std::to_string(values[i]);
    }
    if (values.size() > limit) out += " ...";
    return out;
}

// Logs a multi-line text (monitor/benchmark descriptions) one line per log
// entry so every line keeps its own level prefix.
void log_multiline(const std::string& text) {
    std::string line;
    for (const char character : text) {
        if (character == '\n') {
            if (!line.empty()) vortyx::log(vortyx::LogLevel::Info, line);
            line.clear();
        } else {
            line += character;
        }
    }
    if (!line.empty()) vortyx::log(vortyx::LogLevel::Info, line);
}

// Phase 5: runs one vector addition entirely through the Virtual GPU API,
// using explicit Buffer resources underneath:
//   create -> write (upload) -> execute -> read (download) -> release.
// Returns false and fills 'error' on any failure. Buffers are released
// explicitly with reset() at the end (RAII would release them at scope exit
// even on the error paths above).
bool run_vgpu_resource_vector_add(vortyx::vgpu::VirtualGpu& gpu,
                                  const std::vector<std::int32_t>& input_a,
                                  const std::vector<std::int32_t>& input_b,
                                  std::vector<std::int32_t>& output, std::string& error) {
    const std::size_t count = input_a.size();
    const std::size_t bytes = count * sizeof(std::int32_t);

    const vortyx::resource::BufferDesc desc_in =
        vortyx::resource::BufferDesc::of<std::int32_t>(count, vortyx::resource::ResourceAccess::Read);
    const vortyx::resource::BufferDesc desc_out =
        vortyx::resource::BufferDesc::of<std::int32_t>(count, vortyx::resource::ResourceAccess::Write);

    // 1. Resource creation through the Virtual GPU's Resource Manager, on the
    //    backend this Virtual GPU was explicitly configured for.
    vortyx::resource::ResourceManager* manager = gpu.resources();
    if (manager == nullptr) {
        error = "Virtual GPU has no Resource Manager (not initialized)";
        return false;
    }
    vortyx::resource::BufferResult ra = manager->create_buffer(desc_in, gpu.backend_name());
    if (ra.status != vortyx::compute::Status::Ok) {
        error = "buffer A: " + ra.error;
        return false;
    }
    vortyx::resource::BufferResult rb = manager->create_buffer(desc_in, gpu.backend_name());
    if (rb.status != vortyx::compute::Status::Ok) {
        error = "buffer B: " + rb.error;
        return false;
    }
    vortyx::resource::BufferResult rc = manager->create_buffer(desc_out, gpu.backend_name());
    if (rc.status != vortyx::compute::Status::Ok) {
        error = "buffer C: " + rc.error;
        return false;
    }

    bool ok = false;
    do {
        // 2. Write (upload) input data into the resources.
        const vortyx::compute::ComputeResult wa = ra.buffer.write(input_a.data(), bytes);
        if (wa.status != vortyx::compute::Status::Ok) {
            error = "write A: " + wa.error;
            break;
        }
        const vortyx::compute::ComputeResult wb = rb.buffer.write(input_b.data(), bytes);
        if (wb.status != vortyx::compute::Status::Ok) {
            error = "write B: " + wb.error;
            break;
        }

        // 3. Compute through the Virtual GPU: the backend reads/writes the
        //    resources directly.
        const vortyx::compute::ComputeResult exec = gpu.execute(ra.buffer, rb.buffer, rc.buffer);
        if (exec.status != vortyx::compute::Status::Ok) {
            error = "execute: " + exec.error;
            break;
        }

        // 4. Read (download) the result out of the output resource.
        output.assign(count, 0);
        const vortyx::compute::ComputeResult rd = rc.buffer.read(output.data(), bytes);
        if (rd.status != vortyx::compute::Status::Ok) {
            error = "read C: " + rd.error;
            break;
        }
        ok = true;
    } while (false);

    // 5. Explicit release. (The destructor would do the same on scope exit;
    // reset() just makes the release point visible here.)
    ra.buffer.reset();
    rb.buffer.reset();
    rc.buffer.reset();

    return ok;
}

// Honest one-line description of the device a Virtual GPU executes on.
std::string vgpu_device_text(const vortyx::vgpu::VirtualGpu& gpu) {
    const vortyx::device::DeviceInfo device = gpu.device_info();
    if (device.type == vortyx::device::DeviceType::Unknown) {
        return "no device reported";
    }
    return vortyx::device::describe(device);
}

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Vortyx GPU" << std::endl;
    std::cout << "  Version: " << VORTYX_VERSION_STRING << std::endl;
    std::cout << "  Phase:   16 (Adaptive Compute Fabric)" << std::endl;
    std::cout << "  Build:   " << VORTYX_BUILD_CONFIG << std::endl;
    std::cout << "========================================" << std::endl;

    vortyx::log(vortyx::LogLevel::Info, "Vortyx started.");

    // Hardware Discovery (Phase 2): find CPUs and GPUs on this machine.
    // GPU discovery failure is logged inside discover_devices() and must not
    // stop the program or CPU discovery.
    const std::vector<vortyx::device::DeviceInfo> devices = vortyx::device::discover_devices();

    const std::size_t cpu_count =
        static_cast<std::size_t>(std::count_if(devices.begin(), devices.end(),
            [](const vortyx::device::DeviceInfo& d) {
                return d.type == vortyx::device::DeviceType::Cpu;
            }));
    const std::size_t gpu_count =
        static_cast<std::size_t>(std::count_if(devices.begin(), devices.end(),
            [](const vortyx::device::DeviceInfo& d) {
                return d.type == vortyx::device::DeviceType::Gpu ||
                       d.type == vortyx::device::DeviceType::SoftwareGpu;
            }));

    vortyx::log(vortyx::LogLevel::Info,
                "Discovered " + std::to_string(devices.size()) + " device(s): " +
                    std::to_string(cpu_count) + " CPU, " +
                    std::to_string(gpu_count) + " GPU.");

    for (std::size_t i = 0; i < devices.size(); ++i) {
        vortyx::log(vortyx::LogLevel::Info,
                    "  Device " + std::to_string(i) + ": " +
                        vortyx::device::describe(devices[i]) +
                        " (via " +
                        (devices[i].backend.empty() ? std::string("unknown") : devices[i].backend) +
                        ")");
    }

    const std::vector<std::int32_t> demo_a = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<std::int32_t> demo_b = {10, 20, 30, 40, 50, 60, 70, 80};

    std::vector<std::int32_t> cpu_task_result;
    std::vector<std::int32_t> cpu_resource_result;

    // =====================================================================
    // 1. CPU Virtual GPU (Phase 5): the always-available logical device.
    // =====================================================================
    {
        vortyx::vgpu::VirtualGpuDesc desc;  // backend defaults to "cpu"
        vortyx::vgpu::VirtualGpu gpu;
        if (gpu.initialize(desc) != vortyx::compute::Status::Ok) {
            vortyx::log(vortyx::LogLevel::Error, "CPU Virtual GPU failed to initialize.");
            return 1;
        }
        vortyx::log(vortyx::LogLevel::Info,
                    "CPU Virtual GPU ready: backend='" + gpu.backend_name() + "', state=" +
                        vortyx::vgpu::to_string(gpu.state()) + ", device: " +
                        vgpu_device_text(gpu));

        // --- 1a. Task-based execution through the Virtual GPU ------------
        vortyx::compute::VectorAddTask demo;
        demo.a = demo_a;
        demo.b = demo_b;

        const vortyx::compute::VectorAddResult result = gpu.execute(demo);
        if (result.status == vortyx::compute::Status::Ok) {
            cpu_task_result = result.data;
            vortyx::log(vortyx::LogLevel::Info,
                        "Virtual GPU (cpu) task execution success: C = A + B (" +
                            join_values(result.data, 8) + ")");
        } else {
            vortyx::log(vortyx::LogLevel::Error,
                        std::string("Virtual GPU (cpu) task execution failed: ") +
                            to_string(result.status) + " - " + result.error);
        }

        // --- 1b. Resource-based execution through the Virtual GPU --------
        std::string resource_error;
        if (run_vgpu_resource_vector_add(gpu, demo_a, demo_b, cpu_resource_result,
                                         resource_error)) {
            vortyx::log(vortyx::LogLevel::Info,
                        "Virtual GPU (cpu) resource execution success: C = A + B (" +
                            join_values(cpu_resource_result, 8) + ")");
        } else {
            vortyx::log(vortyx::LogLevel::Error,
                        "Virtual GPU (cpu) resource execution failed: " + resource_error);
        }

        // --- 1c. Honest resource accounting ------------------------------
        const vortyx::resource::ResourceStats stats = gpu.resources()->stats();
        vortyx::log(vortyx::LogLevel::Info,
                    "CPU Virtual GPU resource stats: " + std::to_string(stats.live_buffers) +
                        " live buffer(s), " + std::to_string(stats.live_bytes) +
                        " live byte(s), " + std::to_string(stats.total_allocations) +
                        " total allocation(s).");

        gpu.shutdown();
    }

    // =====================================================================
    // 2. Vulkan Virtual GPU (Phase 5): created always, reported honestly.
    //    A known-but-unavailable backend is a normal environment state, so
    //    initialization succeeds; the truth is in backend_available().
    //    There is NO automatic fallback: the CPU path above was already an
    //    explicit CPU Virtual GPU.
    // =====================================================================
    {
        vortyx::vgpu::VirtualGpuDesc desc;
        desc.backend = "vulkan";
        vortyx::vgpu::VirtualGpu gpu;
        if (gpu.initialize(desc) != vortyx::compute::Status::Ok) {
            vortyx::log(vortyx::LogLevel::Error,
                        "Vulkan Virtual GPU initialization returned an unexpected failure.");
            return 1;
        }

        if (gpu.backend_available()) {
            const vortyx::device::DeviceInfo device = gpu.device_info();
            const bool software = (device.type == vortyx::device::DeviceType::SoftwareGpu);
            vortyx::log(vortyx::LogLevel::Info,
                        std::string("Vulkan Virtual GPU ready: backend='vulkan', device: ") +
                            (device.name.empty() ? std::string("unknown") : device.name) +
                            (software ? " (software/CPU implementation - not a hardware GPU)"
                                      : ""));
            vortyx::log(vortyx::LogLevel::Info,
                        "Device details: " + vgpu_device_text(gpu));

            // --- 2a. Task-based execution -------------------------------
            vortyx::compute::VectorAddTask demo;
            demo.a = demo_a;
            demo.b = demo_b;

            const vortyx::compute::VectorAddResult result = gpu.execute(demo);
            if (result.status == vortyx::compute::Status::Ok) {
                vortyx::log(vortyx::LogLevel::Info,
                            "Virtual GPU (vulkan) task execution success: C = A + B (" +
                                join_values(result.data, 8) + ")");
                const bool match = (result.data == cpu_task_result);
                vortyx::log(vortyx::LogLevel::Info,
                            match ? "Result verification: Virtual GPU (vulkan) output matches "
                                    "Virtual GPU (cpu) output."
                                  : "Result verification: MISMATCH between vulkan and cpu "
                                    "Virtual GPU outputs!");
            } else {
                vortyx::log(vortyx::LogLevel::Warning,
                            std::string("Virtual GPU (vulkan) task execution failed: ") +
                                to_string(result.status) + " - " + result.error);
            }

            // --- 2b. Resource-based execution ---------------------------
            std::vector<std::int32_t> vulkan_resource_result;
            std::string resource_error;
            if (run_vgpu_resource_vector_add(gpu, demo_a, demo_b, vulkan_resource_result,
                                             resource_error)) {
                vortyx::log(vortyx::LogLevel::Info,
                            "Virtual GPU (vulkan) resource execution success: C = A + B (" +
                                join_values(vulkan_resource_result, 8) + ")");
                const bool match = (vulkan_resource_result == cpu_resource_result);
                vortyx::log(vortyx::LogLevel::Info,
                            match ? "Resource verification: vulkan buffer output matches cpu "
                                    "buffer output."
                                  : "Resource verification: MISMATCH between vulkan and cpu "
                                    "buffer outputs!");
            } else {
                vortyx::log(vortyx::LogLevel::Warning,
                            "Virtual GPU (vulkan) resource execution failed: " + resource_error);
            }
        } else {
            vortyx::log(vortyx::LogLevel::Info,
                        "Vulkan Virtual GPU is not usable on this system: " +
                            gpu.backend_unavailable_reason());
            vortyx::log(vortyx::LogLevel::Info,
                        "No fallback was attempted: the CPU Virtual GPU above ran on the "
                        "explicitly configured cpu backend.");
        }

        gpu.shutdown();

        // --- 2c. Honest error behavior after shutdown (no crash) ---------
        vortyx::compute::VectorAddTask demo;
        demo.a = demo_a;
        demo.b = demo_b;
        const vortyx::compute::VectorAddResult refused = gpu.execute(demo);
        vortyx::log(vortyx::LogLevel::Info,
                    std::string("Post-shutdown execute() refused as expected: ") +
                        to_string(refused.status) + " - " + refused.error);
    }

    // =====================================================================
    // 3. Task Queue (Phase 6): asynchronous FIFO execution on ONE Virtual
    //    GPU. Tasks are submitted with enqueue() (never blocking), executed
    //    one at a time by the queue's worker thread, and queried through
    //    their TaskId. The queue is NOT a scheduler: it always uses the
    //    Virtual GPU it was given - here, the explicit CPU one.
    // =====================================================================
    {
        vortyx::vgpu::VirtualGpu gpu;  // backend defaults to "cpu" (explicit choice)
        if (gpu.initialize() != vortyx::compute::Status::Ok) {
            vortyx::log(vortyx::LogLevel::Error, "CPU Virtual GPU failed to initialize.");
            return 1;
        }

        vortyx::queue::TaskQueue queue;
        if (queue.initialize(gpu) != vortyx::compute::Status::Ok) {
            vortyx::log(vortyx::LogLevel::Error, "Task Queue failed to initialize.");
            gpu.shutdown();
            return 1;
        }
        vortyx::log(vortyx::LogLevel::Info,
                    "Task Queue ready: state=" + std::string(vortyx::queue::to_string(queue.state())) +
                        ", execution target='" + gpu.backend_name() + "', policy: FIFO, worker: 1 thread.");

        // Submit several tasks with distinct data; enqueue() returns
        // immediately with a TaskId - it never waits for execution.
        std::vector<vortyx::queue::TaskId> ids;
        std::vector<std::vector<std::int32_t>> expected;
        for (int t = 0; t < 4; ++t) {
            vortyx::compute::VectorAddTask task;
            const std::int32_t base = static_cast<std::int32_t>(t + 1);
            task.a = {base, base * 2, base * 3, base * 4};
            task.b = {10, 20, 30, 40};
            expected.push_back({task.a[0] + task.b[0], task.a[1] + task.b[1],
                                task.a[2] + task.b[2], task.a[3] + task.b[3]});
            const vortyx::queue::EnqueueResult r = queue.enqueue(task);
            if (r.status != vortyx::compute::Status::Ok) {
                vortyx::log(vortyx::LogLevel::Error,
                            std::string("Task Queue enqueue failed: ") + r.error);
                break;
            }
            ids.push_back(r.id);
            vortyx::log(vortyx::LogLevel::Info,
                        "Enqueued task " + std::to_string(r.id) + " (4 elements, worker executes FIFO in the background).");
        }

        // Wait for every task and verify its result against A+B.
        bool all_ok = true;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const vortyx::queue::TaskState state = queue.wait(ids[i]);
            const vortyx::queue::TaskSnapshot snap = queue.task_snapshot(ids[i]);
            const bool completed =
                state == vortyx::queue::TaskState::Completed &&
                snap.result.status == vortyx::compute::Status::Ok &&
                snap.result.data == expected[i];
            all_ok = all_ok && completed;
            vortyx::log(vortyx::LogLevel::Info,
                        "Task " + std::to_string(ids[i]) + ": state=" +
                            vortyx::queue::to_string(state) + ", C = A + B (" +
                            join_values(snap.result.data, 8) + ")" +
                            (completed ? " [verified]" : " [VERIFICATION FAILED]") +
                            (state == vortyx::queue::TaskState::Failed
                                 ? " - " + snap.result.error
                                 : ""));
        }
        vortyx::log(vortyx::LogLevel::Info,
                    all_ok ? "Task Queue verification: all queued tasks completed FIFO with correct results."
                           : "Task Queue verification: MISMATCH detected!");

        // Documented shutdown order: queue first (worker joined, every
        // accepted task processed), Virtual GPU second.
        queue.shutdown();
        vortyx::log(vortyx::LogLevel::Info,
                    "Queue shut down: state=" + std::string(vortyx::queue::to_string(queue.state())) +
                        ", task states remain queryable until the queue is destroyed.");
        gpu.shutdown();
    }

    // =====================================================================
    // 4. Basic Scheduler (Phase 7): decide WHERE to execute. The Scheduler
    //    only SELECTS the execution target (deterministic, explainable,
    //    based on real backend availability); the actual computation keeps
    //    flowing through the unchanged Virtual GPU path. An explicit request
    //    is never silently remapped; the automatic policy picks the first
    //    AVAILABLE backend in the documented order (vulkan > cpu).
    // =====================================================================
    {
        vortyx::scheduler::Scheduler scheduler;
        if (scheduler.initialize() != vortyx::compute::Status::Ok) {
            vortyx::log(vortyx::LogLevel::Error, "Scheduler failed to initialize.");
            return 1;
        }

        // --- 4a. Automatic selection: what CAN this system run on? -------
        vortyx::scheduler::SelectionResult selection = scheduler.select(vortyx::scheduler::SelectionRequest{});
        if (selection.status != vortyx::compute::Status::Ok) {
            vortyx::log(vortyx::LogLevel::Error,
                        "Automatic selection failed: " + selection.error);
            return 1;
        }
        vortyx::log(vortyx::LogLevel::Info,
                    "Automatic selection: backend='" + selection.backend + "', device: " +
                        (selection.device.name.empty() ? std::string("unknown")
                                                       : selection.device.name));
        vortyx::log(vortyx::LogLevel::Info, "Selection reason: " + selection.reason);

        std::vector<std::int32_t> auto_selected_result;

        // --- 4b. Execute through the SELECTED target (unchanged path) ----
        {
            vortyx::vgpu::VirtualGpuDesc desc;
            desc.backend = selection.backend;  // the Scheduler's choice, verbatim
            vortyx::vgpu::VirtualGpu gpu;
            if (gpu.initialize(desc) != vortyx::compute::Status::Ok) {
                vortyx::log(vortyx::LogLevel::Error, "Virtual GPU from selection failed to initialize.");
                return 1;
            }

            vortyx::compute::VectorAddTask demo;
            demo.a = demo_a;
            demo.b = demo_b;
            const vortyx::compute::VectorAddResult result = gpu.execute(demo);
            if (result.status == vortyx::compute::Status::Ok) {
                auto_selected_result = result.data;
                vortyx::log(vortyx::LogLevel::Info,
                            "Execution on the selected backend ('" + gpu.backend_name() +
                                "'): C = A + B (" + join_values(result.data, 8) + ")");
            } else {
                vortyx::log(vortyx::LogLevel::Error,
                            std::string("Execution on the selected backend failed: ") +
                                to_string(result.status) + " - " + result.error);
            }
            gpu.shutdown();
        }

        // --- 4c. Explicit 'cpu' request: honored verbatim, never remapped.
        {
            vortyx::scheduler::SelectionRequest cpu_request;
            cpu_request.mode = vortyx::scheduler::SelectionMode::ExplicitBackend;
            cpu_request.backend = "cpu";
            const vortyx::scheduler::SelectionResult cpu_selection = scheduler.select(cpu_request);
            if (cpu_selection.status == vortyx::compute::Status::Ok) {
                vortyx::log(vortyx::LogLevel::Info,
                            "Explicit selection: backend='" + cpu_selection.backend +
                                "' (" + cpu_selection.reason + ")");

                vortyx::vgpu::VirtualGpuDesc desc;
                desc.backend = cpu_selection.backend;
                vortyx::vgpu::VirtualGpu gpu;
                gpu.initialize(desc);

                vortyx::compute::VectorAddTask demo;
                demo.a = demo_a;
                demo.b = demo_b;
                const vortyx::compute::VectorAddResult result = gpu.execute(demo);
                if (result.status == vortyx::compute::Status::Ok) {
                    const bool match = (result.data == auto_selected_result);
                    vortyx::log(vortyx::LogLevel::Info,
                                match ? "Verification: explicit-cpu execution matches the "
                                        "automatically selected backend's output."
                                      : "Verification: MISMATCH between explicit-cpu and "
                                        "automatically selected backend outputs!");
                } else {
                    vortyx::log(vortyx::LogLevel::Error,
                                std::string("Explicit-cpu execution failed: ") +
                                    to_string(result.status) + " - " + result.error);
                }
                gpu.shutdown();
            } else {
                vortyx::log(vortyx::LogLevel::Error,
                            "Explicit 'cpu' selection failed unexpectedly: " + cpu_selection.error);
            }
        }

        // --- 4d. Explicit 'vulkan' request: honored or honestly refused --
        //         (never silently rerouted to the cpu backend).
        {
            vortyx::scheduler::SelectionRequest vk_request;
            vk_request.mode = vortyx::scheduler::SelectionMode::ExplicitBackend;
            vk_request.backend = "vulkan";
            const vortyx::scheduler::SelectionResult vk_selection = scheduler.select(vk_request);
            if (vk_selection.status == vortyx::compute::Status::Ok) {
                vortyx::log(vortyx::LogLevel::Info,
                            "Explicit selection: backend='vulkan', device: " +
                                (vk_selection.device.name.empty() ? std::string("unknown")
                                                                  : vk_selection.device.name));
            } else {
                vortyx::log(vortyx::LogLevel::Info,
                            "Explicit 'vulkan' selection refused as expected on this system: " +
                                vk_selection.error);
                vortyx::log(vortyx::LogLevel::Info,
                            "No fallback was attempted: an explicit request is never rerouted "
                                "to another backend.");
            }
        }

        scheduler.shutdown();  // shares nothing with the Virtual GPUs above
    }

    // =====================================================================
    // 5. Benchmark + Resource Monitoring (Phase 8): measure the REAL
    //    execution path (Virtual GPU -> Runtime -> Resource Manager ->
    //    Backend) and observe the execution environment. Phase 8 is
    //    measurement and observation ONLY: nothing here changes the Phase 7
    //    Scheduler's fixed policy, and no metric is invented — the monitor
    //    reports exactly what the Runtime's own APIs and the standard
    //    library can honestly provide, marking everything else unavailable.
    // =====================================================================
    {
        vortyx::monitor::ResourceMonitor monitor;

        // --- 5a. Resource snapshot: what is honestly observable right now
        {
            vortyx::compute::Runtime runtime;
            if (runtime.initialize() == vortyx::compute::Status::Ok) {
                const vortyx::monitor::ResourceSnapshot snap = monitor.snapshot(runtime);
                log_multiline("Environment observation:\n" + vortyx::monitor::describe(snap));
                runtime.shutdown();
            }
        }

        // --- 5b. CPU benchmark: real measurements over the real path -----
        std::vector<std::int32_t> benchmark_reference;
        {
            vortyx::vgpu::VirtualGpu gpu;  // explicit cpu backend
            if (gpu.initialize() != vortyx::compute::Status::Ok) {
                vortyx::log(vortyx::LogLevel::Error, "CPU Virtual GPU for benchmarking failed to initialize.");
                return 1;
            }

            vortyx::compute::VectorAddTask task;
            const std::size_t count = 8192;
            task.a.resize(count);
            task.b.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                task.a[i] = static_cast<std::int32_t>(i % 1000) - 300;
                task.b[i] = static_cast<std::int32_t>((i * 7) % 500) + 11;
                benchmark_reference.push_back(task.a[i] + task.b[i]);
            }

            vortyx::benchmark::BenchmarkConfig config;
            config.iterations = 10;
            config.warmup_iterations = 1;
            const vortyx::benchmark::BenchmarkResult result =
                vortyx::benchmark::benchmark_vector_add(gpu, task, config);

            log_multiline(vortyx::benchmark::describe(result));
            if (result.status == vortyx::compute::Status::Ok) {
                // Machine-readable form of the SAME real numbers (stable
                // key=value schema; timings carry their unit in the key).
                std::string flat;
                for (const auto& pair : vortyx::benchmark::to_key_values(result)) {
                    if (!flat.empty()) flat += " ";
                    flat += pair.first + "=" + pair.second;
                }
                vortyx::log(vortyx::LogLevel::Info, "Benchmark key=value export: " + flat);
            }

            // Honest leak observation: the benchmark released everything it
            // allocated (RAII), so the Virtual GPU's manager holds nothing.
            const vortyx::resource::ResourceStats stats = gpu.resources()->stats();
            vortyx::log(vortyx::LogLevel::Info,
                        "Post-benchmark resource stats: " + std::to_string(stats.live_buffers) +
                            " live buffer(s), " + std::to_string(stats.live_bytes) +
                            " live byte(s), " + std::to_string(stats.total_allocations) +
                            " total allocation(s) (live must be 0: benchmark buffers are RAII).");
            gpu.shutdown();
        }

        // --- 5c. Vulkan benchmark: real device only, SKIP otherwise ------
        {
            vortyx::vgpu::VirtualGpuDesc desc;
            desc.backend = "vulkan";
            vortyx::vgpu::VirtualGpu gpu;
            gpu.initialize(desc);  // a known backend always initializes
            if (gpu.backend_available()) {
                vortyx::compute::VectorAddTask task;
                const std::size_t count = 8192;
                task.a.resize(count);
                task.b.resize(count);
                for (std::size_t i = 0; i < count; ++i) {
                    task.a[i] = static_cast<std::int32_t>(i % 1000) - 300;
                    task.b[i] = static_cast<std::int32_t>((i * 7) % 500) + 11;
                }

                vortyx::benchmark::BenchmarkConfig config;
                config.iterations = 10;
                config.warmup_iterations = 1;
                const vortyx::benchmark::BenchmarkResult result =
                    vortyx::benchmark::benchmark_vector_add(gpu, task, config);
                log_multiline(vortyx::benchmark::describe(result));

                if (result.status == vortyx::compute::Status::Ok) {
                    // Cross-backend correctness on the SAME task, checked
                    // against the independently computed reference.
                    vortyx::compute::VectorAddResult cross = gpu.execute(task);
                    const bool match = cross.status == vortyx::compute::Status::Ok &&
                                       cross.data == benchmark_reference;
                    vortyx::log(vortyx::LogLevel::Info,
                                match ? "Verification: vulkan benchmark target output matches "
                                        "the host reference (bit-exact)."
                                      : "Verification: MISMATCH between vulkan output and the "
                                        "host reference!");
                }
            } else {
                vortyx::log(vortyx::LogLevel::Info,
                            "Vulkan benchmark skipped: backend not usable on this system (" +
                                gpu.backend_unavailable_reason() +
                                "). No fallback was attempted and nothing was faked.");
            }
            gpu.shutdown();
        }

        // --- 5d. Snapshot after the benchmarks: repeated observation is
        //        stable and the standalone environment is unchanged.
        {
            vortyx::compute::Runtime runtime;
            if (runtime.initialize() == vortyx::compute::Status::Ok) {
                const vortyx::monitor::ResourceSnapshot snap = monitor.snapshot(runtime);
                vortyx::log(vortyx::LogLevel::Info,
                            "Post-benchmark observation: hardware threads: " +
                                (snap.hardware_threads.has_value()
                                     ? std::to_string(*snap.hardware_threads)
                                     : std::string("unknown")) +
                                ", backends available: " +
                                std::to_string(snap.available_backend_count()) + "/" +
                                std::to_string(snap.backends.size()) + ".");
                runtime.shutdown();
            }
        }
    }

    // =====================================================================
    // 6. Compute Engine (Phase 10): generic ComputeTasks beyond vector
    //    addition — elementwise int32 ops (VectorAdd / VectorMultiply /
    //    VectorScale) with bit-exact cross-backend semantics, plus honest
    //    synchronous BATCH execution (one result per task, partial success
    //    allowed, failures never hidden). This is still the same execution
    //    path: Virtual GPU -> Runtime -> Resource Manager -> Backend.
    // =====================================================================
    {
        vortyx::vgpu::VirtualGpu gpu;  // explicit cpu backend
        if (gpu.initialize() != vortyx::compute::Status::Ok) {
            vortyx::log(vortyx::LogLevel::Error, "CPU Virtual GPU for the Compute Engine failed to initialize.");
            return 1;
        }

        // --- 6a. VectorMultiply: C[i] = A[i] * B[i] ----------------------
        vortyx::compute::ComputeTask multiply;
        multiply.op = vortyx::compute::ComputeOp::VectorMultiply;
        multiply.a = {2, -3, 10, 7};
        multiply.b = {5, 4, -1, 0};
        const vortyx::compute::ComputeTaskResult mul_result = gpu.execute(multiply);
        if (mul_result.status == vortyx::compute::Status::Ok) {
            vortyx::log(vortyx::LogLevel::Info,
                        "Compute Engine (cpu) VectorMultiply success: C = A * B (" +
                            join_values(mul_result.data, 8) + ")");
        } else {
            vortyx::log(vortyx::LogLevel::Error,
                        std::string("Compute Engine VectorMultiply failed: ") +
                            to_string(mul_result.status) + " - " + mul_result.error);
        }

        // --- 6b. VectorScale: C[i] = A[i] * scalar -----------------------
        vortyx::compute::ComputeTask scale;
        scale.op = vortyx::compute::ComputeOp::VectorScale;
        scale.a = {1, -2, 3, -4};
        scale.scalar = -7;
        const vortyx::compute::ComputeTaskResult scale_result = gpu.execute(scale);
        if (scale_result.status == vortyx::compute::Status::Ok) {
            vortyx::log(vortyx::LogLevel::Info,
                        "Compute Engine (cpu) VectorScale success: C = A * (-7) (" +
                            join_values(scale_result.data, 8) + ")");
        } else {
            vortyx::log(vortyx::LogLevel::Error,
                        std::string("Compute Engine VectorScale failed: ") +
                            to_string(scale_result.status) + " - " + scale_result.error);
        }

        // --- 6c. Batch execution: per-task honesty, partial success ------
        std::vector<vortyx::compute::ComputeTask> batch_tasks;
        batch_tasks.push_back(scale);                    // 0: valid
        batch_tasks.push_back(multiply);                 // 1: valid
        vortyx::compute::ComputeTask invalid = scale;
        invalid.b = {1, 2, 3, 4};                        // 2: invalid (scale takes no second input)
        batch_tasks.push_back(invalid);
        batch_tasks.push_back(multiply);                 // 3: valid

        const vortyx::compute::BatchResult batch = gpu.execute_batch(batch_tasks);
        vortyx::log(vortyx::LogLevel::Info,
                    std::string("Batch executed in submission order: ") +
                        to_string(batch.status) + " (" + std::to_string(batch.succeeded) +
                            " succeeded, " + std::to_string(batch.failed) + " failed).");
        for (std::size_t i = 0; i < batch.results.size(); ++i) {
            vortyx::log(vortyx::LogLevel::Info,
                        "  Batch task " + std::to_string(i) + ": " +
                            to_string(batch.results[i].status) +
                            (batch.results[i].status == vortyx::compute::Status::Ok
                                 ? " (results kept, never discarded)"
                                 : " - " + batch.results[i].error));
        }

        gpu.shutdown();
    }

    vortyx::log(vortyx::LogLevel::Info, "Hardware discovery: implemented (Phase 2).");
    vortyx::log(vortyx::LogLevel::Info, "Compute Runtime: implemented (Phase 3) - CPU backend always available, Vulkan GPU backend when a Vulkan device is present.");
    vortyx::log(vortyx::LogLevel::Info, "Compute Resource Manager: implemented (Phase 4) - Buffer resources with explicit host/device memory, upload/download, RAII ownership and safe shutdown.");
    vortyx::log(vortyx::LogLevel::Info, "Virtual GPU: implemented (Phase 5) - one logical compute device per explicitly chosen backend; no automatic backend choice, no silent fallback.");
    vortyx::log(vortyx::LogLevel::Info, "Task Queue: implemented (Phase 6) - FIFO task submission, one worker thread, asynchronous execution, per-task id/state/result, drain-on-shutdown.");
    vortyx::log(vortyx::LogLevel::Info, "Basic Scheduler: implemented (Phase 7) - deterministic execution-target selection (explicit request or automatic vulkan>cpu policy) from real backend availability; selection only, execution stays in the Virtual GPU path.");
    vortyx::log(vortyx::LogLevel::Info, "Benchmark: implemented (Phase 8) - real-path measurement (VirtualGpu::execute end to end) with warmup, repeated iterations, min/average/median/max statistics, throughput and per-iteration correctness verification; measurements only, no performance claims.");
    vortyx::log(vortyx::LogLevel::Info, "Resource Monitoring: implemented (Phase 8) - point-in-time ResourceSnapshots over the Runtime's real backend/device/allocation state; unsupported metrics have no representation instead of fake values; informationally independent of the Scheduler.");
    vortyx::log(vortyx::LogLevel::Info, "Stabilization: implemented (Phase 9) - full stability audit of Phase 1~8; foreign Resource/Buffer handles from another Runtime are now rejected explicitly instead of being silently resolved by colliding per-manager ids; Vulkan execution failures preserve the failing Vulkan call and its VkResult; Runtime/VirtualGpu threading contracts and Buffer::valid() semantics documented; CI verifies the CPU-only build explicitly.");
    vortyx::log(vortyx::LogLevel::Info, "Compute Engine: implemented (Phase 10) - generic ComputeTask layer (elementwise int32 VectorAdd / VectorMultiply / VectorScale, bit-exact on every backend incl. overflow), one shared task->buffer->dispatch path for the legacy and generic APIs, synchronous batch execution with per-task results and honest partial success, CPU fork-join parallel execution for large workloads (bit-identical to sequential), per-op benchmark capability over the real execute() path; task data-parallel domain documented as the future partitioning seam.");
    vortyx::log(vortyx::LogLevel::Info, "Platform Foundation: implemented (Phase 11) - provider-neutral control-plane layer (vortyx::platform, src/platform/): device identity + self-reported metadata, JobEnvelope/ResultEnvelope transport contracts (no data payload, ComputeTask stays local), job lifecycle with documented transitions, AuthN/AuthZ boundary with the RLS-equivalent ownership rule, provider-neutral IPlatformStore with the local/mock InMemoryPlatformStore, a strict standard-library JSON module and the API contract codec pinned by tests; Supabase schema/RLS migration and the Vercel-ready API layer (platform/api) prepared for the owner's post-Phase-11 deployment; the compute core knows nothing about any of it (VORTYX_ENABLE_PLATFORM=OFF builds exactly like Phase 10).");
    vortyx::log(vortyx::LogLevel::Info, "Distributed Foundation: implemented (Phase 12) - vortyx::distributed (src/distributed/, separate static lib over the platform layer): multi-device registry with atomic resource leases and cluster revisions, deterministic element-range sharding, round_robin/least_loaded/capability_fit placement policies, workers executing shards through the unchanged local Runtime (one per device), loopback transport, bounded retry with stable failure codes, duplicate-safe deterministic aggregation, optional Phase 11 store integration, and the local multi-device simulator; real network transport and remote execution are future phases (the compute path knows nothing about any of it).");
    vortyx::log(vortyx::LogLevel::Info, "AI/ML Acceleration + Tensor Layer: implemented (Phase 13) - vortyx::tensor (src/tensor/, separate static lib over the distributed layer): N-D tensors with checked shape/stride/broadcast arithmetic and five explicit dtypes (fp32/fp16/bf16/int32/int8), device placement on the Phase 11/12 identity system, tensor storage allocated ONLY through the Phase 4 resource system, a validated op surface (matmul/gemm/elementwise with NumPy-style broadcasting/reductions/activations/softmax with max-subtraction/transpose/reshape) with a deterministic CPU reference implementation, a runtime adapter routing int32 elementwise ops into the REAL compute engine, deterministic graph planning with liveness-safe buffer-slot reuse, capability-based backend dispatch, and capability-based tensor placement over Phase 12 cluster snapshots (read-only); no hardware tensor kernels, no fp64, no autograd, no quantized kernels, no cross-device transfer, no model file formats - each absence is a documented non-goal.");
    vortyx::log(vortyx::LogLevel::Info, "Production GPU Platform / Serviceization: implemented (Phase 14) - vortyx::service (src/service/, separate static lib over the tensor layer): projects with membership roles (owner/admin/member/viewer) and a pure authorization table, a project quota ledger with exactly-once release, deterministic fixed-window rate limiting over the injected clock, a provider-neutral FIFO job queue, the full submission flow (authentication -> validation -> project authorization -> rate limit -> quota -> queue -> the UNCHANGED Phase 12 orchestrator -> terminal finalize), bounded audit events (no secrets by construction), real-counter metrics only, honest per-component health (an unattached provider reports not_configured, never healthy), artifact METADATA registry (no payload storage), and a machine-readable JSON contract in the strict platform subset; no HTTP server in the C++ core, no Supabase/Redis/cloud integration, no billing - each absence is a documented non-goal.");
    vortyx::log(vortyx::LogLevel::Info, "Production Platform Integration: implemented (Phase 15) - the platform as ONE product: contract fixes in the service layer (the single-owner invariant: the Owner role is never grantable through any membership path; the bounded artifact metadata registry with creator/admin deletion; privileged cross-user cancellation through an explicit trusted ServiceCancellation context that cannot be minted outside the service facade, audited as privileged, with the Phase 14 sleep-poll handoff replaced by atomic cancellation-intent delivery in the Phase 12 orchestrator), the native execution boundary (vortyx::worker, src/worker/: the vortyx_worker_agent process claims queued jobs from the control plane over the worker protocol - claim/lease-heartbeat/complete with an HTTP/1.1 client, plain http only, TLS termination documented as the deployment's job - and executes them on the UNCHANGED Phase 12 distributed stack over local simulated devices with deterministic payload synthesis), the full service API surface (projects/members/jobs/quota/usage/artifacts/audit/metrics over memory or Supabase-PostgreSQL persistence with RLS, additive migration 0003, an atomic claim RPC, a DB-enforced quota trigger and a centralized rate-limit RPC), reconciliation of stale worker leases, and the Vortyx web console (platform/web, a no-build static SPA speaking Supabase Auth REST + the service API); the local end-to-end path (web/API -> worker protocol -> Phase 12 execution -> result -> audit) is real and verified.");
    vortyx::log(vortyx::LogLevel::Info, "Not implemented yet: hardware GPU tensor kernels (Tensor Core/CUDA/ROCm - none exist in this repository and none are claimed), FP64, autograd/training, quantized kernels, model file formats (ONNX/PyTorch/SafeTensors), cross-device tensor transfer, distributed graph partitioning, real network distributed transport between orchestrator and workers (the Phase 12 transport is the in-process loopback), load balancing, work stealing, priority scheduling, memory pooling/suballocation, real GPU telemetry (utilization/VRAM/temperature are never displayed anywhere), TLS in the C++ worker (terminate at a reverse proxy), artifact payload storage (metadata only), billing/marketplace, multi-region clusters, ownership transfer.");

    return 0;
}
