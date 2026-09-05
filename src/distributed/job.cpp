// Distributed job state implementation (Phase 12).

#include "distributed/job.hpp"

namespace vortyx::distributed {

const char* to_string(DistributedJobStatus status) {
    switch (status) {
        case DistributedJobStatus::Queued: return "queued";
        case DistributedJobStatus::Planning: return "planning";
        case DistributedJobStatus::Scheduled: return "scheduled";
        case DistributedJobStatus::Running: return "running";
        case DistributedJobStatus::Completed: return "completed";
        case DistributedJobStatus::Failed: return "failed";
        case DistributedJobStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

bool distributed_job_status_is_terminal(DistributedJobStatus status) {
    switch (status) {
        case DistributedJobStatus::Completed:
        case DistributedJobStatus::Failed:
        case DistributedJobStatus::Cancelled: return true;
        case DistributedJobStatus::Queued:
        case DistributedJobStatus::Planning:
        case DistributedJobStatus::Scheduled:
        case DistributedJobStatus::Running: return false;
    }
    return false;
}

bool distributed_job_transition_valid(DistributedJobStatus from, DistributedJobStatus to) {
    switch (from) {
        case DistributedJobStatus::Queued:
            return to == DistributedJobStatus::Planning ||
                   to == DistributedJobStatus::Cancelled;
        case DistributedJobStatus::Planning:
            return to == DistributedJobStatus::Scheduled ||
                   to == DistributedJobStatus::Running ||  // retry-driven re-planning
                   to == DistributedJobStatus::Failed ||
                   to == DistributedJobStatus::Cancelled;
        case DistributedJobStatus::Scheduled:
            return to == DistributedJobStatus::Running ||
                   to == DistributedJobStatus::Planning ||  // stale-plan re-plan
                   to == DistributedJobStatus::Cancelled;
        case DistributedJobStatus::Running:
            return to == DistributedJobStatus::Completed ||
                   to == DistributedJobStatus::Failed ||
                   to == DistributedJobStatus::Planning ||  // re-placement of a failed shard
                   to == DistributedJobStatus::Cancelled;
        case DistributedJobStatus::Completed:
        case DistributedJobStatus::Failed:
        case DistributedJobStatus::Cancelled:
            return false;  // terminal
    }
    return false;
}

DistributedJobStatus derive_job_status(const std::vector<JobShard>& shards) {
    if (shards.empty()) return DistributedJobStatus::Queued;

    bool any_pending = false, any_retrying = false, any_assigned = false, any_running = false;
    bool any_failed = false, any_cancelled = false;
    for (const JobShard& shard : shards) {
        switch (shard.state) {
            case ShardState::Pending: any_pending = true; break;
            case ShardState::Retrying: any_retrying = true; break;
            case ShardState::Assigned: any_assigned = true; break;
            case ShardState::Running: any_running = true; break;
            case ShardState::Failed: any_failed = true; break;
            case ShardState::Cancelled: any_cancelled = true; break;
            case ShardState::Completed: break;
        }
    }

    // Priority order IS the documented rule: an unfinished shard outranks
    // a failed one (the job is still working), and among terminal-only
    // sets a failure outranks cancellation, which outranks success.
    if (any_pending || any_retrying) return DistributedJobStatus::Planning;
    if (any_assigned) return DistributedJobStatus::Scheduled;
    if (any_running) return DistributedJobStatus::Running;
    if (any_failed) return DistributedJobStatus::Failed;
    if (any_cancelled) return DistributedJobStatus::Cancelled;
    return DistributedJobStatus::Completed;
}

vortyx::platform::JobStatus map_to_platform_job_status(DistributedJobStatus status) {
    switch (status) {
        case DistributedJobStatus::Queued: return vortyx::platform::JobStatus::Queued;
        case DistributedJobStatus::Planning:
        case DistributedJobStatus::Scheduled:
        case DistributedJobStatus::Running: return vortyx::platform::JobStatus::Running;
        case DistributedJobStatus::Completed: return vortyx::platform::JobStatus::Completed;
        case DistributedJobStatus::Failed: return vortyx::platform::JobStatus::Failed;
        case DistributedJobStatus::Cancelled: return vortyx::platform::JobStatus::Cancelled;
    }
    return vortyx::platform::JobStatus::Failed;
}

}  // namespace vortyx::distributed
