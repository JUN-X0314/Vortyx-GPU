// PlatformService (Phase 14) — implementation.

#include "service/platform_service.hpp"

#include <algorithm>
#include <chrono>

#include "distributed/resource.hpp"  // shard_memory_bytes (the honest bytes)

namespace vortyx::service {

using vortyx::distributed::DistributedConfig;
using vortyx::distributed::DistributedJobRecord;
using vortyx::distributed::DistributedOrchestrator;
using vortyx::distributed::DistributedJobStatus;
using vortyx::platform::AuthContext;
using vortyx::platform::JobId;
using vortyx::platform::Status;

namespace {

bool envelope_equal(const vortyx::platform::JobEnvelope& a,
                    const vortyx::platform::JobEnvelope& b) {
    return a.job_id == b.job_id && a.operation == b.operation &&
           a.element_count == b.element_count && a.requested_backend == b.requested_backend &&
           a.priority == b.priority && a.protocol_version == b.protocol_version;
}

AuditOutcome outcome_for(ServiceStatus status) {
    if (status == ServiceStatus::Ok) return AuditOutcome::Ok;
    if (status == ServiceStatus::Forbidden || status == ServiceStatus::Unauthenticated ||
        status == ServiceStatus::QuotaExceeded || status == ServiceStatus::RateLimitExceeded) {
        return AuditOutcome::Denied;
    }
    return AuditOutcome::Error;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ServiceStatus PlatformService::create(Deps deps, const PlatformServiceConfig& config,
                                      std::unique_ptr<PlatformService>& out, std::string& error) {
    if (deps.registry == nullptr || deps.transport == nullptr) {
        error = "platform service requires the distributed registry and transport "
                "(the existing Phase 12 cluster is the execution world)";
        return ServiceStatus::InvalidInput;
    }
    if (!deps.clock) {
        error = "platform service requires an injected clock (determinism by injection)";
        return ServiceStatus::InvalidInput;
    }
    if (config.dispatcher_count < 1 || config.dispatcher_count > 8) {
        error = "dispatcher_count must be 1..8";
        return ServiceStatus::InvalidInput;
    }
    if (config.max_queue_depth < 1) {
        error = "max_queue_depth must be at least 1";
        return ServiceStatus::InvalidInput;
    }
    if (config.max_requested_shard_count < 1) {
        error = "max_requested_shard_count must be at least 1";
        return ServiceStatus::InvalidInput;
    }
    {
        DistributedConfig distributed = deps.distributed_config;
        if (distributed.validate(error) != Status::Ok) {
            error = "invalid distributed configuration: " + error;
            return ServiceStatus::InvalidInput;
        }
    }

    // The orchestrator (the EXISTING Phase 12 facade) — the service never
    // builds a second scheduler.
    vortyx::distributed::DistributedOrchestrator::Deps orchestrator_deps;
    orchestrator_deps.registry = deps.registry;
    orchestrator_deps.transport = deps.transport;
    orchestrator_deps.clock = deps.clock;
    orchestrator_deps.platform_store = deps.platform_store;
    std::unique_ptr<vortyx::distributed::DistributedOrchestrator> orchestrator;
    if (DistributedOrchestrator::create(std::move(orchestrator_deps), deps.distributed_config,
                                        orchestrator, error) != Status::Ok) {
        error = "orchestrator construction failed: " + error;
        return ServiceStatus::InvalidInput;
    }

    std::unique_ptr<PlatformService> service(new PlatformService());
    service->deps_ = std::move(deps);
    service->config_ = config;
    service->orchestrator_ = std::move(orchestrator);

    // Providers: the injected ones, or the owned in-memory references.
    service->projects_ = service->deps_.project_store;
    if (service->projects_ == nullptr) {
        auto store = std::make_unique<InMemoryProjectStore>();
        store->set_clock(service->deps_.clock);
        service->owned_projects_ = std::move(store);
        service->projects_ = service->owned_projects_.get();
    }
    service->queue_ = service->deps_.job_queue;
    if (service->queue_ == nullptr) {
        service->owned_queue_ = std::make_unique<InMemoryJobQueue>(config.max_queue_depth);
        service->queue_ = service->owned_queue_.get();
    }
    service->audit_store_ = service->deps_.audit_store;
    if (service->audit_store_ == nullptr) {
        service->owned_audit_ = std::make_unique<InMemoryAuditStore>(config.audit_max_entries);
        service->audit_store_ = service->owned_audit_.get();
    }
    service->artifacts_store_ = service->deps_.artifact_store;
    if (service->artifacts_store_ == nullptr) {
        auto store = std::make_unique<InMemoryArtifactStore>();
        store->set_clock(service->deps_.clock);
        service->owned_artifacts_ = std::move(store);
        service->artifacts_store_ = service->owned_artifacts_.get();
    }
    // The audit trail shares the store (non-owning alias — the service owns
    // the lifetime).
    service->audit_ = std::make_unique<AuditTrail>(
        std::shared_ptr<IAuditStore>(std::shared_ptr<IAuditStore>(), service->audit_store_),
        service->deps_.clock);
    service->quota_.set_default_quota(config.default_project_quota);
    service->rate_limiter_ = std::make_unique<RateLimiter>(
        service->deps_.clock, config.rate_limit_max_submissions, config.rate_limit_window_ms);

    // Dispatcher threads (the queue drain). The enqueue path notifies them.
    for (std::uint32_t i = 0; i < config.dispatcher_count; ++i) {
        service->dispatchers_.emplace_back([service_ptr = service.get()]() {
            service_ptr->dispatcher_loop();
        });
    }

    out = std::move(service);
    return ServiceStatus::Ok;
}

PlatformService::~PlatformService() {
    {
        std::lock_guard<std::mutex> lock(state_);
        stopping_ = true;
        terminal_cv_.notify_all();
    }
    for (std::thread& dispatcher : dispatchers_) {
        if (dispatcher.joinable()) dispatcher.join();
    }
}

// ---------------------------------------------------------------------------
// Projects
// ---------------------------------------------------------------------------

ServiceStatus PlatformService::create_project(const AuthContext& auth, const std::string& name,
                                              ProjectRecord& out) {
    ProjectRecord request;
    request.name = name;
    const ServiceStatus status = projects_->create_project(auth, request, out);
    audit_->record(auth.user_id, status == ServiceStatus::Ok ? out.project_id : std::string(), "",
                   AuditAction::ProjectCreate, outcome_for(status),
                   status == ServiceStatus::Ok ? "" : service_status_code(status));
    return status;
}

ServiceStatus PlatformService::project(const AuthContext& auth, const ProjectId& project_id,
                                       ProjectRecord& out) {
    return projects_->project(auth, project_id, out);
}

ServiceStatus PlatformService::projects(const AuthContext& auth, std::vector<ProjectRecord>& out) {
    return projects_->projects(auth, out);
}

ServiceStatus PlatformService::archive_project(const AuthContext& auth,
                                               const ProjectId& project_id, ProjectRecord& out) {
    const ServiceStatus status = projects_->archive_project(auth, project_id, out);
    audit_->record(auth.user_id, project_id, "", AuditAction::ProjectArchive, outcome_for(status),
                   status == ServiceStatus::Ok ? "" : service_status_code(status));
    return status;
}

ServiceStatus PlatformService::add_member(const AuthContext& auth, const ProjectId& project_id,
                                          const vortyx::platform::UserId& user_id,
                                          ProjectRole role, ProjectMember& out) {
    // Defense in depth (the store re-checks): the single-owner invariant
    // refuses an Owner grant before the store ever sees the request.
    if (!project_role_grantable(role)) {
        audit_->record(auth.user_id, project_id, "", AuditAction::MembershipChange,
                       AuditOutcome::Denied, service_status_code(ServiceStatus::InvalidInput));
        return ServiceStatus::InvalidInput;
    }
    const ServiceStatus status = projects_->add_member(auth, project_id, user_id, role, out);
    audit_->record(auth.user_id, project_id, "", AuditAction::MembershipChange,
                   outcome_for(status),
                   status == ServiceStatus::Ok ? to_string(role) : service_status_code(status));
    return status;
}

ServiceStatus PlatformService::remove_member(const AuthContext& auth, const ProjectId& project_id,
                                             const vortyx::platform::UserId& user_id) {
    const ServiceStatus status = projects_->remove_member(auth, project_id, user_id);
    audit_->record(auth.user_id, project_id, "", AuditAction::MembershipChange,
                   outcome_for(status),
                   status == ServiceStatus::Ok ? "remove" : service_status_code(status));
    return status;
}

ServiceStatus PlatformService::members(const AuthContext& auth, const ProjectId& project_id,
                                       std::vector<ProjectMember>& out) {
    return projects_->members(auth, project_id, out);
}

ServiceStatus PlatformService::set_project_quota(const AuthContext& auth,
                                                 const ProjectId& project_id,
                                                 const ProjectQuota& quota) {
    ProjectRole role = ProjectRole::Viewer;
    ServiceStatus status = projects_->role_of(auth, project_id, role);
    if (status != ServiceStatus::Ok) return status;
    if (authorize_project_action(role, ProjectAction::ChangeQuota) != ServiceStatus::Ok) {
        audit_->record(auth.user_id, project_id, "", AuditAction::QuotaChange,
                       AuditOutcome::Denied, service_status_code(ServiceStatus::Forbidden));
        return ServiceStatus::Forbidden;
    }
    if (quota.max_concurrent_jobs < 0 || quota.max_running_shards < 0 ||
        quota.max_memory_bytes < 0) {
        return ServiceStatus::InvalidInput;
    }
    quota_.set_quota(project_id, quota);
    audit_->record(auth.user_id, project_id, "", AuditAction::QuotaChange, AuditOutcome::Ok, "");
    return ServiceStatus::Ok;
}

ServiceStatus PlatformService::project_usage(const AuthContext& auth, const ProjectId& project_id,
                                             QuotaUsage& out) {
    ProjectRole role = ProjectRole::Viewer;
    const ServiceStatus status = projects_->role_of(auth, project_id, role);
    if (status != ServiceStatus::Ok) return status;
    if (authorize_project_action(role, ProjectAction::ViewUsage) != ServiceStatus::Ok) {
        return ServiceStatus::Forbidden;
    }
    out = quota_.usage(project_id);
    return ServiceStatus::Ok;
}

ServiceStatus PlatformService::project_role(const AuthContext& auth, const ProjectId& project_id,
                                            ProjectRole& out) const {
    return projects_->role_of(auth, project_id, out);
}

// ---------------------------------------------------------------------------
// Jobs — the submission flow
// ---------------------------------------------------------------------------

ServiceStatus PlatformService::submit_job(const AuthContext& auth, const SubmitJobRequest& request,
                                          ServiceJobView& out, bool& created) {
    created = false;
    std::string error;
    metrics_.inc_submit_attempts();

    // 1. Authentication.
    if (vortyx::platform::validate_auth(auth, error) != Status::Ok) {
        return ServiceStatus::Unauthenticated;
    }

    // 2. Request validation (envelope rules are the Phase 11 contract; the
    //    shard cap is the service-level resource limit on top of the Phase
    //    13 tensor shape guards and the Phase 11 element-count cap).
    if (request.project_id.empty()) {
        error = "project_id is required";
        return ServiceStatus::InvalidInput;
    }
    if (vortyx::platform::validate_job_envelope(request.distributed.envelope, error) !=
        Status::Ok) {
        return ServiceStatus::InvalidInput;
    }
    if (request.distributed.requested_shard_count == 0 ||
        request.distributed.requested_shard_count > config_.max_requested_shard_count) {
        error = "requested_shard_count must be 1.." +
                std::to_string(config_.max_requested_shard_count);
        return ServiceStatus::InvalidInput;
    }
    if (vortyx::compute::validate_compute_task(request.distributed.task, error) !=
        vortyx::compute::Status::Ok) {
        error = "invalid compute task: " + error;
        return ServiceStatus::InvalidInput;
    }
    if (request.distributed.task.op != request.distributed.envelope.operation) {
        error = "envelope operation and task operation disagree";
        return ServiceStatus::InvalidInput;
    }
    if (request.distributed.task.element_count() != request.distributed.envelope.element_count) {
        error = "envelope element_count and task element count disagree";
        return ServiceStatus::InvalidInput;
    }

    // 3./4. Project validation + authorization (Member+ to submit; the
    //    store applies the anti-enumeration rule: foreign = NotFound).
    ProjectRole role = ProjectRole::Viewer;
    ServiceStatus status = projects_->role_of(auth, request.project_id, role);
    if (status != ServiceStatus::Ok) {
        return status;  // Unauthenticated / NotFound
    }
    if (authorize_project_action(role, ProjectAction::SubmitJob) != ServiceStatus::Ok) {
        audit_->record(auth.user_id, request.project_id, request.distributed.envelope.job_id,
                       AuditAction::JobSubmit, AuditOutcome::Denied,
                       service_status_code(ServiceStatus::Forbidden));
        return ServiceStatus::Forbidden;
    }

    ProjectRecord project;
    if (projects_->project(auth, request.project_id, project) != ServiceStatus::Ok) {
        return ServiceStatus::NotFound;
    }
    if (project.status == ProjectStatus::Archived) {
        audit_->record(auth.user_id, request.project_id, request.distributed.envelope.job_id,
                       AuditAction::JobSubmit, AuditOutcome::Denied, "project_archived");
        error = "the project is archived; submissions are refused";
        return ServiceStatus::UnsupportedOperation;
    }

    // Steps 5-8 run as ONE atomic section under the service lock (the
    // idempotency decision, the rate-limit check, the quota reservation and
    // the record/queue insertion must not interleave with a concurrent
    // submission of the same id — a check-then-act gap here let two
    // concurrent same-id submissions BOTH pass the idempotency check and
    // the second overwrite the first's record). The section only touches
    // in-memory policy structures (rate limiter, quota ledger, queue) whose
    // internal locks are taken in the documented order state_ -> (rate |
    // quota | queue); the orchestrator is NEVER reached from here.
    const JobId job_id = request.distributed.envelope.job_id;
    ServiceJobView stored_view;
    {
        std::lock_guard<std::mutex> lock(state_);

        // Idempotency: a replay of the same submission never creates a
        // second job and never re-charges quota; a replay is also NOT
        // rate-limited (it creates no work). The same key under a different
        // identity/scope is a Conflict.
        const auto existing = records_.find(job_id);
        if (existing != records_.end()) {
            const ServiceJobRecord& record = existing->second;
            if (record.view.submitted_by == auth.user_id &&
                record.view.project_id == request.project_id &&
                envelope_equal(record.view.envelope, request.distributed.envelope) &&
                record.view.requested_shard_count == request.distributed.requested_shard_count) {
                out = record.view;
                created = false;
                metrics_.inc_jobs_replayed();
                audit_->record(auth.user_id, request.project_id, job_id, AuditAction::JobSubmit,
                               AuditOutcome::Ok, "replay");
                return ServiceStatus::Ok;
            }
            audit_->record(auth.user_id, request.project_id, job_id, AuditAction::JobSubmit,
                           AuditOutcome::Denied, service_status_code(ServiceStatus::Conflict));
            error = "job id already used by this service with a different submission";
            return ServiceStatus::Conflict;
        }

        // Rate limiting (per user; refused attempts count — ratelimit.hpp).
        if (config_.rate_limit_enabled && !rate_limiter_->try_acquire("submit:" + auth.user_id)) {
            metrics_.inc_rate_limit_rejections();
            audit_->record(auth.user_id, request.project_id, job_id, AuditAction::JobSubmit,
                           AuditOutcome::Denied,
                           service_status_code(ServiceStatus::RateLimitExceeded));
            error = "submission rate limit exceeded for this user";
            return ServiceStatus::RateLimitExceeded;
        }

        // Quota reservation (the project policy ledger; cluster reservation
        // stays in the Phase 12 registry — two responsibilities, two
        // systems). The memory figure is the honest whole-job byte estimate
        // the same Phase 12 shard accounting uses.
        std::int64_t memory_bytes = 0;
        if (!vortyx::distributed::shard_memory_bytes(request.distributed.envelope.element_count,
                                                     request.distributed.envelope.operation,
                                                     memory_bytes, error)) {
            return ServiceStatus::InvalidInput;
        }
        const QuotaEngine::Decision decision =
            quota_.reserve(request.project_id, job_id, request.distributed.requested_shard_count,
                           memory_bytes);
        if (decision.status != ServiceStatus::Ok) {
            if (decision.status == ServiceStatus::QuotaExceeded) metrics_.inc_quota_rejections();
            audit_->record(auth.user_id, request.project_id, job_id, AuditAction::JobSubmit,
                           AuditOutcome::Denied, service_status_code(decision.status));
            error = decision.error;
            return decision.status;
        }

        // Service record + queue insertion.
        ServiceJobRecord record;
        record.view.job_id = job_id;
        record.view.project_id = request.project_id;
        record.view.submitted_by = auth.user_id;
        record.view.envelope = request.distributed.envelope;
        record.view.requested_shard_count = request.distributed.requested_shard_count;
        record.view.status = DistributedJobStatus::Queued;
        record.view.submitted_at_ms = deps_.clock ? deps_.clock->now_ms() : 0;
        record.auth = auth;
        record.request = request;
        stored_view = record.view;
        records_[job_id] = std::move(record);
        order_.push_back(job_id);

        bool enqueued = false;
        QueuedJob queued;
        queued.job_id = job_id;
        queued.enqueued_at_ms = stored_view.submitted_at_ms;
        const ServiceStatus enqueue_status = queue_->enqueue(queued, enqueued);
        if (enqueue_status != ServiceStatus::Ok) {
            // Roll the record back (a resource-exhaustion refusal — never a
            // silent drop, never a stranded reservation).
            records_.erase(job_id);
            order_.pop_back();
            quota_.release(job_id);
            audit_->record(auth.user_id, request.project_id, job_id, AuditAction::JobSubmit,
                           AuditOutcome::Error, service_status_code(enqueue_status));
            error = "service queue is at capacity";
            return enqueue_status;
        }
        // Wake a dispatcher while STILL holding the service lock: a notify
        // issued after releasing it could slip between a dispatcher's
        // predicate check and its wait (a lost wakeup that strands the
        // queue — observed under ASan's stretched timings; notifying under
        // the lock is race-free).
        terminal_cv_.notify_all();
    }

    metrics_.inc_jobs_submitted();
    metrics_.set_jobs_queued(static_cast<std::int64_t>(queue_->depth()));
    audit_->record(auth.user_id, request.project_id, job_id, AuditAction::JobSubmit,
                   AuditOutcome::Ok, "");
    created = true;

    out = stored_view;
    return ServiceStatus::Ok;
}

// ---------------------------------------------------------------------------
// Jobs — dispatch / finalize
// ---------------------------------------------------------------------------

void PlatformService::dispatcher_loop() {
    for (;;) {
        // Wait for work or shutdown (the predicate reads the queue's own
        // state — the lock order state_ -> queue_ is the documented one).
        {
            std::unique_lock<std::mutex> lock(state_);
            terminal_cv_.wait(lock, [this] {
                return stopping_ || queue_->depth() > 0;
            });
        }

        QueuedJob queued;
        if (!queue_->try_dequeue(queued)) {
            if (stopping_) return;
            continue;  // spurious wakeup / queue drained by another dispatcher
        }
        metrics_.set_jobs_queued(static_cast<std::int64_t>(queue_->depth()));

        // The dispatch decision is atomic with cancellation (the module
        // header's contract): a cancel that beats this lock wins without
        // the job ever reaching the orchestrator.
        bool run = false;
        bool cancel_won = false;
        AuthContext submitter;
        SubmitJobRequest request;
        {
            std::lock_guard<std::mutex> lock(state_);
            const auto it = records_.find(queued.job_id);
            if (it == records_.end()) continue;  // defensive: cannot happen
            ServiceJobRecord& record = it->second;
            if (stopping_ || record.cancel_requested) {
                cancel_won = true;  // finalized after the lock is dropped
            } else if (record.request.has_value()) {
                record.dispatch_started = true;
                record.view.status = DistributedJobStatus::Running;
                submitter = record.auth;
                request = *record.request;
                run = true;
            }
        }

        if (cancel_won) {
            finalize_terminal(queued.job_id, DistributedJobStatus::Cancelled, "cancelled",
                              nullptr);
            continue;
        }
        if (!run) continue;

        // The dispatch: the EXISTING Phase 12 orchestrator under the
        // SUBMITTER's identity. NO service lock is held here.
        metrics_.inc_jobs_running();
        DistributedJobRecord dist_record;
        bool dist_created = false;
        const Status orchestrator_status =
            orchestrator_->submit(submitter, request.distributed, dist_record, dist_created);
        (void)orchestrator_status;  // the record's own status is the truth
        metrics_.dec_jobs_running();

        // The payload's job is done: erase it under the lock.
        {
            std::lock_guard<std::mutex> lock(state_);
            const auto it = records_.find(queued.job_id);
            if (it != records_.end()) it->second.request.reset();
        }
        finalize_terminal(queued.job_id, dist_record.status, dist_record.error, &dist_record);
    }
}

void PlatformService::finalize_terminal(const JobId& job_id, DistributedJobStatus status,
                                        const std::string& error,
                                        const DistributedJobRecord* dist) {
    if (!vortyx::distributed::distributed_job_status_is_terminal(status)) {
        return;  // never finalize to a non-terminal state
    }
    std::lock_guard<std::mutex> lock(state_);
    const auto it = records_.find(job_id);
    if (it == records_.end()) return;
    ServiceJobRecord& record = it->second;
    if (vortyx::distributed::distributed_job_status_is_terminal(record.view.status)) {
        return;  // exactly-once: the first terminal observation wins
    }

    record.view.status = status;
    record.view.error = error;
    record.view.terminal_at_ms = deps_.clock ? deps_.clock->now_ms() : 0;
    if (dist != nullptr) {
        record.view.total_shards = static_cast<std::int64_t>(dist->shards.size());
        record.view.succeeded_shards = 0;
        record.view.failed_shards = 0;
        for (const vortyx::distributed::JobShard& shard : dist->shards) {
            if (shard.state == vortyx::distributed::ShardState::Completed) {
                record.view.succeeded_shards += 1;
            } else if (shard.state == vortyx::distributed::ShardState::Failed) {
                record.view.failed_shards += 1;
            }
        }
    }

    // Exactly-once quota release (the ledger refuses the second release).
    quota_.release(job_id);

    switch (status) {
        case DistributedJobStatus::Completed:
            metrics_.inc_jobs_completed();
            break;
        case DistributedJobStatus::Failed:
            metrics_.inc_jobs_failed();
            break;
        case DistributedJobStatus::Cancelled:
            metrics_.inc_jobs_cancelled();
            break;
        default:
            break;
    }
    audit_->record(record.view.submitted_by, record.view.project_id, job_id,
                   AuditAction::JobTerminal,
                   status == DistributedJobStatus::Completed ? AuditOutcome::Ok
                                                             : AuditOutcome::Error,
                   status == DistributedJobStatus::Completed ? "" : to_string(status));
    terminal_cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Jobs — queries, cancel, wait
// ---------------------------------------------------------------------------

ServiceStatus PlatformService::resolve_role_for_job(const AuthContext& auth,
                                                    const ServiceJobRecord& record, bool for_write,
                                                    ProjectRole& out_role) const {
    // The submitter always sees (and may cancel) their own job; project
    // members see the project's jobs per the authz table.
    if (record.view.submitted_by == auth.user_id) {
        return projects_->role_of(auth, record.view.project_id, out_role);
    }
    const ServiceStatus role_status = projects_->role_of(auth, record.view.project_id, out_role);
    if (role_status != ServiceStatus::Ok) return role_status;
    const ProjectAction action =
        for_write ? ProjectAction::CancelAnyJob : ProjectAction::ViewJobs;
    if (authorize_project_action(out_role, action) != ServiceStatus::Ok) {
        return ServiceStatus::Forbidden;
    }
    return ServiceStatus::Ok;
}

ServiceStatus PlatformService::cancel_job(const AuthContext& auth, const JobId& job_id,
                                          ServiceJobView& out) {
    // Resolve the record under the lock; authorization queries run WITHOUT
    // the service lock (the project store has its own).
    std::string project_id;
    bool is_own = false;
    bool terminal = false;
    {
        std::lock_guard<std::mutex> lock(state_);
        const auto it = records_.find(job_id);
        if (it == records_.end()) {
            return ServiceStatus::NotFound;  // unknown AND foreign: invisible
        }
        const ServiceJobRecord& record = it->second;
        project_id = record.view.project_id;
        is_own = record.view.submitted_by == auth.user_id;
        terminal = vortyx::distributed::distributed_job_status_is_terminal(record.view.status);
        if (terminal) out = record.view;  // the honest race outcome below
    }

    ProjectRole role = ProjectRole::Viewer;
    const ServiceStatus role_status = projects_->role_of(auth, project_id, role);
    if (role_status != ServiceStatus::Ok) return role_status;
    if (authorize_project_action(role, is_own ? ProjectAction::CancelOwnJob
                                              : ProjectAction::CancelAnyJob) !=
        ServiceStatus::Ok) {
        audit_->record(auth.user_id, project_id, job_id, AuditAction::JobCancel,
                       AuditOutcome::Denied, service_status_code(ServiceStatus::Forbidden));
        return ServiceStatus::Forbidden;
    }

    if (terminal) {
        // The Phase 11 rule: a terminal job cannot change.
        return ServiceStatus::InvalidInput;
    }

    bool removed_from_queue = false;
    bool needs_orchestrator_cancel = false;
    {
        std::lock_guard<std::mutex> lock(state_);
        const auto it = records_.find(job_id);
        if (it == records_.end()) return ServiceStatus::NotFound;
        ServiceJobRecord& record = it->second;
        if (vortyx::distributed::distributed_job_status_is_terminal(record.view.status)) {
            out = record.view;
            return ServiceStatus::InvalidInput;  // completed under us — the race loser
        }
        record.cancel_requested = true;
        if (!record.dispatch_started) {
            removed_from_queue = queue_->remove(job_id);
            // A failed removal means the dispatcher just dequeued the job;
            // its atomic checkpoint will observe the flag UNLESS it already
            // marked the dispatch — re-read under THIS lock to decide.
            needs_orchestrator_cancel = !removed_from_queue && record.dispatch_started;
        } else {
            needs_orchestrator_cancel = true;
        }
    }

    if (removed_from_queue) {
        finalize_terminal(job_id, DistributedJobStatus::Cancelled, "cancelled", nullptr);
        metrics_.set_jobs_queued(static_cast<std::int64_t>(queue_->depth()));
        audit_->record(auth.user_id, project_id, job_id, AuditAction::JobCancel, AuditOutcome::Ok,
                       "cancelled_in_queue");
        std::lock_guard<std::mutex> lock(state_);
        out = records_.at(job_id).view;
        return ServiceStatus::Ok;
    }

    if (needs_orchestrator_cancel) {
        // The cancellation is delivered ATOMICALLY (Phase 15): the
        // orchestrator either sets the flag on the live record or records a
        // cancellation INTENT that submit() applies at record creation —
        // the record-creation window needs no polling and no retry loop.
        // The Phase 14 100x1ms sleep-poll handoff is gone.
        //
        // The two authorized paths:
        //   * the submitter's own job  -> the ordinary owner path;
        //   * another member's job     -> the EXPLICIT privileged path with
        //     a trusted ServiceCancellation context (never identity
        //     swapping). The privileged action is audited with the acting
        //     admin's real identity and authority.
        DistributedJobRecord dist_record;
        Status orchestrator_status;
        if (is_own) {
            orchestrator_status = orchestrator_->cancel_job(
                auth, job_id, dist_record, vortyx::distributed::CancelDelivery::RecordIntent);
        } else {
            const vortyx::distributed::ServiceCancellation cancellation(
                auth.user_id, "project_admin");
            orchestrator_status = orchestrator_->cancel_job_privileged(
                cancellation, job_id, dist_record,
                vortyx::distributed::CancelDelivery::RecordIntent);
        }

        if (orchestrator_status == Status::InvalidInput) {
            // The job went terminal under us — the honest race outcome.
            std::lock_guard<std::mutex> lock(state_);
            const auto it = records_.find(job_id);
            if (it != records_.end() &&
                vortyx::distributed::distributed_job_status_is_terminal(it->second.view.status)) {
                out = it->second.view;
                audit_->record(auth.user_id, project_id, job_id, AuditAction::JobCancel,
                               AuditOutcome::Error,
                               is_own ? "already_terminal" : "privileged:already_terminal");
                return ServiceStatus::InvalidInput;
            }
            // Not terminal after all (cannot happen in the documented
            // flow): fall through to the requested outcome, the dispatcher
            // still finalizes.
        }
        // Delivered (flag or intent): the dispatcher finalizes. The audit
        // records the privileged authority when the actor is not the
        // submitter — the auditable-privileged-action rule.
        audit_->record(auth.user_id, project_id, job_id, AuditAction::JobCancel,
                       AuditOutcome::Ok,
                       is_own ? "requested" : "privileged:cancel_any_job");
    } else {
        // For the not-yet-dispatched case the dispatcher's checkpoint
        // observes the flag and finalizes; nothing else to do here.
        audit_->record(auth.user_id, project_id, job_id, AuditAction::JobCancel,
                       AuditOutcome::Ok, "requested");
    }
    std::lock_guard<std::mutex> lock(state_);
    out = records_.at(job_id).view;
    return ServiceStatus::Ok;
}

ServiceStatus PlatformService::job(const AuthContext& auth, const JobId& job_id,
                                   ServiceJobView& out) const {
    std::lock_guard<std::mutex> lock(state_);
    const auto it = records_.find(job_id);
    if (it == records_.end()) return ServiceStatus::NotFound;
    ProjectRole role = ProjectRole::Viewer;
    const ServiceStatus access = resolve_role_for_job(auth, it->second, false, role);
    if (access != ServiceStatus::Ok) return access;
    out = it->second.view;
    return ServiceStatus::Ok;
}

ServiceStatus PlatformService::jobs(const AuthContext& auth,
                                    const std::optional<ProjectId>& project_id,
                                    std::vector<ServiceJobView>& out) const {
    std::string error;
    if (vortyx::platform::validate_auth(auth, error) != Status::Ok) {
        return ServiceStatus::Unauthenticated;
    }
    // A project filter requires visibility of that project (a foreign
    // project id is NotFound — never an empty list).
    if (project_id.has_value()) {
        ProjectRole role = ProjectRole::Viewer;
        const ServiceStatus status = projects_->role_of(auth, *project_id, role);
        if (status != ServiceStatus::Ok) return status;
    }
    std::lock_guard<std::mutex> lock(state_);
    out.clear();
    for (const JobId& id : order_) {
        const ServiceJobRecord& record = records_.at(id);
        if (project_id.has_value() && record.view.project_id != *project_id) continue;
        ProjectRole role = ProjectRole::Viewer;
        if (resolve_role_for_job(auth, record, false, role) != ServiceStatus::Ok) {
            continue;  // invisible to this caller
        }
        out.push_back(record.view);
    }
    return ServiceStatus::Ok;
}

ServiceStatus PlatformService::wait_for_terminal(const AuthContext& auth, const JobId& job_id,
                                                 std::int64_t timeout_ms, ServiceJobView& out) {
    std::unique_lock<std::mutex> lock(state_);
    const auto it = records_.find(job_id);
    if (it == records_.end()) return ServiceStatus::NotFound;
    ProjectRole role = ProjectRole::Viewer;
    const ServiceStatus access = resolve_role_for_job(auth, it->second, false, role);
    if (access != ServiceStatus::Ok) return access;

    const bool terminal = terminal_cv_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms), [&] {
            const auto refreshed = records_.find(job_id);
            return refreshed != records_.end() &&
                   vortyx::distributed::distributed_job_status_is_terminal(
                       refreshed->second.view.status);
        });
    out = records_.at(job_id).view;
    if (!terminal) {
        return ServiceStatus::Unavailable;  // "wait timed out" — never a fake terminal
    }
    return ServiceStatus::Ok;
}

ServiceStatus PlatformService::distributed_record(const AuthContext& auth, const JobId& job_id,
                                                  DistributedJobRecord& out) {
    {
        std::lock_guard<std::mutex> lock(state_);
        const auto it = records_.find(job_id);
        if (it == records_.end()) return ServiceStatus::NotFound;
        if (!it->second.dispatch_started) {
            return ServiceStatus::Unavailable;  // never reached the scheduler
        }
    }
    return service_status_from_platform(orchestrator_->job(auth, job_id, out));
}

// ---------------------------------------------------------------------------
// Devices (audited passthrough)
// ---------------------------------------------------------------------------

ServiceStatus PlatformService::register_device(
    const AuthContext& auth, const vortyx::platform::DeviceId& device_id,
    const vortyx::distributed::DeviceCapabilities& capabilities, bool& created) {
    vortyx::distributed::DeviceDescriptor descriptor;
    const Status status =
        orchestrator_->register_device(auth.user_id, device_id, capabilities, descriptor, created);
    audit_->record(auth.user_id, "", device_id, AuditAction::DeviceRegister, outcome_for(
        status == Status::Ok ? ServiceStatus::Ok
                             : (status == Status::Forbidden ? ServiceStatus::Forbidden
                                                            : ServiceStatus::InvalidInput)),
        status == Status::Ok ? "" : vortyx::platform::to_string(status));
    return service_status_from_platform(status);
}

ServiceStatus PlatformService::set_device_state(const AuthContext& auth,
                                                const vortyx::platform::DeviceId& device_id,
                                                vortyx::distributed::DeviceState to) {
    const Status status = orchestrator_->set_device_state(auth.user_id, device_id, to);
    audit_->record(auth.user_id, "", device_id, AuditAction::DeviceStateChange,
                   status == Status::Ok ? AuditOutcome::Ok
                                        : (status == Status::Forbidden ? AuditOutcome::Denied
                                                                       : AuditOutcome::Error),
                   status == Status::Ok ? vortyx::distributed::to_string(to)
                                        : vortyx::platform::to_string(status));
    return service_status_from_platform(status);
}

ServiceStatus PlatformService::heartbeat_device(const AuthContext& auth,
                                                const vortyx::platform::DeviceId& device_id) {
    return service_status_from_platform(orchestrator_->heartbeat_device(auth.user_id, device_id));
}

// ---------------------------------------------------------------------------
// Artifacts / audit / health
// ---------------------------------------------------------------------------

ServiceStatus PlatformService::register_artifact(const AuthContext& auth,
                                                 const std::string& project_id,
                                                 const std::string& name,
                                                 std::int64_t declared_byte_size,
                                                 ArtifactMetadata& out) {
    ProjectRole role = ProjectRole::Viewer;
    const ServiceStatus status = projects_->role_of(auth, project_id, role);
    if (status != ServiceStatus::Ok) return status;
    if (authorize_project_action(role, ProjectAction::RegisterArtifact) != ServiceStatus::Ok) {
        return ServiceStatus::Forbidden;
    }
    ArtifactMetadata request;
    request.project_id = project_id;
    request.name = name;
    request.created_by = auth.user_id;
    request.declared_byte_size = declared_byte_size;
    const ServiceStatus result = artifacts_store_->register_artifact(request, out);
    if (result == ServiceStatus::Ok) {
        audit_->record(auth.user_id, project_id, "", AuditAction::ArtifactRegister,
                       AuditOutcome::Ok, out.artifact_id);
    }
    return result;
}

ServiceStatus PlatformService::artifacts(const AuthContext& auth, const std::string& project_id,
                                         std::vector<ArtifactMetadata>& out) const {
    ProjectRole role = ProjectRole::Viewer;
    const ServiceStatus status = projects_->role_of(auth, project_id, role);
    if (status != ServiceStatus::Ok) return status;
    if (authorize_project_action(role, ProjectAction::ViewJobs) != ServiceStatus::Ok) {
        return ServiceStatus::Forbidden;
    }
    return artifacts_store_->artifacts(project_id, out);
}

ServiceStatus PlatformService::delete_artifact(const AuthContext& auth,
                                               const std::string& project_id,
                                               const ArtifactId& artifact_id) {
    ProjectRole role = ProjectRole::Viewer;
    const ServiceStatus status = projects_->role_of(auth, project_id, role);
    if (status != ServiceStatus::Ok) return status;

    // Creator OR Admin+ (DeleteArtifact). Fetch first: the identity check
    // needs the record, and a foreign/unknown artifact must stay invisible
    // (NotFound — the anti-enumeration rule).
    ArtifactMetadata existing;
    const ServiceStatus fetch = artifacts_store_->artifact(artifact_id, existing);
    if (fetch != ServiceStatus::Ok) {
        return ServiceStatus::NotFound;
    }
    if (existing.project_id != project_id) {
        return ServiceStatus::NotFound;  // cross-project reference: invisible
    }
    const bool is_creator = existing.created_by == auth.user_id;
    if (!is_creator &&
        authorize_project_action(role, ProjectAction::DeleteArtifact) != ServiceStatus::Ok) {
        audit_->record(auth.user_id, project_id, "", AuditAction::ArtifactDelete,
                       AuditOutcome::Denied, service_status_code(ServiceStatus::Forbidden));
        return ServiceStatus::Forbidden;
    }
    const ServiceStatus result = artifacts_store_->unregister_artifact(project_id, artifact_id);
    if (result == ServiceStatus::Ok) {
        audit_->record(auth.user_id, project_id, "", AuditAction::ArtifactDelete,
                       AuditOutcome::Ok, is_creator ? "creator" : "admin");
    }
    return result;
}

std::vector<AuditEvent> PlatformService::audit_tail_for_actor(const AuthContext& auth,
                                                              std::size_t count) const {
    std::string error;
    if (vortyx::platform::validate_auth(auth, error) != Status::Ok) return {};
    std::vector<AuditEvent> tail = audit_->store()->tail(count);
    std::vector<AuditEvent> own;
    for (AuditEvent& event : tail) {
        if (event.actor_user_id == auth.user_id) own.push_back(std::move(event));
    }
    return own;
}

HealthReport PlatformService::health_check(const AuthContext& auth) {
    HealthReport report;
    report.checked_at_ms = deps_.clock ? deps_.clock->now_ms() : 0;

    ComponentHealth service_health;
    service_health.component = "service";
    service_health.status = HealthValue::Healthy;  // the check itself ran here
    service_health.detail = "control plane is responding";
    report.components.push_back(service_health);

    ComponentHealth queue_health;
    queue_health.component = "queue";
    const std::size_t depth = queue_->depth();
    queue_health.status =
        depth < config_.max_queue_depth ? HealthValue::Healthy : HealthValue::Unhealthy;
    queue_health.detail =
        "depth " + std::to_string(depth) + " / capacity " + std::to_string(config_.max_queue_depth);
    report.components.push_back(queue_health);

    ComponentHealth scheduler_health;
    scheduler_health.component = "scheduler";
    scheduler_health.status = HealthValue::Healthy;
    scheduler_health.detail = "the Phase 12 orchestrator is attached";
    report.components.push_back(scheduler_health);

    ComponentHealth store_health;
    store_health.component = "platform_store";
    if (deps_.platform_store != nullptr) {
        store_health.status = HealthValue::Healthy;
        store_health.detail = "a platform store is attached";
    } else {
        // A provider that was never wired up is NOT healthy (the honesty
        // rule): it is explicitly not configured.
        store_health.status = HealthValue::NotConfigured;
        store_health.detail = "no platform store attached (job mirroring disabled)";
    }
    report.components.push_back(store_health);

    // Device health: the CALLER's own devices only (ownership-scoped, the
    // Phase 12 rule — never a global fleet claim).
    orchestrator_->check_heartbeats(auth.user_id);
    vortyx::distributed::ClusterSnapshot snapshot = orchestrator_->cluster_snapshot(auth.user_id);
    for (const vortyx::distributed::DeviceSnapshot& device : snapshot.devices) {
        report.devices.total += 1;
        switch (device.health) {
            case vortyx::distributed::DeviceHealth::Healthy:
                report.devices.healthy += 1;
                break;
            case vortyx::distributed::DeviceHealth::Unhealthy:
                report.devices.unhealthy += 1;
                break;
            case vortyx::distributed::DeviceHealth::Unknown:
                report.devices.offline += 1;
                break;
        }
    }
    ComponentHealth devices_health;
    devices_health.component = "devices";
    devices_health.status =
        report.devices.total == 0
            ? HealthValue::Unknown
            : (report.devices.unhealthy + report.devices.offline == 0 ? HealthValue::Healthy
                                                                      : HealthValue::Unhealthy);
    devices_health.detail = "caller-scoped: " + std::to_string(report.devices.healthy) +
                            " healthy, " + std::to_string(report.devices.unhealthy) +
                            " unhealthy, " + std::to_string(report.devices.offline) +
                            " offline/unknown";
    report.components.push_back(devices_health);

    return report;
}

}  // namespace vortyx::service
