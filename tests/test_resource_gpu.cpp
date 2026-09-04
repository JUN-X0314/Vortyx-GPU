// Compute Resource & Memory Management tests (Phase 4) — GPU (Vulkan) path.
//
// Design rules (same policy as Phase 3's GPU tests):
//  - No hardcoded hardware expectations (no "there must be exactly 1 GPU",
//    no vendor names). Runs on AMD, Intel, NVIDIA, software Vulkan
//    implementations (lavapipe/llvmpipe) and on machines with NO GPU at all.
//  - When the Vulkan backend is unavailable, the test exits successfully
//    with an explicit, visible note saying it did NOT run. It is never
//    reported as GPU success and never silently skipped.
//  - When the backend IS available, REAL GPU buffers are allocated
//    (VkBuffer + VkDeviceMemory through the resource layer), data is
//    uploaded/downloaded through the Buffer API, and results must match the
//    CPU reference bit-exactly. No fake GPU resources exist anywhere.

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "core/compute/task.hpp"
#include "core/compute/runtime.hpp"
#include "core/device/device.hpp"
#include "core/resource/buffer.hpp"
#include "core/resource/resource.hpp"
#include "core/resource/resource_manager.hpp"

using vortyx::compute::ComputeResult;
using vortyx::compute::Runtime;
using vortyx::compute::Status;
using vortyx::compute::to_string;
using vortyx::resource::Buffer;
using vortyx::resource::BufferDesc;
using vortyx::resource::BufferResult;
using vortyx::resource::MemoryLocation;
using vortyx::resource::ResourceAccess;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// to_string(Status) returns const char*; wrapper for string concatenation.
std::string status_name(Status status) { return vortyx::compute::to_string(status); }

std::vector<std::int32_t> make_values(std::size_t count) {
    std::vector<std::int32_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        // Deterministic values well inside int32 range (no overflow).
        values[i] = static_cast<std::int32_t>(i % 1000) - 300;
    }
    return values;
}

}  // namespace

int main() {
    Runtime runtime;
    if (runtime.initialize() != Status::Ok) {
        std::cerr << "FAIL: runtime initialize() failed\n";
        return 1;
    }

    if (!runtime.has_backend("vulkan")) {
        // Environment without a usable Vulkan device: a normal, non-fatal
        // condition. Report it clearly; do NOT fake GPU success.
        std::cout << "SKIPPED (environment): Vulkan backend unavailable - "
                  << runtime.backend_unavailable_reason("vulkan") << "\n";
        std::cout << "Note: GPU resource management was NOT tested on this machine. "
                  << "CPU resource path is covered by test_resource.\n";
        runtime.shutdown();
        return 0;
    }

    vortyx::resource::ResourceManager& manager = runtime.resources();

    std::cout << "Vulkan backend available: "
              << (runtime.backend_device("vulkan").name.empty()
                      ? std::string("unknown device")
                      : runtime.backend_device("vulkan").name)
              << "\n";

    // --- 1. Real GPU buffer creation through the resource layer --------------
    {
        BufferResult created =
            manager.create_buffer(BufferDesc::of<std::int32_t>(64, ResourceAccess::Read), "vulkan");
        check(created.status == Status::Ok,
              "vulkan buffer creation must succeed (error: " + created.error + ")");
        check(created.buffer.valid(), "vulkan buffer handle must be valid");
        check(std::string(created.buffer.backend_name()) == "vulkan",
              "vulkan buffer must report backend 'vulkan'");
        check(created.buffer.memory_location() == MemoryLocation::Device,
              "vulkan buffer memory_location must be Device (real device memory, "
              "never relabeled as host memory)");
        check(created.buffer.byte_size() == 64 * sizeof(std::int32_t),
              "vulkan buffer logical byte size must be exact");
        check(manager.stats().live_buffers == 1, "stats must count the live GPU buffer");
        created.buffer.reset();
        check(manager.stats().live_buffers == 0, "GPU buffer release must free the resource");
    }

    // --- 2. Full GPU lifecycle: create -> write -> execute -> read -> release
    const std::vector<std::size_t> sizes = {4, 16, 64, 1024, 5000};  // incl. non-workgroup multiples
    for (std::size_t size : sizes) {
        BufferResult a =
            manager.create_buffer(BufferDesc::of<std::int32_t>(size, ResourceAccess::Read), "vulkan");
        BufferResult b =
            manager.create_buffer(BufferDesc::of<std::int32_t>(size, ResourceAccess::Read), "vulkan");
        BufferResult c =
            manager.create_buffer(BufferDesc::of<std::int32_t>(size, ResourceAccess::Write), "vulkan");
        check(a.status == Status::Ok && b.status == Status::Ok && c.status == Status::Ok,
              "GPU buffers must be created for size " + std::to_string(size));

        const std::vector<std::int32_t> va = make_values(size);
        const std::vector<std::int32_t> vb = make_values(size);
        const std::size_t bytes = size * sizeof(std::int32_t);

        check(a.buffer.write(va.data(), bytes).status == Status::Ok,
              "GPU upload (write) must succeed for size " + std::to_string(size));
        check(b.buffer.write(vb.data(), bytes).status == Status::Ok,
              "GPU upload (write) must succeed for size " + std::to_string(size));

        const ComputeResult exec = runtime.execute(a.buffer, b.buffer, c.buffer);
        check(exec.status == Status::Ok,
              "GPU execution must succeed for size " + std::to_string(size) +
                  " (error: " + exec.error + ")");

        std::vector<std::int32_t> result(size, 0);
        check(c.buffer.read(result.data(), bytes).status == Status::Ok,
              "GPU download (read) must succeed for size " + std::to_string(size));

        bool match = true;
        for (std::size_t i = 0; i < size; ++i) {
            if (result[i] != va[i] + vb[i]) {
                match = false;
                break;
            }
        }
        check(match, "GPU buffer result must equal A+B for size " + std::to_string(size));

        // Cross-check against the CPU resource path: same API, different
        // backend, bit-exact identical results.
        BufferResult ca =
            manager.create_buffer(BufferDesc::of<std::int32_t>(size, ResourceAccess::Read), "cpu");
        BufferResult cb =
            manager.create_buffer(BufferDesc::of<std::int32_t>(size, ResourceAccess::Read), "cpu");
        BufferResult cc =
            manager.create_buffer(BufferDesc::of<std::int32_t>(size, ResourceAccess::Write), "cpu");
        check(ca.status == Status::Ok && cb.status == Status::Ok && cc.status == Status::Ok,
              "CPU reference buffers must be created");
        check(ca.buffer.write(va.data(), bytes).status == Status::Ok, "cpu write a");
        check(cb.buffer.write(vb.data(), bytes).status == Status::Ok, "cpu write b");
        check(runtime.execute(ca.buffer, cb.buffer, cc.buffer).status == Status::Ok,
              "cpu resource execution must succeed");
        std::vector<std::int32_t> cpu_result(size, 0);
        check(cc.buffer.read(cpu_result.data(), bytes).status == Status::Ok, "cpu read c");
        check(cpu_result == result,
              "CPU and GPU resource results must be bit-identical for size " +
                  std::to_string(size));
    }

    // --- 3. GPU-side error policies (no crash, explicit errors) --------------
    {
        BufferResult a =
            manager.create_buffer(BufferDesc::of<std::int32_t>(8, ResourceAccess::Read), "vulkan");
        Buffer gpu_buffer = std::move(a.buffer);

        const std::vector<std::int32_t> nine(9, 1);
        const ComputeResult w = gpu_buffer.write(nine.data(), nine.size() * sizeof(std::int32_t));
        check(w.status == Status::InvalidInput,
              "oversized GPU write must be rejected before touching the device (got " +
                  status_name(w.status) + ")");

        std::vector<std::int32_t> out(9, 0);
        const ComputeResult r = gpu_buffer.read(out.data(), out.size() * sizeof(std::int32_t));
        check(r.status == Status::InvalidInput,
              "oversized GPU read must be rejected before touching the device");

        gpu_buffer.reset();

        // Wrong access roles are rejected before any GPU dispatch.
        BufferResult in_a =
            manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "vulkan");
        BufferResult in_b =
            manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "vulkan");
        BufferResult out_bad =
            manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "vulkan");
        const ComputeResult exec = runtime.execute(in_a.buffer, in_b.buffer, out_bad.buffer);
        check(exec.status == Status::InvalidInput,
              "GPU execution with a read-only output buffer must be rejected (got " +
                  status_name(exec.status) + ")");

        // Mixing backends is an explicit error: CPU input + GPU input/output.
        BufferResult cpu_in =
            manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "cpu");
        const ComputeResult mixed = runtime.execute(cpu_in.buffer, in_b.buffer, in_a.buffer);
        check(mixed.status == Status::InvalidInput,
              "mixed-backend execution must be rejected (got " + status_name(mixed.status) + ")");
        check(mixed.error.find("different backends") != std::string::npos,
              "mixed-backend error must explain the constraint");
    }

    // --- 4. Shutdown while GPU buffers are alive is safe ---------------------
    {
        Buffer survivor;
        {
            Runtime inner;
            check(inner.initialize() == Status::Ok, "inner runtime must initialize");
            if (inner.has_backend("vulkan")) {
                BufferResult created = inner.resources().create_buffer(
                    BufferDesc::of<std::int32_t>(1024, ResourceAccess::Read), "vulkan");
                check(created.status == Status::Ok,
                      "GPU buffer creation on inner runtime must succeed");
                survivor = std::move(created.buffer);
                check(inner.resources().stats().live_buffers == 1,
                      "inner stats must count the live GPU buffer");

                inner.shutdown();  // releases the buffer BEFORE the device dies
                check(inner.resources().stats().live_buffers == 0,
                      "shutdown must release live GPU buffers (no leaks, no invalid frees)");
                const std::vector<std::int32_t> data(1024, 0);
                const ComputeResult stale =
                    survivor.write(data.data(), data.size() * sizeof(std::int32_t));
                check(stale.status == Status::NotInitialized,
                      "GPU handle after shutdown must fail cleanly (got " +
                          status_name(stale.status) + ")");
            }
        }
        survivor.reset();  // manager gone: must be a safe no-op
    }

    // --- 5. Re-initialization: GPU buffers work again -------------------------
    runtime.shutdown();
    check(runtime.initialize() == Status::Ok, "runtime re-initialization must work");
    if (runtime.has_backend("vulkan")) {
        BufferResult a = runtime.resources().create_buffer(
            BufferDesc::of<std::int32_t>(1024, ResourceAccess::Read), "vulkan");
        BufferResult b = runtime.resources().create_buffer(
            BufferDesc::of<std::int32_t>(1024, ResourceAccess::Read), "vulkan");
        BufferResult c = runtime.resources().create_buffer(
            BufferDesc::of<std::int32_t>(1024, ResourceAccess::Write), "vulkan");
        check(a.status == Status::Ok && b.status == Status::Ok && c.status == Status::Ok,
              "GPU buffer creation after re-init must work");

        const std::vector<std::int32_t> va = make_values(1024);
        const std::vector<std::int32_t> vb = make_values(1024);
        const std::size_t bytes = 1024 * sizeof(std::int32_t);
        check(a.buffer.write(va.data(), bytes).status == Status::Ok, "GPU write a after re-init");
        check(b.buffer.write(vb.data(), bytes).status == Status::Ok, "GPU write b after re-init");
        check(runtime.execute(a.buffer, b.buffer, c.buffer).status == Status::Ok,
              "GPU execution after re-init must work");
        std::vector<std::int32_t> result(1024, 0);
        check(c.buffer.read(result.data(), bytes).status == Status::Ok, "GPU read c after re-init");
        bool match = true;
        for (std::size_t i = 0; i < 1024; ++i) {
            if (result[i] != va[i] + vb[i]) {
                match = false;
                break;
            }
        }
        check(match, "GPU result after re-init must equal A+B");
    }

    // --- 6. Final accounting --------------------------------------------------
    runtime.shutdown();
    check(runtime.resources().stats().live_buffers == 0,
          "no GPU or CPU buffers may survive full shutdown (no leaks)");

    if (failures == 0) {
        std::cout << "GPU resource tests passed.\n";
        return 0;
    }
    return 1;
}
