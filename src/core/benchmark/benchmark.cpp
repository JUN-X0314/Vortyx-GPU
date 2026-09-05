// Benchmark implementation (Phase 8).
//
// What lives here (and nowhere else):
//   - The measurement loop around VirtualGpu::execute() — the ONLY timed
//     thing in this module. There is deliberately no vector-addition code
//     path here that could produce a "result" without going through the
//     Virtual GPU: the only arithmetic in this file is the HOST-SIDE
//     reference used to VERIFY correctness (task doc: correctness before
//     performance), and it is never timed or reported as performance.
//   - The deterministic statistics algorithm (compute_timing_stats), pure
//     and unit-testable without hardware.
//   - The two output renderers (describe / to_key_values) over real fields.
//
// Timing discipline:
//   - std::chrono::steady_clock (monotonic, the project's C++17 standard
//     library) around exactly one execute() call per sample. No sleeps, no
//     fixed delays, no hardcoded expectations, no single-shot numbers.
//   - Verification of an iteration's output happens AFTER its sample is
//     taken, so verification cost never pollutes the timing.
//   - A failed execute() still records that the iteration happened (the
//     iterations counter is incremented for the attempt) but the whole
//     benchmark fails with the real Status — partial statistics are never
//     presented as a success.

#include "core/benchmark/benchmark.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>

#include "core/logger.hpp"
#include "core/vgpu/virtual_gpu.hpp"

namespace vortyx::benchmark {

// ---------------------------------------------------------------------------
// Statistics (pure function)
// ---------------------------------------------------------------------------

bool compute_timing_stats(const std::vector<std::chrono::nanoseconds>& samples,
                          std::size_t element_count,
                          TimingStats& out,
                          std::string& error) {
    out = TimingStats{};
    error.clear();

    if (samples.empty()) {
        error = "no timing samples to summarize (a benchmark over zero "
                "measurements has no honest statistics)";
        return false;
    }
    if (element_count == 0) {
        error = "element count is zero; throughput is undefined";
        return false;
    }

    // double accumulation: nanoseconds-scale integers summed over any
    // realistic iteration count stay far inside double's exact integer
    // range (2^53), so the mean cannot lose precision to overflow.
    std::vector<double> values;
    values.reserve(samples.size());
    for (const std::chrono::nanoseconds& sample : samples) {
        values.push_back(static_cast<double>(sample.count()));
    }

    double min_value = values.front();
    double max_value = values.front();
    double sum = 0.0;
    for (const double value : values) {
        min_value = std::min(min_value, value);
        max_value = std::max(max_value, value);
        sum += value;
    }
    const double count = static_cast<double>(values.size());
    const double average = sum / count;

    // Median over a sorted copy — deterministic and independent of the
    // input order. Odd count: the middle sample; even count: the mean of
    // the two middle samples.
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t n = sorted.size();
    const double median = (n % 2 == 1)
                              ? sorted[n / 2]
                              : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;

    // Population standard deviation (no external statistics dependency).
    double squared_diff = 0.0;
    for (const double value : values) {
        const double diff = value - average;
        squared_diff += diff * diff;
    }
    const double stddev = std::sqrt(squared_diff / count);

    // Throughput: elements per second over the mean iteration time. The
    // average is in nanoseconds, hence the 1e-9.
    const double throughput =
        static_cast<double>(element_count) / (average * 1e-9);

    // min/max are exact sample values, so the round-trip through double is
    // lossless; the aggregate fields stay double by design.
    out.min = std::chrono::nanoseconds(static_cast<std::int64_t>(min_value));
    out.max = std::chrono::nanoseconds(static_cast<std::int64_t>(max_value));
    out.average_ns = average;
    out.median_ns = median;
    out.stddev_ns = stddev;
    out.throughput_elements_per_second = throughput;
    return true;
}

// ---------------------------------------------------------------------------
// Output helpers (declared before the benchmark so the run itself can log
// through the same human-readable formatting it reports with)
// ---------------------------------------------------------------------------

namespace {

// Renders a nanosecond value with a human unit (ns / us / ms / s). The value
// is always the real measurement, only the unit is scaled.
std::string format_duration_ns(double ns) {
    std::ostringstream os;
    if (ns >= 1e9) {
        os << std::fixed << std::setprecision(3) << (ns / 1e9) << " s";
    } else if (ns >= 1e6) {
        os << std::fixed << std::setprecision(3) << (ns / 1e6) << " ms";
    } else if (ns >= 1e3) {
        os << std::fixed << std::setprecision(3) << (ns / 1e3) << " us";
    } else {
        os << std::fixed << std::setprecision(0) << ns << " ns";
    }
    return os.str();
}

// Renders elements/second with an SI-ish scale suffix (the unit stays
// "elements per second"; only the magnitude is factored out).
std::string format_throughput(double elements_per_second) {
    std::ostringstream os;
    if (elements_per_second >= 1e9) {
        os << std::fixed << std::setprecision(2) << (elements_per_second / 1e9)
           << " Gelem/s";
    } else if (elements_per_second >= 1e6) {
        os << std::fixed << std::setprecision(2) << (elements_per_second / 1e6)
           << " Melem/s";
    } else if (elements_per_second >= 1e3) {
        os << std::fixed << std::setprecision(2) << (elements_per_second / 1e3)
           << " kelem/s";
    } else {
        os << std::fixed << std::setprecision(2) << elements_per_second
           << " elem/s";
    }
    return os.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// The benchmark
// ---------------------------------------------------------------------------

namespace {

// The host-side correctness reference for ONE generic compute task: the
// expected output computed with the operation's integer semantics, in the
// SAME defined way the CPU backend computes it (uint32 modular multiply for
// multiply/scale — never signed-overflow UB; see cpu_backend.cpp). Used ONLY
// for verification, never timed, never reported as performance. This is the
// verification reference — the module still has no compute path of its own:
// every measured number comes from VirtualGpu::execute().
std::vector<std::int32_t> expected_output(const ComputeTask& task) {
    std::vector<std::int32_t> expected(task.a.size());
    switch (task.op) {
        case ComputeOp::VectorAdd:
            for (std::size_t i = 0; i < task.a.size(); ++i) {
                expected[i] = task.a[i] + task.b[i];
            }
            break;
        case ComputeOp::VectorMultiply:
            for (std::size_t i = 0; i < task.a.size(); ++i) {
                expected[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(task.a[i]) *
                                                        static_cast<std::uint32_t>(task.b[i]));
            }
            break;
        case ComputeOp::VectorScale:
            for (std::size_t i = 0; i < task.a.size(); ++i) {
                expected[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(task.a[i]) *
                                                        static_cast<std::uint32_t>(task.scalar));
            }
            break;
    }
    return expected;
}

// Full verification of one iteration's result against the reference. Returns
// false (with a reason in 'why') for any wrong output — wrong size, wrong
// value — so a broken backend can never produce a "passing" benchmark.
bool result_matches(const ComputeTaskResult& result,
                    const std::vector<std::int32_t>& expected,
                    std::string& why) {
    if (result.status != Status::Ok) {
        why = std::string(to_string(result.status)) + " - " + result.error;
        return false;
    }
    if (result.data.size() != expected.size()) {
        why = "result has " + std::to_string(result.data.size()) +
              " elements, expected " + std::to_string(expected.size());
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (result.data[i] != expected[i]) {
            why = "result mismatch at index " + std::to_string(i) + ": got " +
                  std::to_string(result.data[i]) + ", expected " +
                  std::to_string(expected[i]);
            return false;
        }
    }
    return true;
}

}  // namespace

BenchmarkResult benchmark_vector_add(vortyx::vgpu::VirtualGpu& gpu,
                                     const VectorAddTask& task,
                                     const BenchmarkConfig& config) {
    // Phase 10: the Phase 8 entry point adapts the legacy task into the
    // generic engine and measures the ONE shared measurement path.
    ComputeTask generic;
    generic.op = ComputeOp::VectorAdd;
    generic.a = task.a;
    generic.b = task.b;
    return benchmark_compute(gpu, generic, config);
}

BenchmarkResult benchmark_compute(vortyx::vgpu::VirtualGpu& gpu,
                                  const ComputeTask& task,
                                  const BenchmarkConfig& config) {
    BenchmarkResult result;

    // --- 1. Config validation (before anything executes) ---------------
    if (config.iterations == 0) {
        result.status = Status::InvalidInput;
        result.error = "benchmark config requests 0 measured iterations; "
                       "statistics over nothing would be fabricated";
        return result;
    }

    // --- 2. Task validation (the workload must be runnable at all) -----
    std::string validation_error;
    const Status validation = validate_compute_task(task, validation_error);
    if (validation != Status::Ok) {
        result.status = validation;
        result.error = "benchmark task is invalid: " + validation_error;
        return result;
    }

    // --- 3. The execution target must be usable ------------------------
    // The benchmark does not initialize or reconfigure Virtual GPUs: a
    // non-Ready one is refused with the project's own status.
    if (!gpu.is_ready()) {
        result.status = Status::NotInitialized;
        result.error = "Virtual GPU is not Ready (state: " +
                       std::string(vortyx::vgpu::to_string(gpu.state())) +
                       "); initialize it before benchmarking";
        return result;
    }

    // --- 4. Record the target BEFORE measuring (honest even on failure).
    // The backend is exactly what the caller configured — the benchmark has
    // no ability and no permission to switch it (no silent fallback ever).
    result.workload = workload_label(task.op);
    result.backend = gpu.backend_name();
    result.device = gpu.device_info();
    result.element_count = task.a.size();
    result.warmup_iterations = config.warmup_iterations;
    result.iterations = 0;

    const std::vector<std::int32_t> expected = expected_output(task);

    // --- 5. Warmup: real execution, verified, excluded from statistics --
    for (std::uint32_t w = 0; w < config.warmup_iterations; ++w) {
        const ComputeTaskResult warm = gpu.execute(task);
        std::string why;
        if (!result_matches(warm, expected, why)) {
            result.status = warm.status != Status::Ok ? warm.status : Status::BackendError;
            result.error = "warmup iteration " + std::to_string(w) +
                           " failed: " + why;
            vortyx::log(vortyx::LogLevel::Warning,
                        "Benchmark aborted during warmup: " + result.error);
            return result;
        }
    }

    // --- 6. Measured iterations: time EXACTLY the execute() call --------
    std::vector<std::chrono::nanoseconds> samples;
    samples.reserve(config.iterations);

    for (std::uint32_t i = 0; i < config.iterations; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        const ComputeTaskResult run = gpu.execute(task);
        const auto end = std::chrono::steady_clock::now();

        result.iterations = i + 1;  // this attempt really happened

        std::string why;
        if (!result_matches(run, expected, why)) {
            result.status = run.status != Status::Ok ? run.status : Status::BackendError;
            result.error = "measured iteration " + std::to_string(i) +
                           " failed: " + why;
            vortyx::log(vortyx::LogLevel::Warning,
                        "Benchmark aborted during measurement: " + result.error);
            return result;  // no partial statistics presented as success
        }

        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin));
    }

    // --- 7. Statistics over the real samples ----------------------------
    std::string stats_error;
    if (!compute_timing_stats(samples, result.element_count, result.timing,
                              stats_error)) {
        // Cannot happen (samples is non-empty here, element_count > 0),
        // but the failure path stays honest instead of reporting zeros.
        result.status = Status::BackendError;
        result.error = "failed to summarize timing samples: " + stats_error;
        return result;
    }

    result.status = Status::Ok;
    result.correctness_verified = true;

    vortyx::log(vortyx::LogLevel::Info,
                "Benchmark '" + result.workload + "' on '" + result.backend + "': " +
                    std::to_string(result.element_count) + " elements x " +
                    std::to_string(result.iterations) + " iterations (" +
                    std::to_string(result.warmup_iterations) +
                    " warmup, excluded): avg " +
                    format_duration_ns(result.timing.average_ns) +
                    ", correctness verified.");
    return result;
}

// ---------------------------------------------------------------------------
// Output forms
// ---------------------------------------------------------------------------

namespace {

const char* device_type_text(vortyx::device::DeviceType type) {
    switch (type) {
        case vortyx::device::DeviceType::Unknown: return "Unknown";
        case vortyx::device::DeviceType::Cpu: return "Cpu";
        case vortyx::device::DeviceType::Gpu: return "Gpu";
        case vortyx::device::DeviceType::SoftwareGpu: return "SoftwareGpu";
    }
    return "Unknown";
}

}  // namespace

std::string describe(const BenchmarkResult& result) {
    std::ostringstream os;
    os << "Benchmark '" << result.workload << "' on backend '" << result.backend << "'";

    if (result.status != Status::Ok) {
        os << " FAILED: " << to_string(result.status) << " - " << result.error;
        return os.str();
    }

    os << " (device: "
       << (result.device.name.empty() ? std::string("unknown")
                                      : result.device.name)
       << "): " << result.element_count << " elements, "
       << result.iterations << " measured iterations ("
       << result.warmup_iterations << " warmup, excluded): min "
       << format_duration_ns(static_cast<double>(result.timing.min.count()))
       << " | avg " << format_duration_ns(result.timing.average_ns)
       << " | median " << format_duration_ns(result.timing.median_ns)
       << " | max " << format_duration_ns(static_cast<double>(result.timing.max.count()))
       << " | stddev " << format_duration_ns(result.timing.stddev_ns)
       << " | throughput " << format_throughput(result.timing.throughput_elements_per_second)
       << " | correctness: " << (result.correctness_verified ? "PASS" : "NOT VERIFIED");
    return os.str();
}

std::vector<std::pair<std::string, std::string>> to_key_values(const BenchmarkResult& result) {
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(15);

    pairs.emplace_back("workload", result.workload);
    pairs.emplace_back("status", to_string(result.status));
    if (!result.error.empty()) pairs.emplace_back("error", result.error);
    pairs.emplace_back("backend", result.backend);
    pairs.emplace_back("device_type", device_type_text(result.device.type));
    pairs.emplace_back("device_name", result.device.name);
    pairs.emplace_back("element_count", std::to_string(result.element_count));
    pairs.emplace_back("warmup_iterations", std::to_string(result.warmup_iterations));
    pairs.emplace_back("iterations", std::to_string(result.iterations));

    // Timing keys exist only when they carry real values: on a failed run
    // there are no statistics, and inventing zeros would be fabrication.
    if (result.status == Status::Ok) {
        pairs.emplace_back("min_ns", std::to_string(result.timing.min.count()));
        pairs.emplace_back("average_ns", [&] {
            std::ostringstream os;
            os << std::fixed << std::setprecision(3) << result.timing.average_ns;
            return os.str();
        }());
        pairs.emplace_back("median_ns", [&] {
            std::ostringstream os;
            os << std::fixed << std::setprecision(3) << result.timing.median_ns;
            return os.str();
        }());
        pairs.emplace_back("max_ns", std::to_string(result.timing.max.count()));
        pairs.emplace_back("stddev_ns", [&] {
            std::ostringstream os;
            os << std::fixed << std::setprecision(3) << result.timing.stddev_ns;
            return os.str();
        }());
        pairs.emplace_back("throughput_elements_per_second", [&] {
            std::ostringstream os;
            os << std::fixed << std::setprecision(3)
               << result.timing.throughput_elements_per_second;
            return os.str();
        }());
        pairs.emplace_back("correctness_verified",
                           result.correctness_verified ? "true" : "false");
    }
    return pairs;
}

}  // namespace vortyx::benchmark
