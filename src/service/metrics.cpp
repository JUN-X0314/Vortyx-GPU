// Service metrics (Phase 14) — implementation.

#include "service/metrics.hpp"

namespace vortyx::service {

ServiceMetricsSnapshot ServiceMetrics::snapshot() const {
    ServiceMetricsSnapshot snapshot;
    snapshot.submit_attempts = submit_attempts_.load(std::memory_order_relaxed);
    snapshot.jobs_submitted = jobs_submitted_.load(std::memory_order_relaxed);
    snapshot.jobs_replayed = jobs_replayed_.load(std::memory_order_relaxed);
    snapshot.jobs_completed = jobs_completed_.load(std::memory_order_relaxed);
    snapshot.jobs_failed = jobs_failed_.load(std::memory_order_relaxed);
    snapshot.jobs_cancelled = jobs_cancelled_.load(std::memory_order_relaxed);
    snapshot.quota_rejections = quota_rejections_.load(std::memory_order_relaxed);
    snapshot.rate_limit_rejections = rate_limit_rejections_.load(std::memory_order_relaxed);
    snapshot.jobs_queued = jobs_queued_.load(std::memory_order_relaxed);
    snapshot.jobs_running = jobs_running_.load(std::memory_order_relaxed);
    return snapshot;
}

}  // namespace vortyx::service
