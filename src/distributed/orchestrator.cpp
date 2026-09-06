// DistributedOrchestrator implementation (Phase 12).
//
// Locking discipline (restated from the header): mutex_ guards the jobs
// table and record mutations; it is NEVER held across transport submits,
// registry calls, or the platform store — those own their own locks. The
// execution pipeline is deadlock-free by construction: at most one lock is
// held at any moment.

#include "distributed/orchestrator.hpp"

#include <thread>
#include <utility>

#include "core/compute/task.hpp"
#include "distributed/lease.hpp"
#include "distributed/shard.hpp"

namespace vortyx::distributed {

// Copy support (the atomic cancel flag blocks implicit generation; a copy
// is a snapshot of the record, including the flag's VALUE).
DistributedJobRecord::DistributedJobRecord(const DistributedJobRecord& other)
    : job_id(other.job_id),
      owner_user_id(other.owner_user_id),
      operation(other.operation),
      element_count(other.element_count),
      requested_backend(other.requested_backend),
      requested_shard_count(other.requested_shard_count),
      status(other.status),
      error(other.error),
      shards(other.shards),
      result(other.result),
      created_at_ms(other.created_at_ms),
      completed_at_ms(other.completed_at_ms),
      platform_error(other.platform_error),
      platform_status(other.platform_status),
      cancel_requested(other.cancel_requested.load(std::memory_order_relaxed)) {}

DistributedJobRecord& DistributedJobRecord::operator=(const DistributedJobRecord& other) {
    if (this != &other) {
        job_id = other.job_id;
        owner_user_id = other.owner_user_id;
        operation = other.operation;
        element_count = other.element_count;
        requested_backend = other.requested_backend;
        requested_shard_count = other.requested_shard_count;
        status = other.status;
        error = other.error;
        shards = other.shards;
        result = other.result;
        created_at_ms = other.created_at_ms;
        completed_at_ms = other.completed_at_ms;
        platform_error = other.platform_error;
        platform_status = other.platform_status;
        cancel_requested.store(other.cancel_requested.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
    }
    return *this;
}

namespace {

// A request fingerprint for the submission idempotency rule: the
// control-plane envelope plus the full local payload. Comparing the data
// is deliberate (correctness over speed): two different payloads under one
// job id is a Conflict, not a replay.
bool same_request(const DistributedJobRequest& a, const DistributedJobRequest& b) {
    return a.envelope.job_id == b.envelope.job_id &&
           a.envelope.operation == b.envelope.operation &&
           a.envelope.element_count == b.envelope.element_count &&
           a.envelope.requested_backend == b.envelope.requested_backend &&
           a.envelope.priority == b.envelope.priority &&
           a.envelope.protocol_version == b.envelope.protocol_version &&
           a.requested_shard_count == b.requested_shard_count &&
           a.task.op == b.task.op && a.task.scalar == b.task.scalar && a.task.a == b.task.a &&
           a.task.b == b.task.b;
}

// Anti-enumeration: unknown AND foreign jobs are the same outcome.
vortyx::platform::Status record_not_found() {
    return vortyx::platform::Status::NotFound;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

vortyx::platform::Status DistributedOrchestrator::create(Deps deps,
                                                         const DistributedConfig& config,
                                                         std::unique_ptr<DistributedOrchestrator>& out,
                                                         std::string& error) {
    if (deps.registry == nullptr || deps.transport == nullptr || deps.clock == nullptr) {
        error = "orchestrator requires a registry, a transport and a clock";
        return vortyx::platform::Status::InvalidInput;
    }
    vortyx::platform::Status status = config.validate(error);
    if (status != vortyx::platform::Status::Ok) return status;

    std::shared_ptr<ISchedulingPolicy> policy;
    if (deps.policy_override != nullptr) {
        // Phase 16 seam: the caller's policy implementation (e.g. the
        // Adaptive Compute Fabric's bridge) replaces the config-name
        // policy. Every other orchestrator behavior is unchanged.
        policy = deps.policy_override;
    } else {
        policy = make_scheduling_policy(config.scheduler_policy);
    }
    if (policy == nullptr) {
        error = "unknown scheduler policy '" + config.scheduler_policy + "'";
        return vortyx::platform::Status::InvalidInput;
    }

    RetryPolicy retry;
    retry.max_attempts = 1 + config.max_retries;  // config counts EXTRA attempts
    retry.backoff_base_ms = config.retry_backoff_ms;

    out.reset(new DistributedOrchestrator(std::move(deps), config, retry, std::move(policy)));
    return vortyx::platform::Status::Ok;
}

DistributedOrchestrator::DistributedOrchestrator(Deps deps, DistributedConfig config,
                                                 RetryPolicy retry,
                                                 std::shared_ptr<ISchedulingPolicy> policy)
    : deps_(std::move(deps)),
      config_(config),
      retry_(retry),
      policy_(std::move(policy)),
      heartbeat_(std::make_unique<HeartbeatMonitor>(*deps_.registry, deps_.clock,
                                                    config_.heartbeat_timeout_ms)) {}

// ---------------------------------------------------------------------------
// Device conveniences
// ---------------------------------------------------------------------------

vortyx::platform::Status DistributedOrchestrator::register_device(const UserId& owner_user_id,
                                                                  const DeviceId& device_id,
                                                                  const DeviceCapabilities& capabilities,
                                                                  DeviceDescriptor& out,
                                                                  bool& created) {
    // The config bound (max_devices == 0 means unlimited), enforced at the
    // orchestrator's registration path with the owner's own view. An
    // IDEMPOTENT replay must not hit the bound: the registry decides
    // replay-vs-conflict, so only NEW registrations beyond the bound are
    // refused here.
    if (config_.max_devices > 0) {
        std::vector<DeviceDescriptor> owned;
        deps_.registry->devices(owner_user_id, owned);
        bool known = false;
        for (const DeviceDescriptor& d : owned) {
            if (d.device_id == device_id) known = true;
        }
        if (!known && owned.size() >= config_.max_devices) {
            std::string error = "device limit reached (max_devices=" +
                                std::to_string(config_.max_devices) + ")";
            return vortyx::platform::Status::InvalidInput;
        }
    }
    return deps_.registry->register_device(device_id, owner_user_id, capabilities, out, created);
}

vortyx::platform::Status DistributedOrchestrator::devices(const UserId& owner_user_id,
                                                          std::vector<DeviceDescriptor>& out) {
    return deps_.registry->devices(owner_user_id, out);
}

vortyx::platform::Status DistributedOrchestrator::device(const UserId& owner_user_id,
                                                         const DeviceId& device_id,
                                                         DeviceDescriptor& out) {
    return deps_.registry->device(owner_user_id, device_id, out);
}

vortyx::platform::Status DistributedOrchestrator::heartbeat_device(const UserId& owner_user_id,
                                                                   const DeviceId& device_id) {
    return deps_.registry->heartbeat_device(owner_user_id, device_id);
}

vortyx::platform::Status DistributedOrchestrator::set_device_state(const UserId& owner_user_id,
                                                                   const DeviceId& device_id,
                                                                   DeviceState to) {
    return deps_.registry->update_device_state(owner_user_id, device_id, to);
}

std::size_t DistributedOrchestrator::check_heartbeats(const UserId& owner_user_id) {
    return heartbeat_->check(owner_user_id);
}

ClusterSnapshot DistributedOrchestrator::cluster_snapshot(const UserId& owner_user_id) {
    // The registry snapshot carries every registered device; the
    // orchestrator hands out the OWNER'S view only (foreign devices are
    // invisible — the Phase 11 visibility rule applied to the cluster).
    ClusterSnapshot full = deps_.registry->snapshot();
    ClusterSnapshot view;
    view.revision = full.revision;
    view.topology = full.topology;
    view.devices = full.visible_for(owner_user_id);
    return view;
}

// ---------------------------------------------------------------------------
// Job queries
// ---------------------------------------------------------------------------

vortyx::platform::Status DistributedOrchestrator::job(const vortyx::platform::AuthContext& auth,
                                                      const JobId& job_id,
                                                      DistributedJobRecord& out) {
    std::string error;
    vortyx::platform::Status status = vortyx::platform::validate_auth(auth, error);
    if (status != vortyx::platform::Status::Ok) return status;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::unique_ptr<DistributedJobRecord>& record : jobs_) {
        if (record->job_id == job_id) {
            if (!vortyx::platform::is_owner(auth, record->owner_user_id)) {
                return record_not_found();  // foreign -> invisible
            }
            out = *record;
            return vortyx::platform::Status::Ok;
        }
    }
    return record_not_found();
}

vortyx::platform::Status DistributedOrchestrator::jobs(const vortyx::platform::AuthContext& auth,
                                                       std::vector<DistributedJobRecord>& out) {
    std::string error;
    vortyx::platform::Status status = vortyx::platform::validate_auth(auth, error);
    if (status != vortyx::platform::Status::Ok) return status;

    std::lock_guard<std::mutex> lock(mutex_);
    out.clear();
    for (const std::unique_ptr<DistributedJobRecord>& record : jobs_) {
        if (vortyx::platform::is_owner(auth, record->owner_user_id)) {
            out.push_back(*record);
        }
    }
    return vortyx::platform::Status::Ok;
}

vortyx::platform::Status DistributedOrchestrator::deliver_cancellation_locked(
    const JobId& job_id, const UserId& requested_by, bool privileged,
    CancelDelivery delivery, DistributedJobRecord& out) {
    // Caller holds mutex_. One core for both cancellation paths: the
    // ownership path and the privileged trusted-service path differ ONLY in
    // who was authorized upstream — delivery is identical.
    for (const std::unique_ptr<DistributedJobRecord>& record : jobs_) {
        if (record->job_id != job_id) continue;
        if (distributed_job_status_is_terminal(record->status)) {
            return vortyx::platform::Status::InvalidInput;
        }
        // The cancellation REQUEST: the executing submit observes it at the
        // next wave/dispatch boundary and drives the shard transitions and
        // lease releases itself (single-writer rule: the submitting thread
        // owns the record's shards and leases).
        record->cancel_requested.store(true, std::memory_order_relaxed);
        out = *record;
        return vortyx::platform::Status::Ok;
    }

    // The record is not visible. Under RefuseUnknown this is the unchanged
    // Phase 12 contract (an unknown job is NotFound). Under RecordIntent
    // the cancellation is handed off atomically: submit() applies the
    // intent when it creates the record (under this same mutex), so no
    // polling, no sleep, no lost cancellation. The intent entry is bounded
    // — it exists only between this call and the record's creation (or the
    // job's terminal transition).
    if (delivery == CancelDelivery::RefuseUnknown) {
        return record_not_found();
    }
    CancelIntent intent;
    intent.requested_by = requested_by;
    intent.privileged = privileged;
    cancel_intents_[job_id] = std::move(intent);
    out = DistributedJobRecord();
    out.job_id = job_id;
    out.status = DistributedJobStatus::Queued;
    return vortyx::platform::Status::Ok;
}

vortyx::platform::Status DistributedOrchestrator::cancel_job(
    const vortyx::platform::AuthContext& auth, const JobId& job_id, DistributedJobRecord& out,
    CancelDelivery delivery) {
    std::string error;
    vortyx::platform::Status status = vortyx::platform::validate_auth(auth, error);
    if (status != vortyx::platform::Status::Ok) return status;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::unique_ptr<DistributedJobRecord>& record : jobs_) {
        if (record->job_id != job_id) continue;
        // THE OWNERSHIP RULE (unchanged): a foreign job is NotFound —
        // identical to what the control-plane RLS does (a foreign row is
        // invisible). Privileged cross-user cancellation goes through
        // cancel_job_privileged, never through identity swapping.
        if (!vortyx::platform::is_owner(auth, record->owner_user_id)) {
            return record_not_found();
        }
        return deliver_cancellation_locked(job_id, auth.user_id, false, delivery, out);
    }
    // Record absent: the intent path still carries the requester (the
    // dispatcher always submits under the owner's identity, so an intent
    // delivered at creation lands on the same owner's job).
    return deliver_cancellation_locked(job_id, auth.user_id, false, delivery, out);
}

vortyx::platform::Status DistributedOrchestrator::cancel_job_privileged(
    const ServiceCancellation& cancellation, const JobId& job_id, DistributedJobRecord& out,
    CancelDelivery delivery) {
    // The trusted-service contract: the SERVICE authorized this action
    // (project role table, CancelAnyJob) and audits it with the acting
    // admin's real identity. The orchestrator performs no ownership check
    // here — that is the entire point of the explicit privileged path (see
    // the module header). 'requested_by' must still be a usable id: an
    // empty actor would make the audit trail meaningless.
    if (cancellation.requested_by().empty()) {
        return vortyx::platform::Status::Unauthenticated;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return deliver_cancellation_locked(job_id, cancellation.requested_by(), true, delivery,
                                       out);
}

// ---------------------------------------------------------------------------
// Submission
// ---------------------------------------------------------------------------

vortyx::platform::Status DistributedOrchestrator::submit(
    const vortyx::platform::AuthContext& auth, const DistributedJobRequest& request,
    DistributedJobRecord& out, bool& created) {
    std::string error;
    vortyx::platform::Status status = vortyx::platform::validate_auth(auth, error);
    if (status != vortyx::platform::Status::Ok) return status;

    status = vortyx::platform::validate_job_envelope(request.envelope, error);
    if (status != vortyx::platform::Status::Ok) return status;

    // The envelope and the local payload must agree (one job, one truth).
    {
        std::string task_error;
        const vortyx::compute::Status task_status =
            vortyx::compute::validate_compute_task(request.task, task_error);
        if (task_status != vortyx::compute::Status::Ok) {
            error = "invalid compute task: " + task_error;
            return vortyx::platform::Status::InvalidInput;
        }
    }
    if (request.task.op != request.envelope.operation) {
        error = "envelope operation and task operation disagree";
        return vortyx::platform::Status::InvalidInput;
    }
    if (request.task.element_count() != request.envelope.element_count) {
        error = "envelope element_count and task element count disagree";
        return vortyx::platform::Status::InvalidInput;
    }
    if (request.requested_shard_count == 0) {
        error = "requested_shard_count must be at least 1";
        return vortyx::platform::Status::InvalidInput;
    }
    if (request.requested_shard_count > config_.max_shards_per_job) {
        error = "requested_shard_count exceeds max_shards_per_job (" +
                std::to_string(config_.max_shards_per_job) + ")";
        return vortyx::platform::Status::InvalidInput;
    }
    // Every shard id must be derivable within the id rules (the LAST index
    // is the longest — checking it covers all).
    std::string probe;
    if (make_shard_id(request.envelope.job_id, request.requested_shard_count - 1, probe, error) !=
        vortyx::platform::Status::Ok) {
        return vortyx::platform::Status::InvalidInput;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::unique_ptr<DistributedJobRecord>& existing : jobs_) {
            if (existing->job_id != request.envelope.job_id) continue;
            // Idempotency rule (the Phase 11 submission semantics).
            const auto stored = requests_.find(existing->job_id);
            if (existing->owner_user_id == auth.user_id && stored != requests_.end() &&
                same_request(request, stored->second)) {
                out = *existing;
                created = false;
                return vortyx::platform::Status::Ok;
            }
            error = "job_id is already used with a different owner or payload";
            return vortyx::platform::Status::Conflict;
        }

        std::unique_ptr<DistributedJobRecord> record = std::make_unique<DistributedJobRecord>();
        record->job_id = request.envelope.job_id;
        record->owner_user_id = auth.user_id;
        record->operation = request.envelope.operation;
        record->element_count = request.envelope.element_count;
        record->requested_backend = request.envelope.requested_backend;
        record->requested_shard_count = request.requested_shard_count;
        record->status = DistributedJobStatus::Queued;
        record->created_at_ms = deps_.clock->now_ms();
        record->platform_status = vortyx::platform::JobStatus::Queued;
        // Cancellation-intent handoff (Phase 15): a cancel that arrived
        // during the record-creation window is applied atomically HERE,
        // under the same mutex that creates the record — the executing run
        // observes the flag at its first wave boundary and dispatches zero
        // shards. This replaces the Phase 14 sleep-poll handoff.
        const auto intent = cancel_intents_.find(request.envelope.job_id);
        if (intent != cancel_intents_.end()) {
            record->cancel_requested.store(true, std::memory_order_relaxed);
            cancel_intents_.erase(intent);
        }
        jobs_.push_back(std::move(record));
        requests_[request.envelope.job_id] = request;
    }

    DistributedJobRecord& job = *jobs_.back();
    created = true;

    // Platform mirror: the submission exists in the control plane as a
    // queued job (idempotent there too). Failures are recorded on the
    // record — never swallowed, never fatal to local execution.
    if (deps_.platform_store != nullptr) {
        vortyx::platform::JobRecord store_record;
        bool store_created = false;
        const vortyx::platform::Status store_status = deps_.platform_store->create_job(
            auth, request.envelope, std::nullopt, store_record, store_created);
        if (store_status != vortyx::platform::Status::Ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            job.platform_error =
                "create_job: " + std::string(vortyx::platform::to_string(store_status));
        }
    }

    run_job(job, auth);

    out = job;
    return vortyx::platform::Status::Ok;
}

// ---------------------------------------------------------------------------
// The execution pipeline
// ---------------------------------------------------------------------------

void DistributedOrchestrator::run_job(DistributedJobRecord& job,
                                      const vortyx::platform::AuthContext& auth) {
    // A cancellation delivered during the record-creation window (the Phase
    // 15 intent path sets the flag atomically at creation) takes effect
    // BEFORE any placement or dispatch: zero shards run. The
    // Queued -> Cancelled transition is the documented table.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (job.cancel_requested.load(std::memory_order_relaxed)) {
            job.status = DistributedJobStatus::Cancelled;
            apply_terminal(job);
            mirror_terminal(auth, job);
            return;
        }
    }

    // Queued -> Planning (sharding/placement begins).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job.status = DistributedJobStatus::Planning;
    }

    std::string reason;
    if (!place_new_shards(job, reason)) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Planning -> Failed (legal): the job is rejected with its stable
        // reason. Nothing ran; nothing holds a lease.
        job.status = DistributedJobStatus::Failed;
        job.error = "placement rejected: " + reason;
        apply_terminal(job);
        mirror_terminal(auth, job);
        return;
    }

    bool cancelled = false;
    while (true) {
        // Scheduled/Planning -> Running for the wave.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (distributed_job_transition_valid(job.status, DistributedJobStatus::Scheduled)) {
                job.status = DistributedJobStatus::Scheduled;
            }
            if (distributed_job_transition_valid(job.status, DistributedJobStatus::Running)) {
                job.status = DistributedJobStatus::Running;
            }
            mirror_running_once(auth, job);
        }

        execute_wave(job, auth);

        // Collect retrying shards / cancellation.
        std::vector<std::size_t> retrying;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (job.cancel_requested.load(std::memory_order_relaxed)) cancelled = true;
            if (!cancelled) {
                for (std::size_t i = 0; i < job.shards.size(); ++i) {
                    if (job.shards[i].state == ShardState::Retrying) retrying.push_back(i);
                }
            }
        }

        if (cancelled) {
            std::lock_guard<std::mutex> lock(mutex_);
            for (JobShard& shard : job.shards) {
                if (shard.state == ShardState::Assigned || shard.state == ShardState::Pending ||
                    shard.state == ShardState::Retrying) {
                    release_shard_lease(job, shard);
                    shard.assigned_device.clear();
                    shard.assigned_lease_id.clear();
                    shard.state = ShardState::Cancelled;
                }
            }
            break;
        }
        if (retrying.empty()) break;

        // Running -> Planning (re-placement). Succeeded shards are never
        // touched (checkpoint semantics).
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (distributed_job_transition_valid(job.status, DistributedJobStatus::Planning)) {
                job.status = DistributedJobStatus::Planning;
            }
        }
        bool any_replaced = false;
        for (std::size_t index : retrying) {
            JobShard& shard = job.shards[index];  // single writer: this submit
            std::string replace_error;
            if (replace_shard(job, shard, replace_error)) {
                any_replaced = true;
            } else {
                // Retry placement impossible: terminal failure of the shard.
                shard.state = ShardState::Failed;
                if (shard.last_error.empty()) shard.last_error = replace_error;
            }
        }
        if (!any_replaced) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (distributed_job_transition_valid(job.status, DistributedJobStatus::Running)) {
                job.status = DistributedJobStatus::Running;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        apply_terminal(job);
        mirror_terminal(auth, job);
        // The execution bookkeeping (aggregator, per-shard leases) dies
        // with the terminal state. The request FINGERPRINT is retained for
        // the orchestrator's lifetime: the submission idempotency rule
        // compares the original payload on every resubmission.
        aggregators_.erase(job.job_id);
        for (const JobShard& shard : job.shards) shard_leases_.erase(shard.shard_id);
    }
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

bool DistributedOrchestrator::place_new_shards(DistributedJobRecord& job, std::string& reason) {
    for (std::uint32_t attempt = 0; attempt < kMaxPlanAttempts; ++attempt) {
        // Freshness before planning: stale devices are judged Offline so a
        // dead device is never the target of a fresh plan.
        heartbeat_->check(job.owner_user_id);

        const ClusterSnapshot snapshot = deps_.registry->snapshot();

        PlacementRequest request;
        request.job_id = job.job_id;
        request.owner_user_id = job.owner_user_id;
        request.operation = job.operation;
        request.requested_backend = job.requested_backend;
        request.element_count = job.element_count;
        request.requested_shard_count = job.requested_shard_count;
        request.allow_fallback = config_.allow_single_device_fallback;

        const PlacementPlan plan = policy_->plan(request, snapshot);
        if (!plan.accepted) {
            reason = std::string(to_string(plan.rejection)) + ": " + plan.message;
            return false;  // a stable rejection is FINAL (no re-plan)
        }
        if (plan.cluster_revision != deps_.registry->revision()) {
            continue;  // stale plan — the cluster moved; re-plan
        }

        // Atomic reservation of every shard. A failure anywhere unwinds
        // the guards (RAII) and re-plans; the capacity race is thereby
        // handled, never forced through.
        std::vector<LeaseGuard> guards;
        guards.reserve(plan.shards.size());
        bool all_reserved = true;
        std::string reserve_error;
        for (const ShardPlan& entry : plan.shards) {
            std::string shard_id;
            if (make_shard_id(job.job_id, entry.shard_index, shard_id, reserve_error) !=
                vortyx::platform::Status::Ok) {
                all_reserved = false;
                reserve_error = "shard id derivation failed: " + reserve_error;
                break;
            }
            DeviceLease lease;
            if (deps_.registry->reserve(job.owner_user_id, entry.device_id, job.job_id, shard_id,
                                        entry.resources, config_.lease_ttl_ms, lease,
                                        reserve_error) != vortyx::platform::Status::Ok) {
                all_reserved = false;
                reserve_error = "reservation failed on device '" + entry.device_id +
                                "': " + reserve_error;
                break;
            }
            guards.emplace_back(deps_.registry, lease);
        }
        if (!all_reserved) {
            guards.clear();  // RAII releases every lease taken so far
            reason = reserve_error;
            continue;  // re-plan
        }

        // Committed: materialize the shard records (Assigned) and take
        // over the leases (detach from the RAII guards).
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job.shards.clear();
            aggregators_.erase(job.job_id);
            aggregators_.emplace(job.job_id,
                                 ResultAggregator(job.job_id,
                                                  static_cast<std::uint32_t>(plan.shards.size()),
                                                  job.element_count));
            for (std::size_t i = 0; i < plan.shards.size(); ++i) {
                const ShardPlan& entry = plan.shards[i];
                std::string local_error;
                JobShard shard;
                make_shard_id(job.job_id, entry.shard_index, shard.shard_id, local_error);
                shard.parent_job_id = job.job_id;
                shard.index = entry.shard_index;
                shard.work.kind = PartitionKind::ElementRange;
                shard.work.element_range = entry.range;
                shard.assigned_device = entry.device_id;
                shard.assigned_lease_id = guards[i].lease().lease_id;
                shard.state = ShardState::Assigned;
                shard_leases_[shard.shard_id] = guards[i].detach();
                job.shards.push_back(std::move(shard));
            }
        }
        return true;
    }
    reason = "placement kept failing while the cluster changed (" + reason + ")";
    return false;
}

bool DistributedOrchestrator::replace_shard(DistributedJobRecord& job, JobShard& shard,
                                            std::string& reason) {
    // A retrying shard keeps its ORIGINAL range; the re-placement request
    // asks for one shard of that range's SIZE and re-bases the result.
    // The device the shard just failed on is EXCLUDED — a retry runs
    // elsewhere whenever another capable device exists.
    for (std::uint32_t attempt = 0; attempt < kMaxPlanAttempts; ++attempt) {
        const ClusterSnapshot snapshot = deps_.registry->snapshot();

        PlacementRequest request;
        request.job_id = job.job_id;
        request.owner_user_id = job.owner_user_id;
        request.operation = job.operation;
        request.requested_backend = job.requested_backend;
        request.element_count = shard.work.element_range.size();
        request.requested_shard_count = 1;
        request.allow_fallback = true;  // a retry takes any single device
        if (!shard.assigned_device.empty()) {
            request.excluded_devices.push_back(shard.assigned_device);
        }

        const PlacementPlan plan = policy_->plan(request, snapshot);
        if (!plan.accepted) {
            reason = std::string(to_string(plan.rejection)) + ": " + plan.message;
            return false;
        }
        if (plan.cluster_revision != deps_.registry->revision()) continue;

        DeviceLease lease;
        if (deps_.registry->reserve(job.owner_user_id, plan.shards.front().device_id, job.job_id,
                                    shard.shard_id, plan.shards.front().resources,
                                    config_.lease_ttl_ms, lease, reason) !=
            vortyx::platform::Status::Ok) {
            reason = "retry reservation failed on device '" + plan.shards.front().device_id +
                     "': " + reason;
            continue;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        shard.assigned_device = plan.shards.front().device_id;
        shard.assigned_lease_id = lease.lease_id;
        shard_leases_[shard.shard_id] = lease;
        shard.state = ShardState::Assigned;  // Retrying -> Assigned (the table)
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

void DistributedOrchestrator::execute_wave(DistributedJobRecord& job,
                                           const vortyx::platform::AuthContext& auth) {
    // Snapshot the dispatch list under the lock, mark those shards Running,
    // then release the lock for the actual submits (the transport and the
    // workers own their own synchronization; the orchestrator's mutex is
    // never held across an execution).
    std::vector<std::size_t> indices;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < job.shards.size(); ++i) {
            if (job.shards[i].state == ShardState::Assigned) {
                indices.push_back(i);
                job.shards[i].state = ShardState::Running;
            }
        }
    }
    if (indices.empty()) return;

    if (config_.shard_threads <= 1) {
        // Sequential: cancellation is honored before EACH dispatch.
        for (std::size_t index : indices) {
            if (job.cancel_requested.load(std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lock(mutex_);
                for (JobShard& shard : job.shards) {
                    if (shard.state == ShardState::Running || shard.state == ShardState::Assigned ||
                        shard.state == ShardState::Pending || shard.state == ShardState::Retrying) {
                        release_shard_lease(job, shard);
                        shard.assigned_device.clear();
                        shard.assigned_lease_id.clear();
                        shard.state = ShardState::Cancelled;
                    }
                }
                return;
            }
            dispatch_and_apply(job, job.shards[index], auth);
        }
        return;
    }

    // Threaded: one thread per dispatched shard (workers serialize their
    // own runtimes; different devices execute in parallel). Cancellation
    // is honored at wave boundaries in this mode.
    const std::uint32_t threads = config_.shard_threads;
    std::vector<ShardResult> results(indices.size());
    std::vector<std::thread> pool;
    std::size_t next = 0;
    std::mutex next_mutex;

    auto worker_loop = [&]() {
        while (true) {
            std::size_t slot = 0;
            {
                std::lock_guard<std::mutex> lock(next_mutex);
                if (next >= indices.size()) return;
                slot = next++;
            }
            results[slot] = build_and_submit(job, job.shards[indices[slot]]);
        }
    };

    const std::uint32_t spawn =
        threads < indices.size() ? threads : static_cast<std::uint32_t>(indices.size());
    for (std::uint32_t i = 0; i < spawn; ++i) {
        pool.emplace_back(worker_loop);
    }
    for (std::thread& worker : pool) worker.join();

    // Apply results under the lock (record mutation is single-writer +
    // locked; jobs_ addresses are stable so the shard references hold).
    for (std::size_t s = 0; s < results.size(); ++s) {
        apply_result(job, job.shards[indices[s]], results[s]);
    }
}

ShardResult DistributedOrchestrator::build_and_submit(const DistributedJobRecord& job,
                                                      const JobShard& shard) {
    // Copies the execution input under the orchestrator's lock, submits
    // WITHOUT it (the transport/worker path is long-running and has its
    // own synchronization).
    ShardExecution execution;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto stored = requests_.find(job.job_id);
        if (stored == requests_.end()) {
            ShardResult failed;
            failed.shard_id = shard.shard_id;
            failed.parent_job_id = shard.parent_job_id;
            failed.shard_index = shard.index;
            failed.device_id = shard.assigned_device;
            failed.completed = false;
            failed.failure_code = FailureCode::InvalidAssignment;
            failed.error = "job payload vanished (internal error)";
            return failed;
        }
        execution.shard_id = shard.shard_id;
        execution.parent_job_id = shard.parent_job_id;
        execution.shard_index = shard.index;
        execution.attempt = shard.attempt + 1;
        execution.device_id = shard.assigned_device;
        execution.work = shard.work;
        execution.task = stored->second.task;
        execution.backend = job.requested_backend;
        execution.lease_id = shard.assigned_lease_id;
    }
    return deps_.transport->submit_shard(execution);
}

void DistributedOrchestrator::dispatch_and_apply(DistributedJobRecord& job, JobShard& shard,
                                                 const vortyx::platform::AuthContext& auth) {
    (void)auth;
    const ShardResult result = build_and_submit(job, shard);
    apply_result(job, shard, result);
}

void DistributedOrchestrator::apply_result(DistributedJobRecord& job, JobShard& shard,
                                           const ShardResult& result) {
    // ALL record mutation happens under the orchestrator's mutex (query
    // threads may read the record concurrently). The registry call inside
    // release_shard_lease takes only the registry's own lock — the nesting
    // orchestrator -> registry is one-directional, so no deadlock exists.
    std::lock_guard<std::mutex> lock(mutex_);

    shard.attempt = result.attempt;

    // The execution attempt is over: its lease goes back regardless of the
    // outcome (an expired lease was reclaimed lazily already). On a
    // FAILURE the device id is KEPT on the shard — the retry's re-placement
    // excludes it — and cleared by the successful re-placement, the cancel
    // path, or terminal application.
    release_shard_lease(job, shard);
    shard.assigned_lease_id.clear();
    if (result.completed) shard.assigned_device.clear();

    const auto aggregator_it = aggregators_.find(job.job_id);

    // A failed attempt that will be retried is NOT a verdict: the
    // aggregator's slot stays open for the attempt that actually settles
    // the shard (otherwise a retry's success would be misread as a
    // duplicate of the earlier failure).
    bool retryable = is_retryable(result.failure_code);
    if (retry_.classify != nullptr) retryable = retry_.classify(result.failure_code);
    const bool will_retry =
        !result.completed && retryable && retry_permitted(retry_, result.attempt);

    if (!will_retry) {
        const AggregateOutcome outcome = aggregator_it == aggregators_.end()
                                             ? AggregateOutcome::Unexpected
                                             : aggregator_it->second.accept(result);
        if (outcome == AggregateOutcome::Duplicate) {
            // A second report for a shard that already has its verdict:
            // keep the first verdict, count the duplicate, change nothing.
            shard.last_failure_code = std::string(to_string(FailureCode::DuplicateResult));
            shard.last_error = "duplicate result ignored (first verdict kept)";
            return;
        }
        if (outcome == AggregateOutcome::Unexpected) {
            shard.state = ShardState::Failed;
            shard.last_failure_code = std::string(to_string(FailureCode::InvalidAssignment));
            shard.last_error = "result for a shard outside the plan";
            return;
        }
    }

    if (result.completed) {
        shard.state = ShardState::Completed;
        return;
    }

    // Failure path: classify, retry or terminate (never hide).
    shard.last_failure_code = std::string(to_string(result.failure_code));
    shard.last_error = result.error;
    shard.retry_count = result.attempt > 0 ? result.attempt - 1 : 0;

    if (will_retry) {
        shard.state = ShardState::Retrying;
        // Backoff stamp (observability; the synchronous executor re-places
        // immediately and does not block — the delay itself is a pure,
        // unit-tested function).
        shard.next_attempt_eligible_ms =
            deps_.clock->now_ms() + retry_delay_ms(retry_, result.attempt);
        return;
    }
    shard.state = ShardState::Failed;
}

// ---------------------------------------------------------------------------
// Terminal application / aggregation / leases
// ---------------------------------------------------------------------------

void DistributedOrchestrator::apply_terminal(DistributedJobRecord& job) {
    // Caller holds mutex_.
    //
    // A status that is ALREADY terminal (e.g. set by the placement-rejection
    // path with its explicit stable reason) is final: deriving from an
    // empty/failed shard list must never overwrite it.
    if (!distributed_job_status_is_terminal(job.status)) {
        const DistributedJobStatus derived = derive_job_status(job.shards);
        for (DistributedJobStatus candidate :
             {DistributedJobStatus::Planning, DistributedJobStatus::Scheduled,
              DistributedJobStatus::Running}) {
            if (job.status == derived) break;
            if (distributed_job_transition_valid(job.status, derived)) {
                job.status = derived;
                break;
            }
            if (distributed_job_transition_valid(job.status, candidate)) {
                job.status = candidate;
            }
        }
        if (job.status != derived) {
            // Unreachable in the documented flow; if a future change breaks
            // the walk, the mismatch is recorded, never hidden.
            job.error = "internal: derived status '" + std::string(to_string(derived)) +
                        "' is not reachable from '" + std::string(to_string(job.status)) + "'";
            job.status = DistributedJobStatus::Failed;
        }
    }

    if (distributed_job_status_is_terminal(job.status)) {
        job.completed_at_ms = deps_.clock->now_ms();
        job.result = assemble_result(job);
        if (job.status == DistributedJobStatus::Failed && job.error.empty()) {
            // A failed job always carries its reason.
            job.error = std::to_string(job.result.failed) + " of " +
                        std::to_string(job.result.shard_count) + " shards failed";
        }
        // The job is terminal: any cancellation intent still attached to it
        // is obsolete (the terminal state won the race) and is dropped —
        // the intent map only ever tracks in-flight submissions.
        cancel_intents_.erase(job.job_id);
    }
}

DistributedResult DistributedOrchestrator::assemble_result(const DistributedJobRecord& job) {
    // Caller holds mutex_ (read-only here).
    const auto it = aggregators_.find(job.job_id);
    if (it != aggregators_.end()) return it->second.assemble();
    // No aggregator (the job never produced a plan — e.g. placement
    // rejected at Planning): an honest empty aggregate.
    DistributedResult empty;
    empty.job_id = job.job_id;
    empty.completed = false;
    empty.shard_count = static_cast<std::uint32_t>(job.shards.size());
    return empty;
}

void DistributedOrchestrator::release_shard_lease(const DistributedJobRecord& job,
                                                  JobShard& shard) {
    // Caller holds mutex_ (or the single-writer sequential path).
    if (shard.assigned_lease_id.empty()) return;
    const auto it = shard_leases_.find(shard.shard_id);
    if (it != shard_leases_.end()) {
        deps_.registry->release_lease(it->second);
        shard_leases_.erase(it);
    }
    (void)job;
}

// ---------------------------------------------------------------------------
// Platform-store mirroring
// ---------------------------------------------------------------------------

void DistributedOrchestrator::mirror_status(const vortyx::platform::AuthContext& auth,
                                            DistributedJobRecord& job,
                                            vortyx::platform::JobStatus to,
                                            const std::string& error_reason) {
    // Caller holds mutex_ when mutating job fields; the store call itself
    // runs WITHOUT this orchestrator's lock held (the store has its own).
    if (deps_.platform_store == nullptr) return;
    if (to == job.platform_status) return;  // no redundant transition

    vortyx::platform::JobRecord store_record;
    const vortyx::platform::Status status =
        deps_.platform_store->update_job(auth, job.job_id, to, error_reason, store_record);
    if (status != vortyx::platform::Status::Ok) {
        const std::string failure = std::string("update_job(") + vortyx::platform::to_string(to) +
                                    "): " + vortyx::platform::to_string(status);
        job.platform_error = job.platform_error.empty() ? failure : job.platform_error + "; " + failure;
        return;
    }
    job.platform_status = to;
}

void DistributedOrchestrator::mirror_running_once(const vortyx::platform::AuthContext& auth,
                                                  DistributedJobRecord& job) {
    // Caller holds mutex_.
    if (deps_.platform_store == nullptr) return;
    if (job.platform_status != vortyx::platform::JobStatus::Queued) return;
    vortyx::platform::JobRecord store_record;
    const vortyx::platform::Status status =
        deps_.platform_store->update_job(auth, job.job_id, vortyx::platform::JobStatus::Running,
                                         "", store_record);
    if (status != vortyx::platform::Status::Ok) {
        const std::string failure = std::string("update_job(running): ") +
                                    vortyx::platform::to_string(status);
        job.platform_error =
            job.platform_error.empty() ? failure : job.platform_error + "; " + failure;
        return;
    }
    job.platform_status = vortyx::platform::JobStatus::Running;
}

void DistributedOrchestrator::mirror_terminal(const vortyx::platform::AuthContext& auth,
                                              DistributedJobRecord& job) {
    // Caller holds mutex_.
    if (deps_.platform_store == nullptr) return;

    const vortyx::platform::JobStatus mapped = map_to_platform_job_status(job.status);

    // The store's put_result TRANSITIONS a Running job to its terminal
    // status while recording the outcome — so for Completed/Failed the
    // result goes FIRST and the status update is implicit. Cancellation is
    // an owner action with no result envelope (the Phase 11 rule).
    if (mapped == vortyx::platform::JobStatus::Completed ||
        mapped == vortyx::platform::JobStatus::Failed) {
        vortyx::platform::ResultEnvelope envelope;
        envelope.job_id = job.job_id;
        envelope.status = mapped;
        envelope.backend = job.result.backends_used.empty() ? std::string()
                                                            : job.result.backends_used.front();
        envelope.error = mapped == vortyx::platform::JobStatus::Failed ? job.error : std::string();
        envelope.result_element_count =
            mapped == vortyx::platform::JobStatus::Completed
                ? std::optional<std::uint64_t>(job.element_count)
                : std::optional<std::uint64_t>();
        vortyx::platform::ResultEnvelope stored;
        const vortyx::platform::Status status =
            deps_.platform_store->put_result(auth, envelope, stored);
        if (status != vortyx::platform::Status::Ok) {
            const std::string failure =
                std::string("put_result: ") + vortyx::platform::to_string(status);
            job.platform_error =
                job.platform_error.empty() ? failure : job.platform_error + "; " + failure;
            // The status still moves (the outcome is known even when the
            // result record failed) — reported, never silent.
            mirror_status(auth, job, mapped,
                          mapped == vortyx::platform::JobStatus::Failed ? job.error : "");
            return;
        }
        job.platform_status = mapped;
        return;
    }

    mirror_status(auth, job, mapped,
                  mapped == vortyx::platform::JobStatus::Cancelled ? "cancelled" : "");
}

}  // namespace vortyx::distributed
