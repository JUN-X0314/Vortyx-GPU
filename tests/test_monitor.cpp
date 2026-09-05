// Resource Monitoring tests (Phase 8) — CPU path.
//
// These tests MUST pass on every system, including machines without any GPU
// and CPU-only builds. They verify the Phase 8 monitor's INVARIANTS:
//
//   system-only snapshot honesty -> full snapshot consistency with the
//   Runtime's own answers (backend availability, unavailable reasons,
//   DeviceInfo, ResourceManager accounting) -> value semantics (a snapshot
//   stays valid when the system changes afterwards) -> repeated snapshots
//   do not corrupt anything -> the Scheduler's behavior is unchanged by
//   monitoring -> read-only concurrent snapshots -> the honest
//   unavailable-representation rules (no fake zeros).
//
// Vulkan-dependent expectations are ADAPTIVE: they assert that the snapshot
// matches whatever the Runtime really reports (available or not, hardware
// or software device) — never a specific hardware configuration. The
// dedicated GPU-path test (test_monitor_gpu.cpp) pins the real-device case.

#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "core/compute/task.hpp"
#include "core/compute/runtime.hpp"
#include "core/device/device.hpp"
#include "core/monitor/monitor.hpp"
#include "core/resource/resource.hpp"
#include "core/scheduler/scheduler.hpp"

using vortyx::compute::Runtime;
using vortyx::compute::Status;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;
using vortyx::monitor::BackendObservation;
using vortyx::monitor::ResourceMonitor;
using vortyx::monitor::ResourceSnapshot;
using vortyx::monitor::describe;
using vortyx::monitor::to_key_values;

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

}  // namespace

int main() {
    // =====================================================================
    // 1. System-only snapshot: honest, sane, explicitly unobserved
    //    Vortyx sections (never fake values for an absent source).
    // =====================================================================
    {
        ResourceMonitor monitor;
        const ResourceSnapshot snap = monitor.snapshot();

        if (snap.hardware_threads.has_value()) {
            check(*snap.hardware_threads > 0, "1a: known hardware thread count is positive");
        }
        // hardware_threads == nullopt is the documented honest "unknown";
        // a fake count is the only failure mode here.

        check(!snap.runtime_observed, "1b: no Runtime was observed");
        check(snap.backends.empty(), "1b: no backend observations without a Runtime");
        check(!snap.resource_stats_valid, "1b: resource stats marked invalid without a source");
        check(snap.available_backend_count() == 0, "1b: zero available backends reported");

        const std::string text = describe(snap);
        check(text.find("not observed") != std::string::npos,
              "1c: description states the runtime was not observed");

        const auto kv = to_key_values(snap);
        bool runtime_observed_false = false;
        for (const auto& pair : kv) {
            if (pair.first == "runtime_observed" && pair.second == "false")
                runtime_observed_false = true;
        }
        check(runtime_observed_false, "1d: export marks the runtime unobserved");
    }

    // =====================================================================
    // 2. Full snapshot against a live Runtime: every value must equal the
    //    Runtime's own answer (single source of truth, re-queried).
    // =====================================================================
    {
        Runtime runtime;
        check(runtime.initialize() == Status::Ok, "2: Runtime initializes");

        ResourceMonitor monitor;
        const ResourceSnapshot snap = monitor.snapshot(runtime);

        check(snap.runtime_observed, "2a: runtime observed");
        check(snap.resource_stats_valid, "2a: resource stats valid");

        const std::vector<std::string> names = runtime.backend_names();
        check(snap.backends.size() == names.size(),
              "2b: one observation per registered backend");

        for (const std::string& name : names) {
            const BackendObservation* obs = snap.find_backend(name);
            check(obs != nullptr, "2b: backend '" + name + "' present in the snapshot");
            if (obs == nullptr) continue;

            // Availability and reason must mirror the Runtime EXACTLY.
            check(obs->available == runtime.has_backend(name),
                  "2b: availability of '" + name + "' matches the Runtime");
            check(obs->unavailable_reason == runtime.backend_unavailable_reason(name),
                  "2b: unavailable reason of '" + name + "' matches the Runtime");
            if (!obs->available) {
                check(!obs->unavailable_reason.empty(),
                      "2b: unavailable backend '" + name + "' carries its real reason");
            }

            // Device info must be the Runtime's own DeviceInfo verbatim.
            check(devices_equal(obs->device, runtime.backend_device(name)),
                  "2c: device info of '" + name + "' matches the Runtime");

            // An available CPU backend reports a CPU device; unknown fields
            // stay optional (never 0-filled).
            if (name == "cpu" && obs->available) {
                check(obs->device.type == vortyx::device::DeviceType::Cpu,
                      "2c: cpu backend reports a Cpu device");
            }
        }

        // Resource accounting must match the manager's own stats.
        const vortyx::resource::ResourceStats live = runtime.resources().stats();
        check(snap.live_buffers == live.live_buffers, "2d: live buffers match the manager");
        check(snap.live_bytes == live.live_bytes, "2d: live bytes match the manager");
        check(snap.total_allocations == live.total_allocations,
              "2d: total allocations match the manager");

        runtime.shutdown();

        // After a Runtime shutdown the snapshot reflects the honest state:
        // the source is gone (uninitialized), so nothing Vortyx-specific is
        // observed — no stale pretending.
        const ResourceSnapshot after = monitor.snapshot(runtime);
        check(!after.runtime_observed, "2e: shutdown Runtime is reported unobserved");
        check(!after.resource_stats_valid, "2e: no resource stats after shutdown");
    }

    // =====================================================================
    // 3. Value semantics + live tracking: snapshots are independent copies
    //    and the accounting tracks real create/release exactly.
    // =====================================================================
    {
        Runtime runtime;
        check(runtime.initialize() == Status::Ok, "3: Runtime initializes");
        ResourceMonitor monitor;

        const ResourceSnapshot before = monitor.snapshot(runtime);
        check(before.live_buffers == 0, "3a: no live buffers initially");

        vortyx::resource::BufferDesc desc;
        desc.element_count = 64;
        desc.element_size = sizeof(std::int32_t);
        desc.access = vortyx::resource::ResourceAccess::Read | vortyx::resource::ResourceAccess::Write;
        vortyx::resource::BufferResult buffer = runtime.resources().create_buffer(desc, "cpu");
        check(buffer.status == Status::Ok, "3a: buffer created");

        const ResourceSnapshot during = monitor.snapshot(runtime);
        check(during.live_buffers == 1, "3a: one live buffer observed");
        check(during.live_bytes == 64 * sizeof(std::int32_t), "3a: live bytes match the buffer");
        check(during.total_allocations == before.total_allocations + 1,
              "3a: allocation counted exactly once");

        // The EARLIER snapshot still holds its own values (value copy, no
        // shared mutable state, no dangling).
        check(before.live_buffers == 0, "3b: earlier snapshot unchanged by system mutation");
        check(before.total_allocations + 1 == during.total_allocations,
              "3b: snapshots are independent observations");

        buffer.buffer.reset();
        const ResourceSnapshot after = monitor.snapshot(runtime);
        check(after.live_buffers == 0, "3c: release observed");
        check(after.live_bytes == 0, "3c: live bytes back to zero");
        check(after.total_allocations == during.total_allocations,
              "3c: total allocations never decrease");
        check(before.live_buffers == 0 && during.live_buffers == 1,
              "3c: kept snapshots still valid after further system changes");

        runtime.shutdown();
    }

    // =====================================================================
    // 4. Repeated snapshots: deterministic over an unchanged system, and
    //    they never corrupt the observed sources.
    // =====================================================================
    {
        Runtime runtime;
        check(runtime.initialize() == Status::Ok, "4: Runtime initializes");
        ResourceMonitor monitor;

        const ResourceSnapshot first = monitor.snapshot(runtime);
        const ResourceSnapshot second = monitor.snapshot(runtime);
        const ResourceSnapshot third = monitor.snapshot(runtime);

        check(first.runtime_observed == second.runtime_observed &&
                  second.runtime_observed == third.runtime_observed,
              "4a: repeated snapshots agree on observation state");
        check(first.backends.size() == third.backends.size(),
              "4a: repeated snapshots see the same backends");
        for (std::size_t i = 0; i < first.backends.size(); ++i) {
            check(first.backends[i].available == third.backends[i].available &&
                      first.backends[i].name == third.backends[i].name,
                  "4a: repeated snapshots agree per backend");
        }
        check(first.live_buffers == third.live_buffers &&
                  first.total_allocations == third.total_allocations,
              "4a: unchanged system -> unchanged accounting");

        // The Runtime still works after being observed repeatedly.
        VectorAddTask task;
        task.a = {1, 2, 3, 4};
        task.b = {10, 20, 30, 40};
        const VectorAddResult result = runtime.execute(task);
        check(result.status == Status::Ok && result.data.size() == 4,
              "4b: Runtime still executes correctly after repeated snapshots");

        runtime.shutdown();
    }

    // =====================================================================
    // 5. Scheduler independence: monitoring neither changes nor disturbs
    //    the Phase 7 selection policy.
    // =====================================================================
    {
        vortyx::scheduler::Scheduler scheduler;
        check(scheduler.initialize() == Status::Ok, "5: Scheduler initializes");

        const vortyx::scheduler::SelectionResult before =
            scheduler.select(vortyx::scheduler::SelectionRequest{});
        check(before.status == Status::Ok, "5a: baseline selection succeeds");

        // Observe: several snapshots against an independent Runtime.
        Runtime runtime;
        check(runtime.initialize() == Status::Ok, "5a: independent Runtime initializes");
        ResourceMonitor monitor;
        const ResourceSnapshot snap = monitor.snapshot(runtime);
        check(snap.runtime_observed, "5a: snapshot taken between selections");

        const vortyx::scheduler::SelectionResult after =
            scheduler.select(vortyx::scheduler::SelectionRequest{});

        // The policy is a pure function of probed availability: unchanged
        // system -> identical decision, with monitoring in between or not.
        check(after.status == before.status, "5b: selection status unchanged by monitoring");
        check(after.backend == before.backend, "5b: selection result unchanged by monitoring");
        check(after.reason == before.reason, "5b: selection reason unchanged by monitoring");

        // An explicit 'cpu' request still resolves exactly as in Phase 7.
        vortyx::scheduler::SelectionRequest cpu_request;
        cpu_request.mode = vortyx::scheduler::SelectionMode::ExplicitBackend;
        cpu_request.backend = "cpu";
        const vortyx::scheduler::SelectionResult cpu_sel = scheduler.select(cpu_request);
        check(cpu_sel.status == Status::Ok && cpu_sel.backend == "cpu",
              "5c: explicit cpu selection still honored");

        runtime.shutdown();
        scheduler.shutdown();
    }

    // =====================================================================
    // 6. Read-only concurrent snapshots: the stateless monitor serves
    //    several threads at once against an UNCHANGED Runtime (pure reads;
    //    the Runtime's own external-serialization rule applies only to
    //    mutators, and there are none here).
    // =====================================================================
    {
        Runtime runtime;
        check(runtime.initialize() == Status::Ok, "6: Runtime initializes");
        ResourceMonitor monitor;

        const ResourceSnapshot reference = monitor.snapshot(runtime);
        std::vector<ResourceSnapshot> results(4);
        // Plain ints, not vector<bool>: vector<bool> packs bits into shared
        // words, which would be a data race between the worker threads.
        std::vector<int> ok(4, 0);

        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&monitor, &runtime, &results, &ok, t] {
                results[static_cast<std::size_t>(t)] = monitor.snapshot(runtime);
                ok[static_cast<std::size_t>(t)] = 1;
            });
        }
        for (std::thread& thread : threads) thread.join();

        for (int t = 0; t < 4; ++t) {
            const std::string ctx = "6: thread " + std::to_string(t);
            check(ok[static_cast<std::size_t>(t)] == 1, ctx + " snapshot completed");
            check(results[static_cast<std::size_t>(t)].runtime_observed == reference.runtime_observed,
                  ctx + " agrees with the reference");
            check(results[static_cast<std::size_t>(t)].live_buffers == reference.live_buffers,
                  ctx + " observes the same accounting");
            check(results[static_cast<std::size_t>(t)].backends.size() == reference.backends.size(),
                  ctx + " observes the same backends");
        }

        runtime.shutdown();
    }

    // =====================================================================
    // 7. Honest unavailable representation: unknown things are optional /
    //    flagged, never fake zeros; unsupported metrics have no fields.
    // =====================================================================
    {
        // Structural honesty: the snapshot type exposes utilization/
        // temperature/power/VRAM-usage/fan metrics NOWHERE — their absence
        // is the representation. (Verified indirectly here by the absence
        // of any such value in the export schema.)
        Runtime runtime;
        check(runtime.initialize() == Status::Ok, "7: Runtime initializes");
        ResourceMonitor monitor;
        const ResourceSnapshot snap = monitor.snapshot(runtime);

        const auto kv = to_key_values(snap);
        for (const auto& pair : kv) {
            check(pair.first.find("utilization") == std::string::npos &&
                      pair.first.find("temperature") == std::string::npos &&
                      pair.first.find("power") == std::string::npos &&
                      pair.first.find("fan") == std::string::npos &&
                      pair.first.find("vram_usage") == std::string::npos,
                  "7: no fabricated metric key in export (" + pair.first + ")");
        }

        // A registered-but-unavailable backend appears with its real reason
        // (adaptive: on this machine that is whatever the Runtime says).
        for (const BackendObservation& obs : snap.backends) {
            if (!obs.available) {
                check(!obs.unavailable_reason.empty(),
                      "7: unavailable '" + obs.name + "' explains itself");
            }
        }

        runtime.shutdown();
    }

    if (failures == 0) {
        std::cout << "Resource monitoring CPU-path tests passed.\n";
        return 0;
    }
    std::cerr << failures << " monitoring CPU-path check(s) FAILED.\n";
    return 1;
}
