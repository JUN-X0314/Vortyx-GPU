// Resource Monitoring GPU-path tests (Phase 8).
//
// Design rules (per project requirements):
//  - When the Vulkan backend is unavailable, the test exits successfully
//    with an explicit, visible note saying it did NOT run. Vulkan
//    monitoring is never faked.
//  - When the backend IS available, the monitor's observation of it must
//    match the real, executing stack: availability true, device info equal
//    to the executing Virtual GPU's own report, and a software Vulkan
//    implementation (e.g. Mesa lavapipe) still labeled SoftwareGpu — never
//    misreported as a hardware GPU.
//  - The monitor stays a pure observer: taking snapshots around real
//    execution must not change any execution result.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "core/compute/task.hpp"
#include "core/compute/runtime.hpp"
#include "core/device/device.hpp"
#include "core/monitor/monitor.hpp"
#include "core/vgpu/virtual_gpu.hpp"

using vortyx::compute::Runtime;
using vortyx::compute::Status;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;
using vortyx::monitor::BackendObservation;
using vortyx::monitor::ResourceMonitor;
using vortyx::monitor::ResourceSnapshot;
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

bool devices_equal(const vortyx::device::DeviceInfo& a, const vortyx::device::DeviceInfo& b) {
    return a.type == b.type && a.name == b.name && a.vendor == b.vendor && a.id == b.id &&
           a.backend == b.backend && a.logical_processors == b.logical_processors &&
           a.physical_cores == b.physical_cores && a.memory_bytes == b.memory_bytes &&
           a.shared_memory_bytes == b.shared_memory_bytes;
}

VectorAddTask make_task(std::size_t count) {
    VectorAddTask task;
    task.a.resize(count);
    task.b.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
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
        // condition. Report it clearly; do NOT fake GPU monitoring.
        std::cout << "SKIPPED (environment): Vulkan backend unavailable - "
                  << probe.backend_unavailable_reason() << "\n";
        std::cout << "Note: Vulkan monitoring was NOT tested on this machine. The monitoring "
                     "CPU path is covered by MonitorTest.\n";
        probe.shutdown();
        return 0;
    }

    // The probe's own honest report is the ground truth the monitor must
    // reproduce (same discovery source, re-queried).
    const vortyx::device::DeviceInfo probe_device = probe.device_info();
    probe.shutdown();

    // =====================================================================
    // The Vulkan backend IS available: the monitor must see exactly what
    // the Runtime sees — no more, no less, no relabeling.
    // =====================================================================
    {
        Runtime runtime;
        check(runtime.initialize() == Status::Ok, "1: Runtime initializes");
        ResourceMonitor monitor;
        const ResourceSnapshot snap = monitor.snapshot(runtime);

        check(snap.runtime_observed, "1: runtime observed");
        const BackendObservation* vulkan = snap.find_backend("vulkan");
        check(vulkan != nullptr, "1: vulkan backend observed");
        if (vulkan == nullptr) {
            runtime.shutdown();
            std::cerr << failures << " monitoring GPU-path check(s) FAILED.\n";
            return 1;
        }

        check(vulkan->available, "2: vulkan reported available (matches the probe)");
        check(vulkan->unavailable_reason.empty(),
              "2: available backend carries no unavailable reason");
        check(devices_equal(vulkan->device, runtime.backend_device("vulkan")),
              "3: vulkan DeviceInfo equals the Runtime's own answer");
        check(vulkan->device.type == probe_device.type,
              "3: vulkan DeviceInfo equals the executing Virtual GPU's report");
        check(vulkan->device.name == probe_device.name,
              "3: vulkan device name equals the executing Virtual GPU's report");
        // Software-device honesty: whatever kind the backend reports is
        // kept verbatim (lavapipe stays SoftwareGpu; hardware stays Gpu).
        check(vulkan->device.type == vortyx::device::DeviceType::Gpu ||
                  vulkan->device.type == vortyx::device::DeviceType::SoftwareGpu,
              "3: device kind is one of the honest GPU representations");

        // --- 4. Observing around real execution changes nothing ----------
        const VectorAddTask task = make_task(1024);
        const ResourceSnapshot before = monitor.snapshot(runtime);

        const VectorAddResult result = runtime.execute(task, "vulkan");
        check(result.status == Status::Ok && result_matches(result, task),
              "4: real Vulkan execution through the observed Runtime succeeds");

        const ResourceSnapshot after = monitor.snapshot(runtime);
        check(before.backends.size() == after.backends.size(),
              "4: backend observations unchanged by execution");
        check(before.find_backend("vulkan")->available ==
                      after.find_backend("vulkan")->available,
              "4: vulkan availability unchanged by execution");
        check(after.total_allocations >= before.total_allocations,
              "4: allocation accounting only moves forward");
        // Task execution releases its buffers (RAII): the live count lands
        // back where it was — observable proof of no leak.
        check(after.live_buffers == before.live_buffers,
              "4: no live buffers left after the executed task (RAII intact)");

        runtime.shutdown();
    }

    if (failures == 0) {
        std::cout << "Resource monitoring GPU-path tests passed.\n";
        return 0;
    }
    std::cerr << failures << " monitoring GPU-path check(s) FAILED.\n";
    return 1;
}
