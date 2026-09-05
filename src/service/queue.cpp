// Provider-neutral service job queue (Phase 14) — implementation.

#include "service/queue.hpp"

#include <algorithm>

namespace vortyx::service {

InMemoryJobQueue::InMemoryJobQueue(std::size_t capacity) : capacity_(capacity) {}

ServiceStatus InMemoryJobQueue::enqueue(const QueuedJob& job, bool& created) {
    created = false;
    if (job.job_id.empty()) {
        return ServiceStatus::InvalidInput;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const QueuedJob& item : items_) {
        if (item.job_id == job.job_id) {
            return ServiceStatus::Ok;  // replay: already queued, one slot
        }
    }
    if (items_.size() >= capacity_) {
        return ServiceStatus::Unavailable;
    }
    items_.push_back(job);
    created = true;
    return ServiceStatus::Ok;
}

bool InMemoryJobQueue::try_dequeue(QueuedJob& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty()) return false;
    out = items_.front();
    items_.erase(items_.begin());
    return true;
}

bool InMemoryJobQueue::remove(const vortyx::platform::JobId& job_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->job_id == job_id) {
            items_.erase(it);
            return true;
        }
    }
    return false;
}

bool InMemoryJobQueue::contains(const vortyx::platform::JobId& job_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const QueuedJob& item : items_) {
        if (item.job_id == job_id) return true;
    }
    return false;
}

std::size_t InMemoryJobQueue::depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

std::vector<QueuedJob> InMemoryJobQueue::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

}  // namespace vortyx::service
