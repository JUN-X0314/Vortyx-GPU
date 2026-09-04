// Compute Resource & Memory Management tests (Phase 4) — CPU path.
//
// These tests MUST pass on every system, including machines without any GPU
// and CPU-only builds. They verify the full Buffer lifecycle against the
// CPU (host memory) backend:
//   creation -> info -> write -> read -> release, plus every documented
//   error policy, move semantics, stats accounting, shutdown safety and the
//   resource-based vector addition execution path.

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "core/compute/task.hpp"
#include "core/compute/runtime.hpp"
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
using vortyx::resource::kInvalidResourceId;
using vortyx::resource::kMaxBufferBytes;
using vortyx::resource::MemoryLocation;
using vortyx::resource::ResourceAccess;
using vortyx::resource::ResourceId;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// to_string(Status) returns const char*; this wrapper makes it usable in
// std::string concatenations inside check messages.
std::string status_name(Status status) { return vortyx::compute::to_string(status); }

// Buffer handles must never be copyable (GPU-backed resources must not be
// accidentally aliased). Verified at compile time.
static_assert(!std::is_copy_constructible<Buffer>::value,
              "Buffer must not be copy-constructible");
static_assert(!std::is_copy_assignable<Buffer>::value, "Buffer must not be copy-assignable");
static_assert(std::is_move_constructible<Buffer>::value, "Buffer must be move-constructible");
static_assert(std::is_move_assignable<Buffer>::value, "Buffer must be move-assignable");

std::vector<std::int32_t> make_values(std::size_t count) {
    std::vector<std::int32_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
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
    vortyx::resource::ResourceManager& manager = runtime.resources();

    // --- 1. Creation and honest resource info --------------------------------
    {
        const BufferDesc desc = BufferDesc::of<std::int32_t>(16, ResourceAccess::Read);
        BufferResult created = manager.create_buffer(desc, "cpu");
        check(created.status == Status::Ok,
              "cpu buffer creation must succeed (error: " + created.error + ")");
        check(created.buffer.valid(), "created buffer must be a valid handle");
        check(created.buffer.id() != kInvalidResourceId, "created buffer must have a real id");
        check(created.buffer.byte_size() == 16 * sizeof(std::int32_t),
              "byte_size must be element_count * element_size");
        check(created.buffer.element_count() == 16, "element_count must round-trip");
        check(created.buffer.desc().element_size == sizeof(std::int32_t),
              "element_size must round-trip");
        check(created.buffer.memory_location() == MemoryLocation::Host,
              "cpu buffer memory_location must be Host (never faked)");
        check(std::string(created.buffer.backend_name()) == "cpu",
              "cpu buffer backend_name must be cpu");

        const vortyx::resource::ResourceStats stats = manager.stats();
        check(stats.live_buffers == 1, "stats must count the live buffer");
        check(stats.live_bytes == 64, "stats must count the live bytes");
        check(stats.total_allocations == 1, "stats must count the allocation");
        created.buffer.reset();

        check(!created.buffer.valid(), "reset() must invalidate the handle");
        check(manager.stats().live_buffers == 0, "reset() must release the resource");
        created.buffer.reset();  // double release must be a safe no-op
        check(manager.stats().live_buffers == 0, "double reset() must not corrupt stats");
    }

    // --- 2. write/read round-trip (full and partial) -------------------------
    {
        BufferResult created =
            manager.create_buffer(BufferDesc::of<std::int32_t>(64, ResourceAccess::Read), "cpu");
        check(created.status == Status::Ok, "roundtrip buffer creation must succeed");
        Buffer buffer = std::move(created.buffer);

        const std::vector<std::int32_t> data = make_values(64);
        const ComputeResult w = buffer.write(data.data(), data.size() * sizeof(std::int32_t));
        check(w.status == Status::Ok, "full write must succeed (error: " + w.error + ")");

        std::vector<std::int32_t> out(64, 0);
        const ComputeResult r = buffer.read(out.data(), out.size() * sizeof(std::int32_t));
        check(r.status == Status::Ok, "full read must succeed (error: " + r.error + ")");
        check(out == data, "read-after-write must return the written values");

        // Partial transfer policy: writing/reading less than the buffer size
        // is allowed (the full size is the upper bound).
        const std::vector<std::int32_t> head = {7, 8, 9, 10};
        check(buffer.write(head.data(), head.size() * sizeof(std::int32_t)).status == Status::Ok,
              "partial write (16 of 256 bytes) must be allowed");
        std::vector<std::int32_t> head_out(4, 0);
        check(buffer.read(head_out.data(), head_out.size() * sizeof(std::int32_t)).status ==
                  Status::Ok,
              "partial read must be allowed");
        check(head_out == head, "partial read must return the partial write's values");
        check(out[4] == data[4], "partial write must not disturb the untouched elements");
        buffer.reset();
    }

    // --- 3. Error policy: oversized / null / zero transfers ------------------
    {
        BufferResult created =
            manager.create_buffer(BufferDesc::of<std::int32_t>(8, ResourceAccess::Read), "cpu");
        Buffer buffer = std::move(created.buffer);

        const std::vector<std::int32_t> nine(9, 1);
        ComputeResult w = buffer.write(nine.data(), nine.size() * sizeof(std::int32_t));
        check(w.status == Status::InvalidInput,
              "write larger than the buffer must be rejected (got " + status_name(w.status) + ")");
        check(w.error.find("exceeds") != std::string::npos,
              "oversized write error must mention 'exceeds'");

        std::vector<std::int32_t> out(9, 0);
        ComputeResult r = buffer.read(out.data(), out.size() * sizeof(std::int32_t));
        check(r.status == Status::InvalidInput,
              "read larger than the buffer must be rejected (got " + status_name(r.status) + ")");
        check(r.error.find("exceeds") != std::string::npos,
              "oversized read error must mention 'exceeds'");

        const std::vector<std::int32_t> ok_data(8, 2);
        w = buffer.write(nullptr, sizeof(std::int32_t));
        check(w.status == Status::InvalidInput, "null-pointer write must be rejected");
        r = buffer.read(nullptr, sizeof(std::int32_t));
        check(r.status == Status::InvalidInput, "null-pointer read must be rejected");

        w = buffer.write(ok_data.data(), 0);
        check(w.status == Status::InvalidInput, "zero-byte write must be rejected by policy");
        r = buffer.read(out.data(), 0);
        check(r.status == Status::InvalidInput, "zero-byte read must be rejected by policy");
        buffer.reset();
    }

    // --- 4. Description validation: empty / zero element size / no access ----
    {
        const BufferResult zero =
            manager.create_buffer(BufferDesc::of<std::int32_t>(0, ResourceAccess::Read), "cpu");
        check(zero.status == Status::InvalidInput,
              "zero-element buffer creation must be rejected (got " + status_name(zero.status) + ")");
        check(!zero.buffer.valid(), "rejected creation must not return a valid handle");
        check(zero.error.find("zero-element") != std::string::npos,
              "zero-element rejection must explain the policy");

        const BufferResult no_access =
            manager.create_buffer(BufferDesc{4, sizeof(std::int32_t), ResourceAccess::None}, "cpu");
        check(no_access.status == Status::InvalidInput,
              "access=None buffer creation must be rejected");

        BufferDesc bad_size;
        bad_size.element_count = 4;
        bad_size.element_size = 0;
        bad_size.access = ResourceAccess::Read;
        const BufferResult zero_elem = manager.create_buffer(bad_size, "cpu");
        check(zero_elem.status == Status::InvalidInput,
              "element_size=0 buffer creation must be rejected");
    }

    // --- 5. Overflow and safety cap: absurd requests fail safely -------------
    {
        // Byte size multiplication would overflow size_t.
        BufferDesc overflow_desc;
        overflow_desc.element_count = std::numeric_limits<std::size_t>::max();
        overflow_desc.element_size = sizeof(std::int64_t);
        overflow_desc.access = ResourceAccess::Read;
        const BufferResult overflow = manager.create_buffer(overflow_desc, "cpu");
        check(overflow.status == Status::InvalidInput,
              "overflowing byte size must be rejected, never wrapped (got " +
                  status_name(overflow.status) + ")");

        // Below overflow but beyond the documented per-buffer safety limit.
        const BufferResult too_big =
            manager.create_buffer(BufferDesc{kMaxBufferBytes + 1, 1, ResourceAccess::Read}, "cpu");
        check(too_big.status == Status::InvalidInput,
              "requests beyond kMaxBufferBytes must be rejected");
        check(too_big.error.find("limit") != std::string::npos,
              "cap rejection must mention the safety limit");
        check(manager.stats().live_buffers == 0,
              "rejected creations must not leave resources behind");
    }

    // --- 6. Unknown / unavailable providers ----------------------------------
    {
        const BufferResult unknown =
            manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "nope");
        check(unknown.status == Status::BackendUnavailable,
              "unknown backend creation must return BackendUnavailable");
        check(!unknown.error.empty(), "unknown backend error must be descriptive");

        // The vulkan provider only exists when the GPU backend is available;
        // on systems without one the request must fail cleanly (this is the
        // GPU-free variant of the policy; the real GPU path lives in
        // test_resource_gpu).
        if (!runtime.has_backend("vulkan")) {
            const BufferResult gpu = manager.create_buffer(
                BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "vulkan");
            check(gpu.status == Status::BackendUnavailable,
                  "vulkan buffer creation without a vulkan device must return "
                  "BackendUnavailable");
            check(!gpu.error.empty(), "vulkan unavailability must explain why");
        }
    }

    // --- 7. Invalid handles ---------------------------------------------------
    {
        Buffer empty;  // never created
        check(!empty.valid(), "default-constructed handle must be invalid");
        check(empty.id() == kInvalidResourceId, "default handle id must be kInvalidResourceId");

        const std::vector<std::int32_t> data(2, 1);
        ComputeResult w = empty.write(data.data(), sizeof(std::int32_t));
        check(w.status == Status::InvalidInput,
              "write through an empty handle must fail cleanly (got " + status_name(w.status) + ")");
        std::vector<std::int32_t> out(2, 0);
        ComputeResult r = empty.read(out.data(), sizeof(std::int32_t));
        check(r.status == Status::InvalidInput,
              "read through an empty handle must fail cleanly");
        empty.reset();  // must be a no-op
        check(!empty.valid(), "reset() on an empty handle must stay invalid");
    }

    // --- 8. Move semantics transfer ownership exactly once -------------------
    {
        BufferResult created =
            manager.create_buffer(BufferDesc::of<std::int32_t>(32, ResourceAccess::Read), "cpu");
        check(created.status == Status::Ok, "move-test buffer creation must succeed");
        const ResourceId original_id = created.buffer.id();
        const std::size_t live_before = manager.stats().live_buffers;

        Buffer target = std::move(created.buffer);
        check(target.valid(), "move target must be valid");
        check(target.id() == original_id, "move must transfer the resource id");
        check(!created.buffer.valid(), "moved-from handle must be invalid");
        check(created.buffer.id() == kInvalidResourceId,
              "moved-from handle id must be kInvalidResourceId");
        check(created.buffer.byte_size() == 0, "moved-from description must be reset");
        check(manager.stats().live_buffers == live_before,
              "move must NOT create or destroy a resource");

        const std::vector<std::int32_t> data = make_values(32);
        check(target.write(data.data(), data.size() * sizeof(std::int32_t)).status == Status::Ok,
              "moved-to handle must be fully usable");

        // Move assignment (target currently owns a resource; it must be
        // released, not leaked, and the moved-from source must go inert).
        Buffer assigned;
        assigned = std::move(target);
        check(assigned.valid(), "move-assigned handle must be valid");
        check(!target.valid(), "move-assigned-from handle must be invalid");
        check(manager.stats().live_buffers == live_before,
              "move assignment must keep exactly one live resource");

        // Both stale handles destruct at scope exit; only 'assigned' releases
        // the resource. If ownership were duplicated, stats would go negative
        // or the program would crash.
        const std::size_t live_before_release = manager.stats().live_buffers;
        assigned.reset();
        check(manager.stats().live_buffers == live_before_release - 1,
              "release after moves must free exactly one resource");
    }

    // --- 9. RAII scope destruction (leak check) -------------------------------
    {
        const std::size_t live_before = manager.stats().live_buffers;
        {
            const BufferResult a =
                manager.create_buffer(BufferDesc::of<std::int32_t>(16, ResourceAccess::Read), "cpu");
            const BufferResult b =
                manager.create_buffer(BufferDesc::of<std::int32_t>(16, ResourceAccess::Read), "cpu");
            const BufferResult c =
                manager.create_buffer(BufferDesc::of<std::int32_t>(16, ResourceAccess::Write), "cpu");
            check(a.status == Status::Ok && b.status == Status::Ok && c.status == Status::Ok,
                  "scope-test buffers must be created");
            check(manager.stats().live_buffers == live_before + 3,
                  "stats must reflect three live buffers");
        }
        check(manager.stats().live_buffers == live_before,
              "RAII destruction at scope exit must release every buffer (no leaks)");
    }

    // --- 10. Resource-based vector addition on the CPU backend ----------------
    {
        const std::size_t count = 1000;
        BufferResult a =
            manager.create_buffer(BufferDesc::of<std::int32_t>(count, ResourceAccess::Read), "cpu");
        BufferResult b =
            manager.create_buffer(BufferDesc::of<std::int32_t>(count, ResourceAccess::Read), "cpu");
        BufferResult c =
            manager.create_buffer(BufferDesc::of<std::int32_t>(count, ResourceAccess::Write), "cpu");
        check(a.status == Status::Ok && b.status == Status::Ok && c.status == Status::Ok,
              "vector-add buffers must be created");

        const std::vector<std::int32_t> va = make_values(count);
        const std::vector<std::int32_t> vb = make_values(count);
        const std::size_t bytes = count * sizeof(std::int32_t);
        check(a.buffer.write(va.data(), bytes).status == Status::Ok, "write a must succeed");
        check(b.buffer.write(vb.data(), bytes).status == Status::Ok, "write b must succeed");

        const ComputeResult exec = runtime.execute(a.buffer, b.buffer, c.buffer);
        check(exec.status == Status::Ok,
              "resource-based cpu vector addition must succeed (error: " + exec.error + ")");

        std::vector<std::int32_t> result(count, 0);
        check(c.buffer.read(result.data(), bytes).status == Status::Ok, "read c must succeed");
        bool match = true;
        for (std::size_t i = 0; i < count; ++i) {
            if (result[i] != va[i] + vb[i]) {
                match = false;
                break;
            }
        }
        check(match, "resource-based cpu result must equal A+B elementwise");

        // Cross-check: the same data through the Phase 3 task API must
        // produce the identical result (both now flow through the resource
        // layer internally).
        vortyx::compute::VectorAddTask task;
        task.a = va;
        task.b = vb;
        const vortyx::compute::VectorAddResult task_result = runtime.execute(task, "cpu");
        check(task_result.status == Status::Ok, "task-based cpu execution must still work");
        check(task_result.data == result,
              "task-based and resource-based cpu results must be identical");
    }

    // --- 11. Resource-based execution validation ------------------------------
    {
        // Wrong access roles are rejected BEFORE any compute happens.
        {
            BufferResult a =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "cpu");
            BufferResult b =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "cpu");
            BufferResult c_bad =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "cpu");
            const ComputeResult r = runtime.execute(a.buffer, b.buffer, c_bad.buffer);
            check(r.status == Status::InvalidInput,
                  "output buffer without Write access must be rejected (got " +
                      status_name(r.status) + ")");
            check(r.error.find("Write") != std::string::npos,
                  "access rejection must mention the Write role");
        }
        {
            BufferResult a_bad =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Write), "cpu");
            BufferResult b =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "cpu");
            BufferResult c =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Write), "cpu");
            const ComputeResult r = runtime.execute(a_bad.buffer, b.buffer, c.buffer);
            check(r.status == Status::InvalidInput,
                  "input buffer without Read access must be rejected");
        }
        // Element-count mismatch across the three buffers.
        {
            BufferResult a =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "cpu");
            BufferResult b =
                manager.create_buffer(BufferDesc::of<std::int32_t>(8, ResourceAccess::Read), "cpu");
            BufferResult c =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Write), "cpu");
            const ComputeResult r = runtime.execute(a.buffer, b.buffer, c.buffer);
            check(r.status == Status::InvalidInput,
                  "element-count mismatch must be rejected (got " + status_name(r.status) + ")");
        }
        // Wrong element representation (not int32).
        {
            BufferResult a =
                manager.create_buffer(BufferDesc{4, 8, ResourceAccess::Read}, "cpu");
            BufferResult b =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "cpu");
            BufferResult c =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Write), "cpu");
            const ComputeResult r = runtime.execute(a.buffer, b.buffer, c.buffer);
            check(r.status == Status::InvalidInput, "non-int32 element buffer must be rejected");
            check(r.error.find("int32") != std::string::npos,
                  "element rejection must mention int32");
        }
        // Empty / released handles.
        {
            BufferResult a =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "cpu");
            BufferResult b =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Read), "cpu");
            BufferResult c =
                manager.create_buffer(BufferDesc::of<std::int32_t>(4, ResourceAccess::Write), "cpu");

            Buffer empty;
            ComputeResult r = runtime.execute(a.buffer, b.buffer, empty);
            check(r.status == Status::InvalidInput,
                  "executing with an empty output handle must fail cleanly");

            Buffer released = std::move(c.buffer);
            released.reset();
            r = runtime.execute(a.buffer, b.buffer, released);
            check(r.status == Status::InvalidInput,
                  "executing with a released handle must fail cleanly");
        }
    }

    // --- 12. Runtime shutdown with live resources is safe --------------------
    {
        Buffer handle_a;  // deliberately declared OUTSIDE the inner runtime
        {
            Runtime inner;
            check(inner.initialize() == Status::Ok, "inner runtime must initialize");

            BufferResult a = inner.resources().create_buffer(
                BufferDesc::of<std::int32_t>(16, ResourceAccess::Read), "cpu");
            check(a.status == Status::Ok, "inner buffer creation must succeed");

            const std::vector<std::int32_t> data = make_values(16);
            check(a.buffer.write(data.data(), data.size() * sizeof(std::int32_t)).status ==
                      Status::Ok,
                  "inner buffer write must succeed");

            inner.shutdown();

            // After shutdown the registry is empty: the handle is inert, ops
            // fail with NotInitialized, and no host/GPU storage is touched.
            check(inner.resources().stats().live_buffers == 0,
                  "shutdown must release all live resources");
            const ComputeResult w = a.buffer.write(data.data(), data.size() * sizeof(std::int32_t));
            check(w.status == Status::NotInitialized,
                  "write after shutdown must report NotInitialized (got " +
                      status_name(w.status) + ")");

            // The handle outlives the inner Runtime block: keep it for the
            // outer-scope safety check below.
            handle_a = std::move(a.buffer);
        }  // inner Runtime destroyed here; handle_a still alive

        // The manager object is gone now. The handle must be completely
        // inert: invalid, safe to use, safe to destroy.
        check(!handle_a.valid(), "handle whose manager died must report invalid");
        const std::vector<std::int32_t> data(16, 0);
        const ComputeResult w = handle_a.write(data.data(), data.size() * sizeof(std::int32_t));
        check(w.status == Status::NotInitialized,
              "write through a handle whose manager died must be a clean error (got " +
                  status_name(w.status) + ")");
        handle_a.reset();  // must be a no-op, never a crash
    }

    // --- 13. Re-initialization after shutdown keeps resources working --------
    runtime.shutdown();
    check(runtime.initialize() == Status::Ok, "re-initialization after shutdown must work");

    {
        BufferResult a =
            manager.create_buffer(BufferDesc::of<std::int32_t>(16, ResourceAccess::Read), "cpu");
        BufferResult b =
            manager.create_buffer(BufferDesc::of<std::int32_t>(16, ResourceAccess::Read), "cpu");
        BufferResult c =
            manager.create_buffer(BufferDesc::of<std::int32_t>(16, ResourceAccess::Write), "cpu");
        check(a.status == Status::Ok,
              "buffer creation after re-initialization must work (error: " + a.error + ")");
        const std::vector<std::int32_t> va = make_values(16);
        const std::vector<std::int32_t> vb = make_values(16);
        check(a.buffer.write(va.data(), va.size() * sizeof(std::int32_t)).status == Status::Ok,
              "write after re-init must work");
        check(b.buffer.write(vb.data(), vb.size() * sizeof(std::int32_t)).status == Status::Ok,
              "write after re-init must work");
        check(runtime.execute(a.buffer, b.buffer, c.buffer).status == Status::Ok,
              "resource-based execution after re-init must work");
    }

    // --- 14. Final accounting: nothing may be left alive ---------------------
    runtime.shutdown();
    check(manager.stats().live_buffers == 0, "no buffers may survive full shutdown (no leaks)");

    if (failures == 0) {
        std::cout << "Resource tests passed.\n";
        return 0;
    }
    return 1;
}
