// Virtual GPU Interface tests (Phase 5) — CPU path.
//
// These tests MUST pass on every system, including machines without any GPU
// and CPU-only builds. They verify the Virtual GPU lifecycle against the
// always-available CPU backend:
//   creation -> uninitialized errors -> initialization -> task execution ->
//   result verification -> resource-based execution -> shutdown/re-init ->
//   move semantics -> honest backend unavailability reporting.
//
// Honesty rules enforced here:
//   - execute() before initialize() / after shutdown() fails clearly.
//   - An unknown backend name fails at initialize(), never silently later.
//   - A known-but-unavailable backend NEVER falls back silently: execute()
//     reports Status::BackendUnavailable with the real reason.
//   - No test requires a specific GPU, vendor or device name.

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/compute/task.hpp"
#include "core/device/device.hpp"
#include "core/resource/buffer.hpp"
#include "core/resource/resource.hpp"
#include "core/resource/resource_manager.hpp"
#include "core/vgpu/virtual_gpu.hpp"

using vortyx::compute::ComputeResult;
using vortyx::compute::Status;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;
using vortyx::vgpu::State;
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

// Virtual GPUs own a live execution context (Runtime + backends). Like the
// resource handles below them, they must never be copyable. Verified at
// compile time.
static_assert(!std::is_copy_constructible<VirtualGpu>::value,
              "VirtualGpu must not be copy-constructible");
static_assert(!std::is_copy_assignable<VirtualGpu>::value,
              "VirtualGpu must not be copy-assignable");
static_assert(std::is_move_constructible<VirtualGpu>::value,
              "VirtualGpu must be move-constructible");
static_assert(std::is_move_assignable<VirtualGpu>::value,
              "VirtualGpu must be move-assignable");

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
    // --- 1. Fresh object: Uninitialized and honestly unusable ---------------
    {
        VirtualGpu gpu;
        check(gpu.state() == State::Uninitialized, "a fresh VirtualGpu must be Uninitialized");
        check(!gpu.is_ready(), "a fresh VirtualGpu must not be ready");
        check(std::string(vortyx::vgpu::to_string(gpu.state())) == "Uninitialized",
              "to_string(State) must report Uninitialized");
        check(gpu.backend_name() == "cpu", "default description must select the cpu backend");
        check(!gpu.backend_available(), "an uninitialized VirtualGpu must not claim availability");
        check(!gpu.backend_unavailable_reason().empty(),
              "an uninitialized VirtualGpu must explain why it has no available backend");
        check(gpu.resources() == nullptr,
              "an uninitialized VirtualGpu must not expose a Resource Manager");
        check(gpu.device_info().type == vortyx::device::DeviceType::Unknown,
              "an uninitialized VirtualGpu must not fabricate device info");

        const VectorAddResult early = gpu.execute(make_task(4));
        check(early.status == Status::NotInitialized,
              "execute() before initialize() must fail with NotInitialized");
        check(!early.error.empty(), "the NotInitialized result must carry a reason");

        VectorAddTask a4 = make_task(4);
        VectorAddTask b4 = make_task(4);
        vortyx::resource::Buffer dummy_a, dummy_b, dummy_c;
        const ComputeResult early_buffers = gpu.execute(dummy_a, dummy_b, dummy_c);
        check(early_buffers.status == Status::NotInitialized,
              "buffer execute() before initialize() must fail with NotInitialized");
        (void)a4;
        (void)b4;

        gpu.shutdown();  // must be safe on a fresh object
        check(gpu.state() == State::ShutDown, "shutdown() on a fresh object must end in ShutDown");
    }

    // --- 2. CPU Virtual GPU: initialization and honest state -----------------
    VirtualGpu cpu_gpu;
    {
        VirtualGpuDesc desc;  // backend defaults to "cpu"
        const Status init = cpu_gpu.initialize(desc);
        check(init == Status::Ok, "cpu VirtualGpu initialize() must succeed on every system");
        check(cpu_gpu.state() == State::Ready, "initialized VirtualGpu must be Ready");
        check(cpu_gpu.is_ready(), "is_ready() must agree with state()");
        check(cpu_gpu.backend_name() == "cpu", "backend_name() must report the configured backend");
        check(cpu_gpu.backend_available(), "the cpu backend must be available on every system");
        check(cpu_gpu.backend_unavailable_reason().empty(),
              "an available backend must have an empty unavailable reason");
        check(cpu_gpu.resources() != nullptr,
              "a Ready VirtualGpu must expose its Resource Manager");

        const vortyx::device::DeviceInfo device = cpu_gpu.device_info();
        check(device.type == vortyx::device::DeviceType::Cpu,
              "the cpu backend must report a device typed Cpu (never fabricated GPU data)");
    }

    // --- 3. Vector addition through the CPU Virtual GPU ----------------------
    {
        for (const std::size_t size : {std::size_t{4}, std::size_t{16}, std::size_t{1024},
                                       std::size_t{10007}}) {
            const VectorAddTask task = make_task(size);
            const VectorAddResult result = cpu_gpu.execute(task);
            check(result.status == Status::Ok,
                  "cpu VirtualGpu execution must succeed for size " + std::to_string(size) +
                      " (error: " + result.error + ")");
            check(result_matches(result, task),
                  "cpu VirtualGpu result must equal A+B for size " + std::to_string(size));
        }
    }

    // --- 4. Invalid tasks are rejected through the Virtual GPU ---------------
    {
        VectorAddTask mismatch;
        mismatch.a = {1, 2, 3};
        mismatch.b = {1, 2};
        const VectorAddResult r1 = cpu_gpu.execute(mismatch);
        check(r1.status == Status::InvalidInput,
              "mismatched sizes must return InvalidInput through the Virtual GPU");

        VectorAddTask empty;
        const VectorAddResult r2 = cpu_gpu.execute(empty);
        check(r2.status == Status::InvalidInput,
              "an empty task must return InvalidInput through the Virtual GPU");
    }

    // --- 5. Unknown backend: early, explicit configuration error -------------
    {
        VirtualGpu gpu;
        VirtualGpuDesc bad;
        bad.backend = "cuda";  // no such backend exists in Vortyx
        const Status init = gpu.initialize(bad);
        check(init == Status::BackendUnavailable,
              "an unknown backend name must fail initialize() with BackendUnavailable");
        check(gpu.state() == State::Uninitialized,
              "a failed configuration must leave the VirtualGpu Uninitialized (no half state)");
        check(gpu.resources() == nullptr,
              "a failed configuration must not expose a Resource Manager");
        check(gpu.execute(make_task(4)).status == Status::NotInitialized,
              "execute() must stay refused after a failed configuration");

        // Recovery on the same object with a valid description.
        const Status retry = gpu.initialize(VirtualGpuDesc{});
        check(retry == Status::Ok, "re-initialization with a valid backend must succeed");
        check(gpu.state() == State::Ready && gpu.execute(make_task(8)).status == Status::Ok,
              "the recovered VirtualGpu must be Ready and executable");
    }

    // --- 6. Idempotent initialize / refused reconfiguration ------------------
    {
        VirtualGpuDesc same;
        same.backend = "cpu";
        check(cpu_gpu.initialize(same) == Status::Ok,
              "re-initializing a Ready VirtualGpu with the same backend must be idempotent");
        check(cpu_gpu.state() == State::Ready, "idempotent initialize() must keep the state Ready");

        VirtualGpuDesc other;
        other.backend = "vulkan";
        const Status refused = cpu_gpu.initialize(other);
        check(refused == Status::InvalidInput,
              "changing the backend while Ready must be refused with InvalidInput");
        check(cpu_gpu.state() == State::Ready && cpu_gpu.backend_name() == "cpu",
              "a refused reconfiguration must leave the VirtualGpu Ready on its original backend");
    }

    // --- 7. Resource-based execution through the Virtual GPU -----------------
    {
        vortyx::resource::ResourceManager* manager = cpu_gpu.resources();
        check(manager != nullptr, "resources() must be non-null while Ready");

        const auto desc_in =
            vortyx::resource::BufferDesc::of<std::int32_t>(64, vortyx::resource::ResourceAccess::Read);
        const auto desc_out = vortyx::resource::BufferDesc::of<std::int32_t>(
            64, vortyx::resource::ResourceAccess::Write);

        vortyx::resource::BufferResult ra = manager->create_buffer(desc_in, cpu_gpu.backend_name());
        vortyx::resource::BufferResult rb = manager->create_buffer(desc_in, cpu_gpu.backend_name());
        vortyx::resource::BufferResult rc = manager->create_buffer(desc_out, cpu_gpu.backend_name());
        check(ra.status == Status::Ok && rb.status == Status::Ok && rc.status == Status::Ok,
              "buffer creation through the Virtual GPU's manager must succeed");
        check(std::string(ra.buffer.backend_name()) == "cpu",
              "buffers must report the backend they were created on");
        check(ra.buffer.memory_location() == vortyx::resource::MemoryLocation::Host,
              "cpu buffers must honestly report Host memory");

        const VectorAddTask task = make_task(64);
        const std::size_t bytes = task.a.size() * sizeof(std::int32_t);
        check(ra.buffer.write(task.a.data(), bytes).status == Status::Ok, "write A must succeed");
        check(rb.buffer.write(task.b.data(), bytes).status == Status::Ok, "write B must succeed");

        const ComputeResult exec = cpu_gpu.execute(ra.buffer, rb.buffer, rc.buffer);
        check(exec.status == Status::Ok,
              "resource-based execution through the Virtual GPU must succeed (error: " +
                  exec.error + ")");

        std::vector<std::int32_t> out(64, 0);
        check(rc.buffer.read(out.data(), bytes).status == Status::Ok, "read C must succeed");
        bool match = true;
        for (std::size_t i = 0; i < task.a.size(); ++i) {
            if (out[i] != task.a[i] + task.b[i]) match = false;
        }
        check(match, "resource-based result must equal A+B");

        // Dead handles must be rejected, never executed silently.
        vortyx::resource::Buffer moved_away = std::move(ra.buffer);
        check(!ra.buffer.valid(), "the moved-from handle must be invalid");
        const ComputeResult dead = cpu_gpu.execute(ra.buffer, rb.buffer, rc.buffer);
        check(dead.status == Status::InvalidInput,
              "a moved-from buffer must be rejected with InvalidInput");
        moved_away.reset();
        const ComputeResult released = cpu_gpu.execute(moved_away, rb.buffer, rc.buffer);
        check(released.status == Status::InvalidInput,
              "a released buffer must be rejected with InvalidInput");
    }

    // --- 7b. Buffers of ANOTHER Virtual GPU are rejected (Phase 9) -----------
    {
        // Regression test (Phase 9 stability fix): a Virtual GPU documents
        // that its execute(a, b, c) only runs "live resources of THIS Virtual
        // GPU". A valid cpu-buffer handle from a SECOND, cpu-configured
        // Virtual GPU passes the handle/backend pre-checks but must never be
        // executed here — its id lives in the OTHER Virtual GPU's manager
        // registry, and resolving it locally could silently bind the wrong
        // storage. The execution path now verifies ownership explicitly.
        vortyx::resource::Buffer survivor;
        {
            VirtualGpu other_gpu;  // cpu backend (same as cpu_gpu)
            check(other_gpu.initialize() == Status::Ok, "7b: second Virtual GPU must initialize");

            vortyx::resource::BufferResult oa = other_gpu.resources()->create_buffer(
                vortyx::resource::BufferDesc::of<std::int32_t>(
                    4, vortyx::resource::ResourceAccess::Read),
                other_gpu.backend_name());
            vortyx::resource::BufferResult ob = other_gpu.resources()->create_buffer(
                vortyx::resource::BufferDesc::of<std::int32_t>(
                    4, vortyx::resource::ResourceAccess::Read),
                other_gpu.backend_name());
            vortyx::resource::BufferResult oc = other_gpu.resources()->create_buffer(
                vortyx::resource::BufferDesc::of<std::int32_t>(
                    4, vortyx::resource::ResourceAccess::Write),
                other_gpu.backend_name());
            check(oa.status == Status::Ok && ob.status == Status::Ok && oc.status == Status::Ok,
                  "7b: foreign Virtual GPU buffers must be created");
            check(std::string(oa.buffer.backend_name()) == "cpu",
                  "7b: the foreign buffers must share the configured backend name "
                  "(so only the ownership check can reject them)");

            const ComputeResult refused = cpu_gpu.execute(oa.buffer, ob.buffer, oc.buffer);
            check(refused.status == Status::InvalidInput,
                  std::string("7b: another Virtual GPU's buffers must be rejected with InvalidInput "
                              "(got ") + vortyx::compute::to_string(refused.status) + ")");
            check(!refused.error.empty(),
                  "7b: the rejection must explain the ownership rule");

            // Keep one handle alive past the other Virtual GPU's shutdown to
            // also cover the outlived-manager case in the same shape.
            survivor = std::move(oc.buffer);
            other_gpu.shutdown();
        }
        const ComputeResult after_shutdown = cpu_gpu.execute(survivor, survivor, survivor);
        check(after_shutdown.status == Status::InvalidInput,
              "7b: a handle whose Virtual GPU is gone must still be rejected cleanly");
        survivor.reset();
    }

    // --- 8. Known-but-unavailable backend: honest, no silent fallback --------
    // Environment-adaptive but always honest: on machines WITH a usable
    // Vulkan device this exercises real execution; without one it verifies
    // that the failure is reported truthfully instead of sneaking onto the
    // CPU. Both outcomes are valid; faking is not.
    {
        VirtualGpu vulkan_gpu;
        VirtualGpuDesc desc;
        desc.backend = "vulkan";
        const Status init = vulkan_gpu.initialize(desc);
        check(init == Status::Ok,
              "initializing with a known backend must succeed even when it is unavailable");

        if (vulkan_gpu.backend_available()) {
            const VectorAddResult r = vulkan_gpu.execute(make_task(64));
            check(r.status == Status::Ok && result_matches(r, make_task(64)),
                  "an available vulkan backend must really execute through the Virtual GPU");
        } else {
            const std::string reason = vulkan_gpu.backend_unavailable_reason();
            check(!reason.empty(),
                  "an unavailable backend must expose a non-empty honest reason");
            const VectorAddResult r = vulkan_gpu.execute(make_task(64));
            check(r.status == Status::BackendUnavailable,
                  "execute() on an unavailable backend must fail with BackendUnavailable "
                  "(silent CPU fallback forbidden)");
            check(!r.error.empty(), "the BackendUnavailable result must carry the real reason");
            std::cout << "Note: vulkan backend unavailable in this environment - "
                      << reason << "\n";
        }
        vulkan_gpu.shutdown();
    }

    // --- 9. shutdown: state, refusal, safety, re-initialization --------------
    {
        cpu_gpu.shutdown();
        check(cpu_gpu.state() == State::ShutDown, "shutdown() must end in ShutDown");
        check(!cpu_gpu.is_ready(), "a shut-down VirtualGpu must not be ready");
        check(cpu_gpu.resources() == nullptr,
              "a shut-down VirtualGpu must not expose a Resource Manager");
        check(!cpu_gpu.backend_available(), "a shut-down VirtualGpu must not claim availability");

        const VectorAddResult after = cpu_gpu.execute(make_task(4));
        check(after.status == Status::NotInitialized,
              "execute() after shutdown() must fail with NotInitialized");
        check(after.error.find("shut down") != std::string::npos,
              "the post-shutdown error must clearly say the Virtual GPU is shut down");

        cpu_gpu.shutdown();  // double shutdown must be a safe no-op
        check(cpu_gpu.state() == State::ShutDown, "double shutdown() must stay ShutDown");

        check(cpu_gpu.initialize() == Status::Ok,
              "re-initialization after shutdown() must succeed");
        check(cpu_gpu.state() == State::Ready, "re-initialized VirtualGpu must be Ready");
        const VectorAddResult revived = cpu_gpu.execute(make_task(32));
        check(revived.status == Status::Ok && result_matches(revived, make_task(32)),
              "re-initialized VirtualGpu must execute correctly");
    }

    // --- 10. Repeated initialize/shutdown cycles stay safe -------------------
    {
        VirtualGpu gpu;
        for (int cycle = 0; cycle < 3; ++cycle) {
            check(gpu.initialize() == Status::Ok, "cycle " + std::to_string(cycle) + " init");
            check(gpu.execute(make_task(16)).status == Status::Ok,
                  "cycle " + std::to_string(cycle) + " execute");
            gpu.shutdown();
            check(gpu.state() == State::ShutDown,
                  "cycle " + std::to_string(cycle) + " shutdown");
        }
    }

    // --- 11. Move semantics: ownership transfers exactly once ----------------
    {
        VirtualGpu source;
        check(source.initialize() == Status::Ok, "move test: source must initialize");

        VirtualGpu destination(std::move(source));
        check(destination.state() == State::Ready, "the destination must inherit the Ready state");
        check(destination.backend_name() == "cpu", "the destination must inherit the description");
        check(destination.resources() != nullptr, "the destination must own the Resource Manager");
        const VectorAddResult moved_run = destination.execute(make_task(24));
        check(moved_run.status == Status::Ok && result_matches(moved_run, make_task(24)),
              "the destination must be fully functional after the move");
        check(source.state() == State::Uninitialized,
              "the moved-from source must be Uninitialized");
        check(source.execute(make_task(4)).status == Status::NotInitialized,
              "the moved-from source must refuse execute()");
        source.shutdown();  // must be safe on a moved-from object

        // Move assignment.
        VirtualGpu assigned;
        assigned.initialize(VirtualGpuDesc{});  // give it its own live state first
        assigned = std::move(destination);
        check(assigned.state() == State::Ready && assigned.execute(make_task(24)).status == Status::Ok,
              "move assignment must transfer a working VirtualGpu");
        check(destination.state() == State::Uninitialized,
              "the move-assigned source must be Uninitialized");
        // The assigned object's previous Runtime was released by the
        // assignment (shutdown before takeover) — no leaks, no double owner.
    }

    // --- 12. Buffer handles outliving their Virtual GPU stay safe ------------
    {
        vortyx::resource::Buffer survivor;
        {
            VirtualGpu gpu;
            check(gpu.initialize() == Status::Ok, "outliving test: init must succeed");
            vortyx::resource::BufferResult created =
                gpu.resources()->create_buffer(
                    vortyx::resource::BufferDesc::of<std::int32_t>(8,
                        vortyx::resource::ResourceAccess::Read),
                    gpu.backend_name());
            check(created.status == Status::Ok, "outliving test: buffer creation");
            survivor = std::move(created.buffer);
        }  // VirtualGpu destroyed here: Runtime, manager and all resources released

        check(!survivor.valid(), "a handle whose Virtual GPU is gone must be invalid");
        const std::vector<std::int32_t> data(8, 1);
        check(survivor.write(data.data(), data.size() * sizeof(std::int32_t)).status !=
                  Status::Ok,
              "writing through an orphaned handle must fail cleanly");
        std::vector<std::int32_t> out(8, 0);
        check(survivor.read(out.data(), out.size() * sizeof(std::int32_t)).status != Status::Ok,
              "reading through an orphaned handle must fail cleanly");
    }  // survivor destructor: safe no-op, no crash

    cpu_gpu.shutdown();

    if (failures == 0) {
        std::cout << "Virtual GPU CPU tests passed.\n";
        return 0;
    }
    return 1;
}
