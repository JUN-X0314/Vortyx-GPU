#pragma once

// Benchmark (Phase 8).
//
// The measurement layer for Vortyx compute execution. Phase 8 is about
// MEASURABILITY and OBSERVABILITY, not performance optimization: the
// benchmark answers "how long does this workload actually take on THIS
// execution target, and is the result correct?" — nothing more.
//
//   Benchmark -> Virtual GPU -> Compute Runtime -> Resource Manager -> Backend
//
// The one hard rule: the benchmark NEVER computes the workload itself. It
// does not contain a vector-addition loop, does not touch backends, does not
// allocate device memory, and does not replicate any Runtime logic. It times
// repeated calls to the UNCHANGED VirtualGpu::execute() path — the exact
// path an application uses (Virtual GPU -> Runtime -> Resource Manager ->
// Backend) — and turns the raw wall-clock samples into statistics.
//
// What is measured (documented scope, no hidden boundaries):
//   - One sample = one VirtualGpu::execute(task) call, timed with
//     std::chrono::steady_clock around the call (monotonic; no sleeps, no
//     hardcoded expectations, no single-shot measurements).
//   - The task-based execution path translates a task into buffer resources
//     per call (allocate -> upload -> dispatch -> download -> release), so
//     every sample INCLUDES allocation, host->device transfer, execution,
//     device->host readback and teardown, for both backends. That end-to-end
//     scope is IDENTICAL for "cpu" and "vulkan" (the Runtime performs the
//     same translation for both), which keeps cross-backend comparisons
//     meaningful. It is CPU wall-clock time — the current Runtime API does
//     not expose GPU-internal execution boundaries, so no number here is
//     ever labeled "GPU time".
//   - Setup outside the timed region: input construction, Virtual GPU
//     initialization, the host-side reference used for correctness, and all
//     result verification happen outside the measured window.
//
// Correctness comes before performance:
//   - Every iteration's output (warmup AND measured) is verified against a
//     host-computed reference (C[i] == A[i] + B[i]) OUTSIDE the timed
//     window. A benchmark whose results are wrong is reported as failed —
//     it is never reported as a fast success.
//   - A failing iteration fails the whole benchmark with its real Status and
//     message. Successful iterations are never cherry-picked around a
//     failure.
//   - Warmup iterations are executed first and excluded from all statistics;
//     a warmup failure also fails the benchmark (reported as such in the
//     error string).
//
// Determinism (what is and is not deterministic):
//   - Deterministic: the workload definition (the caller's task), the
//     iteration/warmup counts, which backend is measured (the one the given
//     Virtual GPU was configured with — the benchmark cannot and will not
//     switch it), the statistics algorithm, the correctness verdict, and
//     the output schema (describe()/to_key_values()).
//   - NOT deterministic: the timing numbers themselves. They vary with the
//     machine and system state — that is what measurement means. No test or
//     tool may assert a specific timing value (see tests/test_benchmark.cpp:
//     invariants only, e.g. min <= average <= max).
//
// No performance claims: the module reports measured numbers for the given
// run. It never claims one backend is faster than another, never scores or
// ranks hardware, and never feeds its results back into the Phase 7
// Scheduler (whose policy stays a fixed availability rule: vulkan > cpu).
// Connecting measurements to scheduling decisions is future work that this
// phase deliberately does not implement.
//
// Ownership and lifetime:
//   - The benchmark owns NOTHING and has NO lifecycle: it is a set of pure
//     functions plus value-type results. The caller owns the Virtual GPU and
//     must keep it alive (and not shut it down) while benchmark_vector_add()
//     runs — the same "caller owns the execution context" rule the TaskQueue
//     documents. The benchmark never initializes, shuts down or reconfigures
//     the Virtual GPU it is given.
//   - The caller also owns the task inputs; they are read only. Benchmark
//     inputs and outputs are value types (RAII vectors), so no benchmark
//     resource can leak past the call.
//
// Threading:
//   - benchmark_vector_add() is a pure function of its arguments and the
//     real execution state; two benchmarks may run concurrently on two
//     DIFFERENT Virtual GPUs (each Virtual GPU is single-threaded by its own
//     contract). One Virtual GPU must not be executed from two threads at
//     once — that is the Virtual GPU's existing rule, not a new one.
//
// Error handling follows the project-wide result style (Phase 3): no
// exceptions thrown by this module, explicit Status values with
// human-readable 'error' strings, no new status vocabulary (the Phase 3
// Status enum covers every failure here: InvalidInput for bad
// configs/tasks, NotInitialized for a non-Ready Virtual GPU,
// BackendUnavailable for an unusable backend, BackendError for execution or
// correctness failures).

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/compute/task.hpp"
#include "core/device/device.hpp"

// Forward declaration: the benchmark times an application-owned Virtual GPU.
// The complete type is required only in the .cpp, keeping header
// dependencies one-directional (benchmark -> vgpu stays out of this header).
namespace vortyx::vgpu {
class VirtualGpu;
}

namespace vortyx::benchmark {

// The benchmark layer speaks the project-wide result vocabulary from Phase 3
// (one unified error model, no second status system).
using vortyx::compute::Status;
using vortyx::compute::VectorAddResult;
using vortyx::compute::VectorAddTask;
// Phase 10 (Compute Engine): the generic task vocabulary for the generic
// benchmark entry point.
using vortyx::compute::ComputeTask;
using vortyx::compute::ComputeTaskResult;
using vortyx::compute::ComputeOp;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// What and how often to measure. The WORKLOAD is the caller's task (its
// element count is the workload size — construct the task with the size you
// want measured); the config only controls the measurement repetition. There
// is deliberately no "workload size" field here that could disagree with the
// actual task. Phase 10 accepts any generic ComputeTask (see
// benchmark_compute); benchmark_vector_add remains the unchanged Phase 8
// entry point for VectorAddTask workloads.
struct BenchmarkConfig {
    // Measured iterations. Every iteration is a real execute() timed with
    // steady_clock and verified for correctness. Must be > 0 (a benchmark
    // with zero measurements is refused with Status::InvalidInput — it could
    // only produce fake statistics).
    std::uint32_t iterations = 10;

    // Warmup iterations executed BEFORE the measured ones (initialization
    // cost, caches, first-touch allocation). Warmup results are verified for
    // correctness but are NEVER included in the statistics. 0 is allowed
    // (policy: warmup is optional; the first measured iteration then carries
    // any one-time cost, honestly).
    std::uint32_t warmup_iterations = 1;
};

// ---------------------------------------------------------------------------
// Statistics over real samples
// ---------------------------------------------------------------------------

// Aggregate timing statistics over the measured samples. Every field is
// derived from real steady_clock samples by compute_timing_stats(); there is
// no estimated or fabricated number here. Units are part of the names:
// integer samples are nanoseconds, aggregate *_ns fields are nanoseconds as
// double, throughput is elements per second.
struct TimingStats {
    // Fastest and slowest measured iteration (warmup excluded).
    std::chrono::nanoseconds min{0};
    std::chrono::nanoseconds max{0};

    // Arithmetic mean of the samples in nanoseconds (double accumulation to
    // avoid integer overflow; individual samples are nanoseconds, so double
    // precision is ample for any realistic iteration count).
    double average_ns = 0.0;

    // Median: middle sample for an odd count, mean of the two middle
    // samples for an even count (samples sorted; the algorithm in
    // compute_timing_stats is deterministic).
    double median_ns = 0.0;

    // Population standard deviation of the samples (nanoseconds). 0 for
    // identical samples; a crude but honest spread indicator without
    // pulling in a statistics library.
    double stddev_ns = 0.0;

    // element_count / (average_ns * 1e-9): mean elements processed per
    // second across the measured iterations.
    double throughput_elements_per_second = 0.0;
};

// Pure statistics over REAL samples — no I/O, no clock access, no global
// state, so tests can pin the algorithm with hand-computed values without
// any timing flakiness.
//
//   samples        measured iterations (steady_clock durations, warmup
//                  already excluded by the caller); must be non-empty
//   element_count  workload size used for throughput (> 0)
//
// Returns false (and fills 'error') for an empty sample list or a zero
// element count — computing statistics over nothing would be fabrication.
// On success fills 'out'. Deterministic: the same samples always produce
// the same stats.
bool compute_timing_stats(const std::vector<std::chrono::nanoseconds>& samples,
                          std::size_t element_count,
                          TimingStats& out,
                          std::string& error);

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

// The complete, unit-annotated record of ONE benchmark run. This is the
// machine-readable form (consume the fields directly); describe() renders it
// for humans and to_key_values() exports a stable key=value schema for
// tooling — no unit-less numbers anywhere.
//
// Fields 'workload' / 'backend' / 'device' / 'element_count' /
// 'warmup_iterations' / 'iterations' describe WHAT was measured (always
// filled, even on failure — they are known before the first timed call). The
// timing statistics and 'correctness_verified' are meaningful only when
// status == Status::Ok; on failure they stay zero/false and 'error' explains
// why.
struct BenchmarkResult {
    Status status = Status::Ok;
    std::string error;  // empty when status == Ok

    // WHAT was measured: the stable workload label of the operation
    // ("vector_add", "vector_multiply", "vector_scale" — see
    // compute::workload_label). Phase 8 runs all said "vector_add"; the key
    // set of the exporter is unchanged, only the label can differ now.
    std::string workload = "vector_add";

    // The measured execution target: exactly the configured backend of the
    // Virtual GPU the benchmark was given (a backend name, e.g. "cpu" or
    // "vulkan"). The benchmark never switches backends — a failed run on an
    // unavailable backend still reports THAT backend here, never a fallback.
    std::string backend;

    // The concrete device behind 'backend' (the Virtual GPU's own
    // DeviceInfo, never fabricated). May be a default (Unknown) DeviceInfo
    // when the backend cannot report a device (e.g. unavailable Vulkan).
    vortyx::device::DeviceInfo device;

    // Workload size: element count of the measured task. For every current
    // op one element is one int32 element processed end to end.
    std::size_t element_count = 0;

    // Warmup iterations requested (executed first, excluded from stats).
    std::uint32_t warmup_iterations = 0;

    // Measured iterations COMPLETED when the benchmark ended. == the
    // requested count on success; on failure it is the number of measured
    // iterations that ran before the failure (they still happened — the
    // count is never inflated).
    std::uint32_t iterations = 0;

    // Statistics over the measured iterations (meaningful only on success).
    TimingStats timing{};

    // True only when status == Ok AND every executed iteration (warmup and
    // measured) produced bit-exact expected output. A run that never
    // verified (it failed earlier) reports false — verification is never
    // assumed.
    bool correctness_verified = false;
};

// ---------------------------------------------------------------------------
// The benchmark itself
// ---------------------------------------------------------------------------

// Benchmarks ONE VectorAddTask on the given, caller-owned Virtual GPU by
// executing the REAL path (Virtual GPU -> Runtime -> Resource Manager ->
// Backend) 'config.warmup_iterations + config.iterations' times. This is the
// unchanged Phase 8 entry point; it adapts the task into the generic engine
// and measures the same single path benchmark_compute() measures.
//
// Sequence:
//   1. Validate the config (iterations > 0) and the task
//      (validate_vector_add: non-empty, equal sizes) — Status::InvalidInput
//      otherwise, before anything executes.
//   2. Require a Ready Virtual GPU — Status::NotInitialized otherwise.
//   3. Record the target: backend = gpu.backend_name(), device =
//      gpu.device_info() (the honest target even if it later fails).
//   4. Warmup: execute config.warmup_iterations times, verifying each
//      result. A warmup failure ends the benchmark with that real Status
//      and an error naming the warmup iteration — never a "success".
//   5. Measure: execute config.iterations times, each call bracketed by
//      steady_clock::now() samples; verify each result outside the timed
//      window; a failed or wrong iteration ends the benchmark with the real
//      Status (BackendError for a wrong result) and the iteration index.
//   6. Summarize with compute_timing_stats.
//
// Never throws, never falls back to another backend, never modifies the
// Virtual GPU's configuration, never shuts it down. Returns by value; all
// memory is RAII.
BenchmarkResult benchmark_vector_add(vortyx::vgpu::VirtualGpu& gpu,
                                     const VectorAddTask& task,
                                     const BenchmarkConfig& config);

// Generic benchmark (Phase 10): the same measurement discipline as
// benchmark_vector_add, for any ComputeOp the engine supports (VectorAdd /
// VectorMultiply / VectorScale).
//   - STILL measures ONLY the real VirtualGpu::execute() path: the module
//     contains no compute kernel of its own. The host-side reference below
//     is verification, never a timed or reported computation.
//   - Workload label: the operation's stable label ("vector_multiply", ...)
//     in the result and exporter — comparisons across DIFFERENT operations
//     stay semantically labeled, never collapsed into one number.
//   - Throughput definition (unchanged semantics): element_count /
//     average_ns — one element is one int32 element processed end to end
//     for every current op, so the number means the same thing per op.
//   - Correctness: every iteration (warmup AND measured) is verified
//     against the op's host-computed reference outside the timed window;
//     a wrong iteration fails the whole benchmark. Integer results are
//     bit-exact, so the verdict is exact, not tolerance-based.
BenchmarkResult benchmark_compute(vortyx::vgpu::VirtualGpu& gpu,
                                  const ComputeTask& task,
                                  const BenchmarkConfig& config);

// ---------------------------------------------------------------------------
// Output forms
// ---------------------------------------------------------------------------

// Human-readable multi-line description of a result, e.g.:
//   "Benchmark 'vector_add' on backend 'cpu' (device: ...): 1024 elements,
//    10 iterations (1 warmup, excluded): min 12.3 us | avg 13.4 us |
//    median 13.2 us | max 18.9 us | stddev 1.1 us |
//    throughput 76.4 Melem/s | correctness: PASS"
// Timings are rendered with human units (ns/us/ms) derived from the real
// nanosecond values; throughput with an SI-ish scale suffix. The exact
// layout may evolve; values are always real measurements.
std::string describe(const BenchmarkResult& result);

// Machine-readable stable key=value pairs (no external serialization
// dependency; keys are stable across this version). Numeric fields carry
// their unit in the key ('_ns', '_elements_per_second'); timings are
// reported in raw nanoseconds; doubles use fixed-point notation. The same
// status/error vocabulary as the struct. Consumers (tools, logs, future
// exporters) can rely on the key set: status, error, backend, device_type,
// device_name, element_count, warmup_iterations, iterations, min_ns,
// average_ns, median_ns, max_ns, stddev_ns, throughput_elements_per_second,
// correctness_verified.
std::vector<std::pair<std::string, std::string>> to_key_values(const BenchmarkResult& result);

}  // namespace vortyx::benchmark
