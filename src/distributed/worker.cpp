// LocalWorker implementation (Phase 12) — the adapter over the existing
// local execution path. The slicing is the only NEW computation here, and
// it copies a contiguous elementwise range (pinned bit-exact against the
// full-range result by the test suite).

#include "distributed/worker.hpp"

namespace vortyx::distributed {

const char* to_string(WorkerState state) {
    switch (state) {
        case WorkerState::Starting: return "starting";
        case WorkerState::Ready: return "ready";
        case WorkerState::Running: return "running";
        case WorkerState::Draining: return "draining";
        case WorkerState::Stopped: return "stopped";
    }
    return "unknown";
}

namespace {

// Fills a failed result with the common fields from the execution.
ShardResult failed_result(const ShardExecution& execution, FailureCode code,
                          const std::string& error) {
    ShardResult result;
    result.shard_id = execution.shard_id;
    result.parent_job_id = execution.parent_job_id;
    result.shard_index = execution.shard_index;
    result.attempt = execution.attempt;
    result.device_id = execution.device_id;
    result.completed = false;
    result.failure_code = code;
    result.error = error;
    return result;
}

}  // namespace

LocalWorker::LocalWorker(DeviceId device_id,
                         std::vector<vortyx::compute::ComputeOp> claimed_operations)
    : device_id_(std::move(device_id)), claimed_operations_(std::move(claimed_operations)) {}

LocalWorker::~LocalWorker() { stop(); }

WorkerState LocalWorker::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

vortyx::compute::Status LocalWorker::start(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == WorkerState::Ready || state_ == WorkerState::Running) {
        return vortyx::compute::Status::Ok;  // idempotent
    }
    if (state_ == WorkerState::Stopped || state_ == WorkerState::Draining) {
        error = "worker is draining or stopped; create a new worker instead";
        return vortyx::compute::Status::InvalidInput;
    }
    runtime_ = std::make_unique<vortyx::compute::Runtime>();
    const vortyx::compute::Status status = runtime_->initialize();
    if (status != vortyx::compute::Status::Ok) {
        // Honest failure: the runtime's own reason is carried, the worker
        // stays Starting and owns nothing.
        error = "worker runtime initialization failed";
        runtime_.reset();
        return status;
    }
    state_ = WorkerState::Ready;
    return vortyx::compute::Status::Ok;
}

vortyx::compute::Status LocalWorker::drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == WorkerState::Draining) return vortyx::compute::Status::Ok;  // idempotent
    if (state_ == WorkerState::Stopped) {
        return vortyx::compute::Status::InvalidInput;
    }
    if (state_ == WorkerState::Starting) {
        return vortyx::compute::Status::NotInitialized;
    }
    state_ = WorkerState::Draining;
    return vortyx::compute::Status::Ok;
}

void LocalWorker::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == WorkerState::Stopped) return;
    // A worker with an in-flight shard cannot be executing (the executor
    // holds mutex_ while running); stopping here is therefore safe.
    if (runtime_) {
        runtime_->shutdown();
        runtime_.reset();
    }
    state_ = WorkerState::Stopped;
}

bool LocalWorker::validate_and_slice(const ShardExecution& execution,
                                     vortyx::compute::ComputeTask& slice,
                                     std::string& resolved_backend, ShardResult& failure) {
    if (execution.device_id != device_id_) {
        failure = failed_result(execution, FailureCode::InvalidAssignment,
                                "assignment targets device '" + execution.device_id +
                                    "' but this worker serves '" + device_id_ + "'");
        return false;
    }
    if (execution.work.kind != PartitionKind::ElementRange) {
        failure = failed_result(execution, FailureCode::InvalidAssignment,
                                "unsupported partition kind (this worker executes element ranges)");
        return false;
    }
    const ElementRange& range = execution.work.element_range;
    if (range.begin >= range.end) {
        failure = failed_result(execution, FailureCode::InvalidAssignment,
                                "empty element range (begin >= end)");
        return false;
    }
    const std::uint64_t total = execution.task.element_count();
    if (range.end > total) {
        failure = failed_result(execution, FailureCode::InvalidAssignment,
                                "shard range [" + std::to_string(range.begin) + ", " +
                                    std::to_string(range.end) + ") exceeds the task domain (" +
                                    std::to_string(total) + " elements)");
        return false;
    }
    bool operation_claimed = false;
    for (const vortyx::compute::ComputeOp claimed : claimed_operations_) {
        if (claimed == execution.task.op) {
            operation_claimed = true;
            break;
        }
    }
    if (!operation_claimed) {
        // The worker was built for its device's claims; the assignment must
        // agree (a mismatch means the placement lied about capability).
        failure = failed_result(execution, FailureCode::InvalidAssignment,
                                "assignment operation does not match this device's claim");
        return false;
    }

    // Backend resolution: explicit request if given (validated against the
    // runtime's honest availability — never silently remapped), else the
    // first available runtime backend (deterministic: the runtime's
    // registration order). An explicit backend that is unavailable FAILS —
    // the no-silent-fallback rule applies per device too.
    if (!execution.backend.empty()) {
        if (!runtime_->has_backend(execution.backend)) {
            failure = failed_result(execution, FailureCode::WorkerExecutionFailed,
                                    "backend '" + execution.backend +
                                        "' is not available on this device");
            return false;
        }
        resolved_backend = execution.backend;
    } else {
        const std::vector<std::string> available = runtime_->backend_names();
        if (available.empty()) {
            failure = failed_result(execution, FailureCode::WorkerExecutionFailed,
                                    "no backend is available on this device");
            return false;
        }
        resolved_backend = available.front();
    }

    // The slice: a contiguous copy of the shard's range. The elementwise
    // ops are range-independent, so slice-result + reassembly equals the
    // full-range result bit for bit (pinned by tests).
    const std::uint64_t size = range.end - range.begin;
    slice.op = execution.task.op;
    slice.scalar = execution.task.scalar;
    slice.a.assign(execution.task.a.begin() + static_cast<std::ptrdiff_t>(range.begin),
                   execution.task.a.begin() + static_cast<std::ptrdiff_t>(range.end));
    if (!execution.task.b.empty()) {
        slice.b.assign(execution.task.b.begin() + static_cast<std::ptrdiff_t>(range.begin),
                       execution.task.b.begin() + static_cast<std::ptrdiff_t>(range.end));
    }
    (void)size;
    return true;
}

ShardResult LocalWorker::execute_shard(const ShardExecution& execution) {
    // Serialize executions (the Runtime's single-thread contract) and the
    // lifecycle check.
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ == WorkerState::Stopped || state_ == WorkerState::Starting) {
        return failed_result(execution, FailureCode::WorkerExecutionFailed,
                             std::string("worker is not started (state: ") +
                                 to_string(state_) + ")");
    }
    if (state_ == WorkerState::Draining) {
        return failed_result(execution, FailureCode::WorkerExecutionFailed,
                             "worker is draining and accepts no new assignments");
    }

    vortyx::compute::ComputeTask slice;
    std::string resolved_backend;
    ShardResult refusal;
    if (!validate_and_slice(execution, slice, resolved_backend, refusal)) {
        return refusal;
    }

    state_ = WorkerState::Running;
    const vortyx::compute::ComputeTaskResult outcome = runtime_->execute(slice, resolved_backend);
    state_ = WorkerState::Ready;

    ShardResult result = failed_result(execution, FailureCode::None, std::string());
    result.backend = resolved_backend;
    if (outcome.status != vortyx::compute::Status::Ok) {
        result.completed = false;
        result.failure_code = FailureCode::WorkerExecutionFailed;
        result.error = std::string("execution failed on backend '") + resolved_backend +
                       "': " + outcome.error;
        return result;
    }
    result.completed = true;
    result.data = std::move(outcome.data);
    result.element_begin = execution.work.element_range.begin;
    result.element_end = execution.work.element_range.end;
    return result;
}

}  // namespace vortyx::distributed
