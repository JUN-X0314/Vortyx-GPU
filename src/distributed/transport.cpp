// LocalInProcessTransport implementation (Phase 12).

#include "distributed/transport.hpp"

namespace vortyx::distributed {

bool LocalInProcessTransport::attach(IWorker* worker) {
    if (worker == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const IWorker* existing : workers_) {
        if (existing->device_id() == worker->device_id()) return false;  // one per device
    }
    workers_.push_back(worker);
    injections_.emplace_back();
    return true;
}

void LocalInProcessTransport::detach_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    workers_.clear();
    injections_.clear();
    cancelled_shards_.clear();
}

ShardResult LocalInProcessTransport::submit_shard(const ShardExecution& execution) {
    IWorker* worker = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < workers_.size(); ++i) {
            if (workers_[i]->device_id() == execution.device_id) {
                // Deterministic injection: fail BEFORE the worker runs.
                Injection& injection = injections_[i];
                if (injection.remaining > 0) {
                    --injection.remaining;
                    ShardResult failed;
                    failed.shard_id = execution.shard_id;
                    failed.parent_job_id = execution.parent_job_id;
                    failed.shard_index = execution.shard_index;
                    failed.attempt = execution.attempt;
                    failed.device_id = execution.device_id;
                    failed.completed = false;
                    failed.failure_code = injection.code;
                    failed.error = "injected transport failure (" +
                                   std::string(to_string(injection.code)) + ")";
                    return failed;
                }
                worker = workers_[i];
                break;
            }
        }
    }
    if (worker == nullptr) {
        ShardResult failed;
        failed.shard_id = execution.shard_id;
        failed.parent_job_id = execution.parent_job_id;
        failed.shard_index = execution.shard_index;
        failed.attempt = execution.attempt;
        failed.device_id = execution.device_id;
        failed.completed = false;
        failed.failure_code = FailureCode::DeviceLost;
        failed.error = "no worker is attached for device '" + execution.device_id + "'";
        return failed;
    }
    return worker->execute_shard(execution);
}

bool LocalInProcessTransport::cancel_shard(const std::string& shard_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Record the request. The local transport's synchronous submit cannot
    // retroactively stop a running shard; the recorded request is the
    // seam a future async transport implements for real. Returns true when
    // the request was recorded (idempotent).
    for (const std::string& existing : cancelled_shards_) {
        if (existing == shard_id) return true;
    }
    cancelled_shards_.push_back(shard_id);
    return true;
}

IWorker* LocalInProcessTransport::worker_for(const DeviceId& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (IWorker* worker : workers_) {
        if (worker->device_id() == device_id) return worker;
    }
    return nullptr;
}

void LocalInProcessTransport::inject_failure(const DeviceId& device_id, std::uint32_t count,
                                             FailureCode code) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < workers_.size(); ++i) {
        if (workers_[i]->device_id() == device_id) {
            injections_[i].remaining = count;
            injections_[i].code = code;
            return;
        }
    }
    // Unknown device: nothing to inject (tests only inject attached devices).
}

std::uint32_t LocalInProcessTransport::injected_failures(const DeviceId& device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < workers_.size(); ++i) {
        if (workers_[i]->device_id() == device_id) return injections_[i].remaining;
    }
    return 0;
}

}  // namespace vortyx::distributed
