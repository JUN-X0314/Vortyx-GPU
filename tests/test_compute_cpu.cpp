// CPU compute path tests (Phase 3).
//
// These tests MUST pass on every system, including machines without any
// GPU and builds without the Vulkan backend compiled in.

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "core/compute/task.hpp"
#include "core/compute/runtime.hpp"
#include "core/device/device.hpp"

using vortyx::compute::Runtime;
using vortyx::compute::Status;
using vortyx::compute::to_string;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;

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
    // --- Runtime lifecycle --------------------------------------------------
    {
        Runtime runtime;
        check(!runtime.is_initialized(), "fresh Runtime must not be initialized");

        // Executing before initialize() must return a clear error, not crash.
        const VectorAddTask task = make_task(4);
        const VectorAddResult early = runtime.execute(task, "cpu");
        check(early.status == Status::NotInitialized,
              "execute() before initialize() must report NotInitialized");

        check(runtime.initialize() == Status::Ok, "initialize() must succeed");
        check(runtime.is_initialized(), "initialize() must set the initialized flag");

        // Registered backends: cpu always; vulkan compiled when enabled.
        const std::vector<std::string> names = runtime.backend_names();
        const bool has_cpu = std::find(names.begin(), names.end(), "cpu") != names.end();
        check(has_cpu, "cpu backend must always be registered");
        check(runtime.has_backend("cpu"), "cpu backend must be available everywhere");

        // --- CPU vector addition correctness on several sizes ---------------
        const std::vector<std::size_t> sizes = {4, 16, 1024, 10007};
        for (std::size_t size : sizes) {
            const VectorAddTask task2 = make_task(size);
            const VectorAddResult result = runtime.execute(task2, "cpu");
            check(result.status == Status::Ok,
                  "cpu execution must succeed for size " + std::to_string(size));
            check(result_matches(result, task2),
                  "cpu result must equal A+B for size " + std::to_string(size));
        }

        // Default execute() runs on cpu as well.
        const VectorAddTask task3 = make_task(64);
        const VectorAddResult default_result = runtime.execute(task3);
        check(default_result.status == Status::Ok,
              "default execute() must work");
        check(default_result.data == runtime.execute(task3, "cpu").data,
              "default execute() must match the cpu backend result");

        // --- Invalid inputs --------------------------------------------------
        {
            VectorAddTask mismatch;
            mismatch.a = {1, 2, 3};
            mismatch.b = {1, 2};
            const VectorAddResult r = runtime.execute(mismatch, "cpu");
            check(r.status == Status::InvalidInput,
                  "mismatched input sizes must return InvalidInput");
            check(!r.error.empty(), "InvalidInput must carry an error message");
        }
        {
            VectorAddTask empty;
            const VectorAddResult r = runtime.execute(empty, "cpu");
            check(r.status == Status::InvalidInput,
                  "empty input must return InvalidInput (documented decision)");
        }

        // --- Unsupported/unavailable backends --------------------------------
        {
            const VectorAddTask task4 = make_task(8);
            const VectorAddResult unknown = runtime.execute(task4, "does-not-exist");
            check(unknown.status == Status::BackendUnavailable,
                  "unknown backend name must return BackendUnavailable");
            check(!unknown.error.empty(), "unknown backend error must be descriptive");

            // Any registered-but-unavailable backend must also fail cleanly.
            if (!runtime.has_backend("vulkan")) {
                const VectorAddResult r = runtime.execute(task4, "vulkan");
                check(r.status == Status::BackendUnavailable,
                      "unavailable vulkan backend must return BackendUnavailable");
                check(!r.error.empty(),
                      "unavailable backend must report a reason");
                check(!runtime.backend_unavailable_reason("vulkan").empty(),
                      "unavailable reason must be exposed by the runtime");
            }
        }

        // --- Device info ------------------------------------------------------
        {
            const vortyx::device::DeviceInfo info = runtime.backend_device("cpu");
            check(info.type == vortyx::device::DeviceType::Cpu,
                  "cpu backend device info must be typed Cpu");
        }

        // --- Shutdown + re-initialization (resource cleanup path) -------------
        runtime.shutdown();
        check(!runtime.is_initialized(), "shutdown() must clear the initialized flag");

        const VectorAddTask task5 = make_task(8);
        const VectorAddResult after = runtime.execute(task5, "cpu");
        check(after.status == Status::NotInitialized,
              "execute() after shutdown() must report NotInitialized");

        // Re-initialize and execute again: exercises full create/destroy/create.
        check(runtime.initialize() == Status::Ok, "re-initialize after shutdown must work");
        const VectorAddResult again = runtime.execute(make_task(1024), "cpu");
        check(again.status == Status::Ok, "cpu execution after re-init must work");
        runtime.shutdown();
    }

    // Second full Runtime lifecycle in the same process.
    {
        Runtime runtime;
        check(runtime.initialize() == Status::Ok, "second Runtime instance must initialize");
        const VectorAddResult result = runtime.execute(make_task(500), "cpu");
        check(result.status == Status::Ok, "second Runtime instance must execute");
        runtime.shutdown();
    }

    if (failures == 0) {
        std::cout << "CPU compute tests passed.\n";
        return 0;
    }
    return 1;
}
