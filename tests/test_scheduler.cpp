// Basic Scheduler tests (Phase 7) — CPU path.
//
// These tests MUST pass on every system, including machines without any GPU
// and CPU-only builds. They verify:
//   lifecycle -> request validation -> the pure policy (synthetic candidates,
//   no hardware involved) -> real selections against the probed system
//   (adaptive: no assumptions about whether a Vulkan device exists) ->
//   determinism -> shutdown/re-initialization -> TaskQueue integration ->
//   concurrent select() calls.
//
// Design rules honored here:
//   - No timing-based flakiness. Concurrency is verified with joins and
//     result consistency, never with sleeps.
//   - No hardware assumptions: every Vulkan-dependent expectation adapts to
//     the real availability probed through an independent Virtual GPU.
//   - Failures are never faked: an unavailable backend must produce an
//     honest failing selection, never a remapped "success".

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>
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
using vortyx::scheduler::basic_scheduler_select;
using vortyx::scheduler::PolicyCandidate;
using vortyx::scheduler::Scheduler;
using vortyx::scheduler::SelectionMode;
using vortyx::scheduler::SelectionRequest;
using vortyx::scheduler::SelectionResult;
using vortyx::scheduler::State;
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

// The Scheduler owns a probe Runtime and follows the same one-address-for-life
// rule as the TaskQueue. Verified at compile time.
static_assert(!std::is_copy_constructible<Scheduler>::value,
              "Scheduler must not be copy-constructible");
static_assert(!std::is_copy_assignable<Scheduler>::value,
              "Scheduler must not be copy-assignable");
static_assert(!std::is_move_constructible<Scheduler>::value,
              "Scheduler must not be move-constructible");
static_assert(!std::is_move_assignable<Scheduler>::value,
              "Scheduler must not be move-assignable");

// The Scheduler exposes NO execute/task API at all: selection is its entire
// surface. (Structural check of the "selects, never computes" boundary.)
template <typename T, typename = void>
struct has_execute_method : std::false_type {};
template <typename T>
struct has_execute_method<T, std::void_t<decltype(std::declval<T&>().execute(
                                 std::declval<const VectorAddTask&>()))>> : std::true_type {
};
static_assert(!has_execute_method<Scheduler>::value,
              "Scheduler must not have an execute() method (it never computes)");

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

bool device_fields_equal(const vortyx::device::DeviceInfo& a,
                         const vortyx::device::DeviceInfo& b) {
    return a.type == b.type && a.name == b.name && a.vendor == b.vendor && a.id == b.id;
}

std::string describe_selected(const SelectionResult& r) {
    return "backend='" + r.backend + "' device.type=" +
           std::string([](vortyx::device::DeviceType t) {
               switch (t) {
                   case vortyx::device::DeviceType::Unknown: return "Unknown";
                   case vortyx::device::DeviceType::Cpu: return "Cpu";
                   case vortyx::device::DeviceType::Gpu: return "Gpu";
                   case vortyx::device::DeviceType::SoftwareGpu: return "SoftwareGpu";
               }
               return "?";
           }(r.device.type)) +
           " device.name='" + r.device.name + "'";
}

}  // namespace

int main() {
    // The real availability of the Vulkan backend on THIS machine, probed
    // through an independent Virtual GPU (not through the Scheduler under
    // test), so the adaptive expectations below have an honest source.
    bool vulkan_available = false;
    std::string vulkan_unavailable_reason;
    {
        VirtualGpu probe;
        VirtualGpuDesc desc;
        desc.backend = "vulkan";
        if (probe.initialize(desc) != Status::Ok) {
            std::cerr << "FAIL: vulkan VirtualGpu probe initialize() must not fail for a "
                         "known backend\n";
            return 1;
        }
        vulkan_available = probe.backend_available();
        if (!vulkan_available) {
            vulkan_unavailable_reason = probe.backend_unavailable_reason();
        }
        probe.shutdown();
    }

    // =====================================================================
    // 1. Fresh object: Uninitialized, refusals, no-op shutdown.
    // =====================================================================
    {
        Scheduler scheduler;
        check(scheduler.state() == State::Uninitialized, "fresh Scheduler must be Uninitialized");
        check(!scheduler.is_ready(), "fresh Scheduler must not be ready");

        const SelectionResult refused = scheduler.select(SelectionRequest{});
        check(refused.status == Status::NotInitialized,
              "select() before initialize() must fail with NotInitialized");
        check(!refused.error.empty(), "refused select() must explain why");
        check(refused.backend.empty(), "a failed selection must not name a backend");

        SelectionRequest explicit_req;
        explicit_req.mode = SelectionMode::ExplicitBackend;
        explicit_req.backend = "cpu";
        check(scheduler.select(explicit_req).status == Status::NotInitialized,
              "explicit select() before initialize() must fail with NotInitialized");

        scheduler.shutdown();  // no-op on Uninitialized: must be safe
        check(scheduler.state() == State::ShutDown,
              "shutdown() on an Uninitialized Scheduler lands on ShutDown");
    }

    // =====================================================================
    // 2. initialize(): Ready, idempotent re-init, documented priority.
    // =====================================================================
    {
        Scheduler scheduler;
        check(scheduler.initialize() == Status::Ok, "initialize() must succeed");
        check(scheduler.state() == State::Ready, "initialized Scheduler must be Ready");
        check(scheduler.is_ready(), "is_ready() must agree with state()");

        // Idempotent re-initialization (no configuration to change).
        check(scheduler.initialize() == Status::Ok,
              "re-initializing while Ready must stay Ok (idempotent)");
        check(scheduler.state() == State::Ready, "state must remain Ready after idempotent init");

        // The documented automatic priority order, pinned so it cannot drift
        // silently.
        const std::vector<std::string>& priority = Scheduler::automatic_priority();
        check(priority.size() == 2, "automatic priority must contain exactly two backends");
        check(priority.size() >= 2 && priority[0] == "vulkan" && priority[1] == "cpu",
              "automatic priority must be the documented order: vulkan > cpu");
    }

    // =====================================================================
    // 3. The pure policy over synthetic candidates (no hardware involved).
    //    Pins the documented Basic rules exactly.
    // =====================================================================
    {
        std::size_t index = 0;
        std::string reason;
        std::string error;

        // 3a. Explicit request for an available backend succeeds.
        std::vector<PolicyCandidate> both = {
            {"vulkan", true, std::string{}},
            {"cpu", true, std::string{}},
        };
        check(basic_scheduler_select(both, SelectionMode::ExplicitBackend, "cpu", index,
                                     reason, error) == Status::Ok &&
                  index == 1,
              "explicit 'cpu' (available) must select the cpu candidate");
        check(basic_scheduler_select(both, SelectionMode::ExplicitBackend, "vulkan", index,
                                     reason, error) == Status::Ok &&
                  index == 0,
              "explicit 'vulkan' (available) must select the vulkan candidate");
        check(reason.find("explicit request honored") != std::string::npos,
              "explicit success must be explainable");

        // 3b. Explicit request for a registered-but-unavailable backend FAILS
        //     with the real reason and is NEVER remapped to the working one.
        std::vector<PolicyCandidate> vulkan_down = {
            {"vulkan", false, "no Vulkan device on this system"},
            {"cpu", true, std::string{}},
        };
        const Status down_status = basic_scheduler_select(
            vulkan_down, SelectionMode::ExplicitBackend, "vulkan", index, reason, error);
        check(down_status == Status::BackendUnavailable,
              "explicit unavailable backend must fail with BackendUnavailable");
        check(error.find("no Vulkan device on this system") != std::string::npos,
              "explicit failure must carry the backend's real reason");
        check(reason == error, "policy reason must mirror the error on failure");

        // 3c. Explicit request for an unknown backend fails listing names.
        const Status unknown_status = basic_scheduler_select(
            both, SelectionMode::ExplicitBackend, "cuda", index, reason, error);
        check(unknown_status == Status::BackendUnavailable,
              "unknown backend must fail with BackendUnavailable");
        check(error.find("unknown backend 'cuda'") != std::string::npos &&
                  error.find("vulkan") != std::string::npos && error.find("cpu") != std::string::npos,
              "unknown-backend error must list the registered backends");

        // 3d. Empty explicit name is a caller bug: InvalidInput.
        check(basic_scheduler_select(both, SelectionMode::ExplicitBackend, std::string{},
                                     index, reason, error) == Status::InvalidInput,
              "empty explicit backend name must fail with InvalidInput");

        // 3e. Automatic policy: BOTH available -> the FIRST candidate in the
        //     given priority order wins (documented: vulkan > cpu).
        check(basic_scheduler_select(both, SelectionMode::Automatic, std::string{}, index,
                                     reason, error) == Status::Ok &&
                  index == 0,
              "automatic with both available must select the first priority candidate");
        check(reason.find("automatic policy") != std::string::npos &&
                  reason.find("vulkan") != std::string::npos,
              "automatic success must be explainable and name the choice");

        // The same auto request over a REVERSED candidate order picks the
        // first of THAT order: the policy honors the priority it is given,
        // it does not hardcode names.
        std::vector<PolicyCandidate> reversed = {
            {"cpu", true, std::string{}},
            {"vulkan", true, std::string{}},
        };
        check(basic_scheduler_select(reversed, SelectionMode::Automatic, std::string{}, index,
                                     reason, error) == Status::Ok &&
                  index == 0,
              "automatic policy must select the first available candidate in the given order");

        // 3f. Automatic policy: vulkan unavailable -> cpu (documented
        //     fallback, with the real reason in the explanation).
        check(basic_scheduler_select(vulkan_down, SelectionMode::Automatic, std::string{},
                                     index, reason, error) == Status::Ok &&
                  index == 1,
              "automatic with vulkan unavailable must select cpu");
        check(reason.find("no Vulkan device on this system") != std::string::npos,
              "automatic fallback reason must include the skipped backend's real reason");

        // 3g. Automatic policy: nothing available -> honest failure.
        std::vector<PolicyCandidate> all_down = {
            {"vulkan", false, "no device"},
            {"cpu", false, "theoretical: cpu is always available"},
        };
        check(basic_scheduler_select(all_down, SelectionMode::Automatic, std::string{}, index,
                                     reason, error) == Status::BackendUnavailable,
              "automatic with no available candidate must fail honestly");

        // 3h. Automatic policy: empty candidate list -> honest failure.
        std::vector<PolicyCandidate> none;
        check(basic_scheduler_select(none, SelectionMode::Automatic, std::string{}, index,
                                     reason, error) == Status::BackendUnavailable,
              "automatic with no candidates must fail honestly");

        // 3i. Determinism: identical inputs -> identical decisions.
        std::size_t index2 = 0;
        std::string reason2;
        std::string error2;
        basic_scheduler_select(vulkan_down, SelectionMode::Automatic, std::string{}, index,
                               reason, error);
        basic_scheduler_select(vulkan_down, SelectionMode::Automatic, std::string{}, index2,
                               reason2, error2);
        check(index == index2 && reason == reason2 && error == error2,
              "the policy must be deterministic for identical inputs");
    }

    // =====================================================================
    // 4. Real selections through a Ready Scheduler (adaptive, honest).
    // =====================================================================
    {
        Scheduler scheduler;
        check(scheduler.initialize() == Status::Ok, "initialize() for real selections");

        // 4a. Automatic: the decision must match the independently probed
        //     reality and be deterministic across repeats.
        SelectionResult auto1 = scheduler.select(SelectionRequest{});
        check(auto1.status == Status::Ok, "automatic selection must succeed on a Ready Scheduler");
        const std::string expected_backend = vulkan_available ? "vulkan" : "cpu";
        check(auto1.backend == expected_backend,
              "automatic selection must be '" + expected_backend +
                  "' on this system (got: '" + auto1.backend + "')");
        check(!auto1.reason.empty(), "automatic selection must carry its reason");
        check(!auto1.device.name.empty() || auto1.device.type != vortyx::device::DeviceType::Unknown,
              "a successful selection must identify a concrete device: " +
                  describe_selected(auto1));
        if (auto1.backend == "cpu") {
            check(auto1.device.type == vortyx::device::DeviceType::Cpu,
                  "a cpu selection must report a Cpu device");
        } else {
            check(auto1.device.type == vortyx::device::DeviceType::Gpu ||
                      auto1.device.type == vortyx::device::DeviceType::SoftwareGpu,
                  "a vulkan selection must report a Gpu or SoftwareGpu device");
        }

        SelectionResult auto2 = scheduler.select(SelectionRequest{});
        SelectionResult auto3 = scheduler.select(SelectionRequest{});
        check(auto2.backend == auto1.backend && auto3.backend == auto1.backend &&
                  auto2.reason == auto1.reason && auto3.reason == auto1.reason &&
                  device_fields_equal(auto2.device, auto1.device),
              "repeated automatic selections on the same system must be identical");

        // 4b. Explicit "cpu": always succeeds, never remapped.
        SelectionRequest cpu_req;
        cpu_req.mode = SelectionMode::ExplicitBackend;
        cpu_req.backend = "cpu";
        SelectionResult cpu_sel = scheduler.select(cpu_req);
        check(cpu_sel.status == Status::Ok, "explicit 'cpu' must succeed everywhere");
        check(cpu_sel.backend == "cpu", "explicit 'cpu' must select exactly 'cpu'");
        check(cpu_sel.device.type == vortyx::device::DeviceType::Cpu,
              "explicit 'cpu' selection must report a Cpu device");

        // 4c. Explicit "vulkan": adaptive — succeeds only when really usable;
        //     otherwise an honest failure that NEVER names cpu as chosen.
        SelectionRequest vk_req;
        vk_req.mode = SelectionMode::ExplicitBackend;
        vk_req.backend = "vulkan";
        SelectionResult vk_sel = scheduler.select(vk_req);
        if (vulkan_available) {
            check(vk_sel.status == Status::Ok,
                  "explicit 'vulkan' must succeed when the probe says it is available");
            check(vk_sel.backend == "vulkan", "explicit 'vulkan' must select exactly 'vulkan'");
        } else {
            check(vk_sel.status == Status::BackendUnavailable,
                  "explicit 'vulkan' without a device must fail with BackendUnavailable");
            check(vk_sel.error.find(vulkan_unavailable_reason) != std::string::npos,
                  "the failure must carry the backend's real unavailable reason");
            check(vk_sel.backend.empty(),
                  "a failed explicit 'vulkan' selection must NOT name a chosen backend "
                  "(no silent fallback to cpu)");
        }

        // 4d. Unknown and malformed requests are refused clearly.
        SelectionRequest cuda_req;
        cuda_req.mode = SelectionMode::ExplicitBackend;
        cuda_req.backend = "cuda";
        const SelectionResult cuda_sel = scheduler.select(cuda_req);
        check(cuda_sel.status == Status::BackendUnavailable &&
                  cuda_sel.error.find("unknown backend 'cuda'") != std::string::npos,
              "an unknown backend request must fail listing the registered names");

        SelectionRequest empty_req;
        empty_req.mode = SelectionMode::ExplicitBackend;
        empty_req.backend = "";
        check(scheduler.select(empty_req).status == Status::InvalidInput,
              "an explicit request with an empty name must fail with InvalidInput");

        SelectionRequest conflict_req;  // Automatic mode must not name a backend
        conflict_req.mode = SelectionMode::Automatic;
        conflict_req.backend = "cpu";
        check(scheduler.select(conflict_req).status == Status::InvalidInput,
              "Automatic mode with a named backend must fail with InvalidInput");

        // 4e. The selection matches what an executing Virtual GPU would see:
        //     build a Virtual GPU FROM the selection and compare devices.
        {
            VirtualGpuDesc desc;
            desc.backend = auto1.backend;
            VirtualGpu gpu;
            check(gpu.initialize(desc) == Status::Ok,
                  "a Virtual GPU created from the selected backend must initialize");
            check(gpu.backend_name() == auto1.backend,
                  "the Virtual GPU must run on exactly the selected backend");
            if (auto1.backend == "vulkan") {
                check(gpu.backend_available(),
                      "a Scheduler-selected vulkan Virtual GPU must be available when the "
                      "selection succeeded");
            }
            check(device_fields_equal(gpu.device_info(), auto1.device) ||
                      (gpu.device_info().type == auto1.device.type &&
                       gpu.device_info().name == auto1.device.name),
                  "the executing device must match the selection's reported device");
            gpu.shutdown();
        }

        scheduler.shutdown();
    }

    // =====================================================================
    // 5. Shutdown lifecycle: refusals after shutdown, re-initialization,
    //    double shutdown, destructor safety.
    // =====================================================================
    {
        Scheduler scheduler;
        check(scheduler.initialize() == Status::Ok, "lifecycle: initialize");
        scheduler.shutdown();
        check(scheduler.state() == State::ShutDown, "lifecycle: shutdown lands on ShutDown");

        const SelectionResult after = scheduler.select(SelectionRequest{});
        check(after.status == Status::NotInitialized,
              "select() after shutdown() must fail with NotInitialized");

        scheduler.shutdown();  // double shutdown: safe
        check(scheduler.state() == State::ShutDown, "double shutdown must stay ShutDown");

        check(scheduler.initialize() == Status::Ok, "re-initialization after shutdown");
        check(scheduler.state() == State::Ready, "re-initialized Scheduler must be Ready");
        check(scheduler.select(SelectionRequest{}).status == Status::Ok,
              "select() must work again after re-initialization");

        for (int cycle = 0; cycle < 3; ++cycle) {
            scheduler.shutdown();
            check(scheduler.initialize() == Status::Ok,
                  "cycle " + std::to_string(cycle) + " re-initialize");
        }
        // Destructor runs shutdown() again: must be safe (no crash, no leak).
    }

    // =====================================================================
    // 6. TaskQueue integration: the selection feeds the UNCHANGED Phase 5/6
    //    path, and the actual computation happens there — not in the
    //    Scheduler.
    // =====================================================================
    {
        Scheduler scheduler;
        check(scheduler.initialize() == Status::Ok, "integration: scheduler initialize");

        const SelectionResult selection = scheduler.select(SelectionRequest{});
        check(selection.status == Status::Ok, "integration: automatic selection");

        VirtualGpu gpu;
        VirtualGpuDesc desc;
        desc.backend = selection.backend;  // the Scheduler's choice, verbatim
        check(gpu.initialize(desc) == Status::Ok, "integration: Virtual GPU from selection");
        check(gpu.backend_name() == selection.backend,
              "integration: Virtual GPU runs on the selected backend");

        TaskQueue queue;
        check(queue.initialize(gpu) == Status::Ok, "integration: TaskQueue on selected Virtual GPU");

        const VectorAddTask task = make_task(64);
        const EnqueueResult enq = queue.enqueue(task);
        check(enq.status == Status::Ok, "integration: enqueue");
        check(queue.wait(enq.id) == TaskState::Completed, "integration: task must complete");

        // Bit-exact comparison against an independently executed CPU
        // reference (the long-standing Phase 3/5 verification rule).
        VirtualGpu reference;
        check(reference.initialize() == Status::Ok, "integration: reference Virtual GPU");
        const VectorAddResult ref_result = reference.execute(task);
        check(ref_result.status == Status::Ok, "integration: reference execution");

        const auto snap = queue.task_snapshot(enq.id);
        check(snap.result.status == Status::Ok && result_matches(snap.result, task),
              "integration: queued result must equal A+B on the selected backend");
        check(snap.result.data == ref_result.data,
              "integration: selected-backend result must match the cpu reference bit-exactly");

        reference.shutdown();
        queue.shutdown();  // documented order: queue first ...
        gpu.shutdown();    // ... then its Virtual GPU
        scheduler.shutdown();  // the Scheduler shares nothing with them: any order is safe
    }

    // =====================================================================
    // 7. Concurrent select() (API contract: allowed) + concurrent shutdown.
    // =====================================================================
    {
        Scheduler scheduler;
        check(scheduler.initialize() == Status::Ok, "concurrency: initialize");

        SelectionRequest cpu_req;
        cpu_req.mode = SelectionMode::ExplicitBackend;
        cpu_req.backend = "cpu";

        std::vector<std::string> auto_backends(4 * 25);
        std::vector<Status> cpu_statuses(4 * 25, Status::Ok);
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([t, &scheduler, &auto_backends, &cpu_statuses, &cpu_req]() {
                for (int i = 0; i < 25; ++i) {
                    const SelectionResult r = scheduler.select(SelectionRequest{});
                    auto_backends[static_cast<std::size_t>(t) * 25 +
                                  static_cast<std::size_t>(i)] = r.backend;
                    const SelectionResult c = scheduler.select(cpu_req);
                    cpu_statuses[static_cast<std::size_t>(t) * 25 +
                                 static_cast<std::size_t>(i)] = c.status;
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }

        const bool consistent_auto =
            std::all_of(auto_backends.begin(), auto_backends.end(),
                        [&auto_backends](const std::string& b) { return b == auto_backends[0] && !b.empty(); });
        check(consistent_auto,
              "concurrent automatic selections must all return the same backend");
        const bool all_cpu_ok =
            std::all_of(cpu_statuses.begin(), cpu_statuses.end(),
                        [](Status s) { return s == Status::Ok; });
        check(all_cpu_ok, "concurrent explicit 'cpu' selections must all succeed");

        scheduler.shutdown();
        check(scheduler.state() == State::ShutDown, "concurrency: shutdown after joins");
    }

    if (failures == 0) {
        std::cout << "Basic Scheduler CPU tests passed.\n";
        return 0;
    }
    return 1;
}
