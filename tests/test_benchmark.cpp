// Benchmark tests (Phase 8) — CPU path.
//
// These tests MUST pass on every system, including machines without any GPU
// and CPU-only builds. They verify the Phase 8 benchmark module's
// INVARIANTS — never timing values (a timing assertion like
// "elapsed < 10ms" would be flaky by design and is deliberately absent):
//
//   config/task validation -> Real-path execution on the CPU backend ->
//   statistics invariants (min <= median-ish <= max, count == requested) ->
//   correctness verdict -> throughput consistency -> repeated runs ->
//   backend honesty (never silently switched) -> warmup/failure
//   propagation -> the pure statistics algorithm (hand-computed values) ->
//   TaskQueue interference check.
//
// Design rules honored here:
//   - No timing-based flakiness: only structural invariants over real
//     measurements (e.g. min <= average <= max, iterations == requested).
//   - No hardware assumptions: every Vulkan-dependent expectation adapts to
//     the real availability probed through the same Virtual GPU.
//   - The benchmark itself must run the REAL path: verified structurally
//     (the API takes a VirtualGpu and returns its backend/device) and by
//     result consistency against independent executions of the same GPU.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "core/benchmark/benchmark.hpp"
#include "core/compute/task.hpp"
#include "core/device/device.hpp"
#include "core/queue/task_queue.hpp"
#include "core/vgpu/virtual_gpu.hpp"

using vortyx::benchmark::BenchmarkConfig;
using vortyx::benchmark::BenchmarkResult;
using vortyx::benchmark::TimingStats;
using vortyx::benchmark::benchmark_vector_add;
using vortyx::benchmark::compute_timing_stats;
using vortyx::benchmark::describe;
using vortyx::benchmark::to_key_values;
using vortyx::compute::Status;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;
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
        task.a[i] = static_cast<std::int32_t>(i % 1000) - 300;
        task.b[i] = static_cast<std::int32_t>((i * 7) % 500) + 11;
    }
    return task;
}

bool data_matches(const std::vector<std::int32_t>& data, const VectorAddTask& task) {
    if (data.size() != task.a.size()) return false;
    for (std::size_t i = 0; i < task.a.size(); ++i) {
        if (data[i] != task.a[i] + task.b[i]) return false;
    }
    return true;
}

// Shared invariant checks for a successful benchmark result. The expected
// backend is caller-supplied (this helper is used for cpu AND vulkan runs).
void check_success_invariants(const BenchmarkResult& r, const VectorAddTask& task,
                              std::uint32_t requested_iterations,
                              const std::string& expected_backend,
                              const std::string& context) {
    check(r.status == Status::Ok, context + ": status is Ok");
    check(r.error.empty(), context + ": no error on success");
    check(r.backend == expected_backend, context + ": backend is the measured target");
    check(r.element_count == task.a.size(), context + ": element count matches the task");
    check(r.iterations == requested_iterations,
          context + ": measured iteration count matches the request");
    check(r.correctness_verified, context + ": correctness verdict present (all iterations verified)");

    // Timing invariants (structural, never specific values).
    check(r.timing.min >= std::chrono::nanoseconds{0}, context + ": min sample is non-negative");
    check(r.timing.max >= r.timing.min, context + ": max >= min");
    check(r.timing.average_ns >= static_cast<double>(r.timing.min.count()),
          context + ": average >= min");
    check(r.timing.average_ns <= static_cast<double>(r.timing.max.count()),
          context + ": average <= max");
    check(r.timing.median_ns >= static_cast<double>(r.timing.min.count()),
          context + ": median >= min");
    check(r.timing.median_ns <= static_cast<double>(r.timing.max.count()),
          context + ": median <= max");
    check(r.timing.stddev_ns >= 0.0, context + ": stddev is non-negative");
    check(r.timing.max.count() > 0, context + ": samples are real time, not zeros");

    // Throughput consistency with the reported average.
    const double expected_throughput =
        static_cast<double>(task.a.size()) / (r.timing.average_ns * 1e-9);
    check(std::fabs(expected_throughput - r.timing.throughput_elements_per_second) <=
              1e-6 * expected_throughput,
          context + ": throughput consistent with element count / average time");
}

}  // namespace

int main() {
    // =====================================================================
    // 1. Config and task validation: nothing executes, nothing is faked.
    // =====================================================================
    {
        VirtualGpu gpu;  // backend defaults to "cpu"
        check(gpu.initialize() == Status::Ok, "1: CPU Virtual GPU initializes");

        const VectorAddTask task = make_task(64);

        BenchmarkConfig zero_iterations;
        zero_iterations.iterations = 0;
        zero_iterations.warmup_iterations = 0;
        BenchmarkResult r = benchmark_vector_add(gpu, task, zero_iterations);
        check(r.status == Status::InvalidInput, "1a: zero measured iterations refused");
        check(!r.error.empty(), "1a: refusal carries a reason");
        check(r.iterations == 0, "1a: nothing was measured");

        VectorAddTask mismatch = task;
        mismatch.b.pop_back();
        r = benchmark_vector_add(gpu, mismatch, BenchmarkConfig{});
        check(r.status == Status::InvalidInput, "1b: size-mismatched task refused");
        check(!r.error.empty(), "1b: refusal carries a reason");

        VectorAddTask empty;
        r = benchmark_vector_add(gpu, empty, BenchmarkConfig{});
        check(r.status == Status::InvalidInput, "1c: empty task refused");
        check(!r.error.empty(), "1c: refusal carries a reason");

        gpu.shutdown();

        // A non-Ready Virtual GPU is refused, never benchmarked.
        r = benchmark_vector_add(gpu, task, BenchmarkConfig{});
        check(r.status == Status::NotInitialized, "1d: non-Ready Virtual GPU refused");
        check(!r.error.empty(), "1d: refusal carries a reason");
    }

    // =====================================================================
    // 2. Valid CPU benchmark: real path, real statistics, verified result.
    // =====================================================================
    {
        VirtualGpu gpu;
        check(gpu.initialize() == Status::Ok, "2: CPU Virtual GPU initializes");

        const VectorAddTask task = make_task(1024);
        BenchmarkConfig config;
        config.iterations = 7;
        config.warmup_iterations = 2;

        const BenchmarkResult r = benchmark_vector_add(gpu, task, config);
        check_success_invariants(r, task, 7, "cpu", "2a");

        // The measured workload is the REAL execution path: an independent
        // execution through the same Virtual GPU must agree bit-exactly
        // with what the benchmark verified.
        const VectorAddResult independent = gpu.execute(task);
        check(independent.status == Status::Ok && data_matches(independent.data, task),
              "2b: independent execution agrees with the benchmarked path");

        // Machine-readable export carries the same real values.
        const auto kv = to_key_values(r);
        bool has_backend = false, has_min = false, has_correctness = false;
        for (const auto& pair : kv) {
            if (pair.first == "backend" && pair.second == "cpu") has_backend = true;
            if (pair.first == "min_ns") has_min = true;
            if (pair.first == "correctness_verified" && pair.second == "true") has_correctness = true;
        }
        check(has_backend, "2c: export names the measured backend");
        check(has_min, "2c: export carries the min timing in ns");
        check(has_correctness, "2c: export carries the correctness verdict");

        // Human-readable form renders without crashing and names the target.
        const std::string text = describe(r);
        check(text.find("cpu") != std::string::npos, "2d: description names the backend");
        check(text.find("PASS") != std::string::npos, "2d: description carries the verdict");

        gpu.shutdown();
    }

    // =====================================================================
    // 3. Repeated benchmarks: deterministic verdict, independent timings.
    //    (Timing VALUES are never compared — only the correctness and the
    //    structural invariants are required to reproduce.)
    // =====================================================================
    {
        VirtualGpu gpu;
        check(gpu.initialize() == Status::Ok, "3: CPU Virtual GPU initializes");

        const VectorAddTask task = make_task(256);
        BenchmarkConfig config;
        config.iterations = 5;
        config.warmup_iterations = 1;

        const BenchmarkResult first = benchmark_vector_add(gpu, task, config);
        const BenchmarkResult second = benchmark_vector_add(gpu, task, config);
        check_success_invariants(first, task, 5, "cpu", "3a(first)");
        check_success_invariants(second, task, 5, "cpu", "3a(second)");

        // The workload definition is deterministic: same task, same verdict.
        check(first.correctness_verified == second.correctness_verified,
              "3b: correctness verdict is deterministic");

        // Zero warmup is allowed by policy and still verified honestly.
        BenchmarkConfig no_warmup;
        no_warmup.iterations = 3;
        no_warmup.warmup_iterations = 0;
        const BenchmarkResult r = benchmark_vector_add(gpu, task, no_warmup);
        check_success_invariants(r, task, 3, "cpu", "3c(zero warmup)");
        check(r.warmup_iterations == 0, "3c: zero warmup recorded honestly");

        gpu.shutdown();
    }

    // =====================================================================
    // 4. Backend honesty: a benchmark on an unavailable backend FAILS with
    //    that backend's real error and is never rerouted to "cpu".
    //    (Adaptive: with a real Vulkan device the same request succeeds.)
    // =====================================================================
    {
        VirtualGpuDesc desc;
        desc.backend = "vulkan";
        VirtualGpu gpu;
        check(gpu.initialize(desc) == Status::Ok, "4: vulkan Virtual GPU initializes (known backend)");

        const VectorAddTask task = make_task(64);
        BenchmarkConfig config;
        config.iterations = 3;
        config.warmup_iterations = 1;

        const BenchmarkResult r = benchmark_vector_add(gpu, task, config);
        if (!gpu.backend_available()) {
            check(r.status == Status::BackendUnavailable,
                  "4a: unavailable backend fails the benchmark honestly");
            check(r.backend == "vulkan",
                  "4a: the failed run still reports the REQUESTED backend (no silent switch)");
            check(!r.error.empty(), "4a: the real reason is carried");
            check(!r.correctness_verified, "4a: nothing was verified");
            // The failure must surface from the WARMUP phase (the config
            // requests one warmup iteration before any measurement).
            check(r.error.find("warmup") != std::string::npos,
                  "4a: warmup failure is named as the abort point");
        } else {
            // Real Vulkan device present: the benchmark must succeed on it
            // and report the vulkan target (the GPU-path test pins this in
            // depth; here we only keep the adaptive expectation honest).
            check_success_invariants(r, task, 3, "vulkan", "4b(real device)");
        }
        gpu.shutdown();
    }

    // =====================================================================
    // 5. Pure statistics algorithm: hand-computed, deterministic values
    //    over synthetic samples (no clocks, no flakiness possible).
    // =====================================================================
    {
        TimingStats stats;
        std::string error;

        // Empty input is refused — statistics over nothing would be fake.
        check(!compute_timing_stats({}, 100, stats, error), "5a: empty samples refused");
        check(!error.empty(), "5a: refusal carries a reason");

        std::vector<std::chrono::nanoseconds> samples = {
            std::chrono::nanoseconds{100}, std::chrono::nanoseconds{200},
            std::chrono::nanoseconds{300}, std::chrono::nanoseconds{400}};
        check(compute_timing_stats(samples, 1000, stats, error), "5b: four samples summarized");
        check(stats.min == std::chrono::nanoseconds{100}, "5b: min is exact");
        check(stats.max == std::chrono::nanoseconds{400}, "5b: max is exact");
        check(std::fabs(stats.average_ns - 250.0) < 1e-9, "5b: average is exact");
        check(std::fabs(stats.median_ns - 250.0) < 1e-9, "5b: even-count median is mean of middle two");
        // Population stddev of {100,200,300,400} = sqrt(12500) = 111.803...
        check(std::fabs(stats.stddev_ns - 111.80339887498948) < 1e-6, "5b: stddev is exact");
        // 1000 elements / 250ns = 4e9 elements per second.
        check(std::fabs(stats.throughput_elements_per_second - 4e9) < 1e-3,
              "5b: throughput is exact");

        samples = {std::chrono::nanoseconds{10}, std::chrono::nanoseconds{20},
                   std::chrono::nanoseconds{30}};
        check(compute_timing_stats(samples, 10, stats, error), "5c: three samples summarized");
        check(std::fabs(stats.median_ns - 20.0) < 1e-9, "5c: odd-count median is the middle sample");
        check(std::fabs(stats.stddev_ns - 8.16496580927726) < 1e-6, "5c: stddev is exact");

        samples = {std::chrono::nanoseconds{50}};
        check(compute_timing_stats(samples, 10, stats, error), "5d: single sample summarized");
        check(stats.min == std::chrono::nanoseconds{50} && stats.max == std::chrono::nanoseconds{50},
              "5d: single sample min == max");
        check(std::fabs(stats.average_ns - 50.0) < 1e-9, "5d: single sample average");
        check(std::fabs(stats.median_ns - 50.0) < 1e-9, "5d: single sample median");
        check(stats.stddev_ns == 0.0, "5d: identical samples have zero spread");

        check(!compute_timing_stats({std::chrono::nanoseconds{10}}, 0, stats, error),
              "5e: zero element count refused (throughput undefined)");
    }

    // =====================================================================
    // 6. Integration: benchmarking one Virtual GPU leaves the TaskQueue
    //    (and its FIFO/single-worker semantics) untouched.
    // =====================================================================
    {
        VirtualGpu queue_gpu;  // explicit cpu
        check(queue_gpu.initialize() == Status::Ok, "6: queue Virtual GPU initializes");
        vortyx::queue::TaskQueue queue;
        check(queue.initialize(queue_gpu) == Status::Ok, "6: TaskQueue initializes");

        VirtualGpu bench_gpu;  // a DIFFERENT Virtual GPU for the benchmark
        check(bench_gpu.initialize() == Status::Ok, "6: benchmark Virtual GPU initializes");

        const VectorAddTask task = make_task(512);
        const BenchmarkResult r = benchmark_vector_add(bench_gpu, task, BenchmarkConfig{});
        check_success_invariants(r, task, 10, "cpu", "6a");

        // The queue still executes its own tasks FIFO with correct results
        // after the benchmark ran elsewhere.
        VectorAddTask queued;
        queued.a = {1, 2, 3, 4};
        queued.b = {10, 20, 30, 40};
        const vortyx::queue::EnqueueResult er = queue.enqueue(queued);
        check(er.status == Status::Ok, "6b: enqueue after benchmark succeeds");
        check(queue.wait(er.id) == vortyx::queue::TaskState::Completed, "6b: queued task completes");
        const vortyx::queue::TaskSnapshot snap = queue.task_snapshot(er.id);
        check(snap.result.status == Status::Ok && snap.result.data.size() == 4 &&
                  snap.result.data[0] == 11 && snap.result.data[3] == 44,
              "6b: queued task result is still correct");

        // Shutdown order (Phase 6 contract) is unchanged.
        queue.shutdown();
        queue_gpu.shutdown();
        bench_gpu.shutdown();
    }

    if (failures == 0) {
        std::cout << "Benchmark CPU-path tests passed.\n";
        return 0;
    }
    std::cerr << failures << " benchmark CPU-path check(s) FAILED.\n";
    return 1;
}
