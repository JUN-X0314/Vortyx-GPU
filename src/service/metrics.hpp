#pragma once

// Service metrics (Phase 14) — REAL counters only.
//
// Every value here is a count the service actually performed or observed:
// accepted submissions, terminal outcomes, policy refusals, queue/running
// gauges. NOTHING measures latency or throughput in Phase 14 and NOTHING is
// estimated — a metric the service does not measure does not exist here
// (the fake-telemetry rule). A future phase adds real timing behind the
// same snapshot shape.
//
// Thread-safe: counters are atomics; snapshot() returns a consistent copy
// (each field is an independent atomic read — a snapshot is a close-enough
// instant view for observability and is documented as such, never as a
// barrier-consistent transaction).

#include <atomic>
#include <cstdint>

namespace vortyx::service {

struct ServiceMetricsSnapshot {
    // Submission outcomes.
    std::uint64_t submit_attempts = 0;    // ALL submit calls (the API-proxy counter)
    std::uint64_t jobs_submitted = 0;     // accepted (created) submissions
    std::uint64_t jobs_replayed = 0;      // idempotent replays (no side effects)

    // Terminal outcomes (created jobs only; every created job ends in one).
    std::uint64_t jobs_completed = 0;
    std::uint64_t jobs_failed = 0;
    std::uint64_t jobs_cancelled = 0;

    // Policy refusals.
    std::uint64_t quota_rejections = 0;
    std::uint64_t rate_limit_rejections = 0;

    // Gauges (the instant of the snapshot).
    std::int64_t jobs_queued = 0;
    std::int64_t jobs_running = 0;
};

class ServiceMetrics final {
public:
    void inc_submit_attempts() { submit_attempts_.fetch_add(1, std::memory_order_relaxed); }
    void inc_jobs_submitted() { jobs_submitted_.fetch_add(1, std::memory_order_relaxed); }
    void inc_jobs_replayed() { jobs_replayed_.fetch_add(1, std::memory_order_relaxed); }
    void inc_jobs_completed() { jobs_completed_.fetch_add(1, std::memory_order_relaxed); }
    void inc_jobs_failed() { jobs_failed_.fetch_add(1, std::memory_order_relaxed); }
    void inc_jobs_cancelled() { jobs_cancelled_.fetch_add(1, std::memory_order_relaxed); }
    void inc_quota_rejections() { quota_rejections_.fetch_add(1, std::memory_order_relaxed); }
    void inc_rate_limit_rejections() {
        rate_limit_rejections_.fetch_add(1, std::memory_order_relaxed);
    }

    void set_jobs_queued(std::int64_t value) { jobs_queued_.store(value, std::memory_order_relaxed); }
    void inc_jobs_running() { jobs_running_.fetch_add(1, std::memory_order_relaxed); }
    void dec_jobs_running() { jobs_running_.fetch_sub(1, std::memory_order_relaxed); }

    ServiceMetricsSnapshot snapshot() const;

private:
    std::atomic<std::uint64_t> submit_attempts_{0};
    std::atomic<std::uint64_t> jobs_submitted_{0};
    std::atomic<std::uint64_t> jobs_replayed_{0};
    std::atomic<std::uint64_t> jobs_completed_{0};
    std::atomic<std::uint64_t> jobs_failed_{0};
    std::atomic<std::uint64_t> jobs_cancelled_{0};
    std::atomic<std::uint64_t> quota_rejections_{0};
    std::atomic<std::uint64_t> rate_limit_rejections_{0};
    std::atomic<std::int64_t> jobs_queued_{0};
    std::atomic<std::int64_t> jobs_running_{0};
};

}  // namespace vortyx::service
