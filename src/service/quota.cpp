// Project quota engine (Phase 14) — implementation.

#include "service/quota.hpp"

namespace vortyx::service {

void QuotaEngine::set_default_quota(const ProjectQuota& quota) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_quota_ = quota;
}

ProjectQuota QuotaEngine::default_quota() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return default_quota_;
}

void QuotaEngine::set_quota(const std::string& project_id, const ProjectQuota& quota) {
    std::lock_guard<std::mutex> lock(mutex_);
    quotas_[project_id] = quota;
}

ProjectQuota QuotaEngine::quota(const std::string& project_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return quota_for_locked(project_id);
}

ProjectQuota QuotaEngine::quota_for_locked(const std::string& project_id) const {
    const auto it = quotas_.find(project_id);
    return it != quotas_.end() ? it->second : default_quota_;
}

QuotaEngine::Decision QuotaEngine::reserve(const std::string& project_id,
                                           const vortyx::platform::JobId& job_id,
                                           std::int64_t shards, std::int64_t memory_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Dimension sanity first (a malformed request is never a quota verdict).
    if (shards < 1 || memory_bytes < 0) {
        Decision decision;
        decision.status = ServiceStatus::InvalidInput;
        decision.error = "quota reservation needs shards >= 1 and memory_bytes >= 0";
        return decision;
    }

    // Replay / conflict on the ledger.
    const auto existing = ledger_.find(job_id);
    if (existing != ledger_.end()) {
        Decision decision;
        if (existing->second.project_id == project_id &&
            existing->second.shards == shards &&
            existing->second.memory_bytes == memory_bytes) {
            decision.status = ServiceStatus::Ok;  // replay: no double charge
            decision.usage_after = usage_.count(project_id) ? usage_[project_id] : QuotaUsage{};
        } else {
            decision.status = ServiceStatus::Conflict;
            decision.error = "job id already holds a different quota reservation";
        }
        return decision;
    }

    const ProjectQuota limits = quota_for_locked(project_id);
    QuotaUsage next = usage_.count(project_id) ? usage_[project_id] : QuotaUsage{};

    Decision decision;
    if (next.active_jobs + 1 > limits.max_concurrent_jobs) {
        decision.status = ServiceStatus::QuotaExceeded;
        decision.error = "project concurrent-job quota exceeded (" +
                         std::to_string(limits.max_concurrent_jobs) + ")";
        return decision;
    }
    if (next.running_shards + shards > limits.max_running_shards) {
        decision.status = ServiceStatus::QuotaExceeded;
        decision.error = "project running-shard quota exceeded (" +
                         std::to_string(limits.max_running_shards) + " requested total " +
                         std::to_string(next.running_shards + shards) + ")";
        return decision;
    }
    if (next.reserved_memory_bytes + memory_bytes > limits.max_memory_bytes) {
        decision.status = ServiceStatus::QuotaExceeded;
        decision.error = "project memory quota exceeded (" +
                         std::to_string(limits.max_memory_bytes) + " bytes)";
        return decision;
    }

    next.active_jobs += 1;
    next.running_shards += shards;
    next.reserved_memory_bytes += memory_bytes;
    usage_[project_id] = next;
    ledger_[job_id] = Reservation{project_id, shards, memory_bytes};

    decision.status = ServiceStatus::Ok;
    decision.usage_after = next;
    return decision;
}

bool QuotaEngine::release(const vortyx::platform::JobId& job_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = ledger_.find(job_id);
    if (it == ledger_.end()) {
        return false;  // unknown or already released — the ledger never
                       // double-credits and never goes negative
    }
    const auto usage_it = usage_.find(it->second.project_id);
    if (usage_it != usage_.end()) {
        QuotaUsage& usage = usage_it->second;
        usage.active_jobs -= 1;
        usage.running_shards -= it->second.shards;
        usage.reserved_memory_bytes -= it->second.memory_bytes;
        // Defensive clamp (a broken ledger would surface as zeros, not
        // negatives — accounting stays displayable under any bug).
        if (usage.active_jobs < 0) usage.active_jobs = 0;
        if (usage.running_shards < 0) usage.running_shards = 0;
        if (usage.reserved_memory_bytes < 0) usage.reserved_memory_bytes = 0;
    }
    ledger_.erase(it);
    return true;
}

bool QuotaEngine::has_reservation(const vortyx::platform::JobId& job_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ledger_.count(job_id) != 0;
}

QuotaUsage QuotaEngine::usage(const std::string& project_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = usage_.find(project_id);
    return it != usage_.end() ? it->second : QuotaUsage{};
}

}  // namespace vortyx::service
