#pragma once

// PlatformService (Phase 14) — the serviceization facade.
//
// The full control-plane flow, WRAPPING (never replacing) the existing
// layers:
//
//   submit_job(auth, request):
//     Authentication   (platform::AuthContext — the transport's output)
//       -> Request validation     (envelope/task/shard limits)
//       -> Project validation     (IProjectStore — visibility = membership)
//       -> Authorization          (authz table: SubmitJob = Member+)
//       -> Rate limiting          (deterministic fixed window per user)
//       -> Quota reservation      (project policy ledger, idempotent)
//       -> Queue insertion        (IJobQueue, FIFO, idempotent)
//       -> [dispatcher] Scheduler handoff (Phase 12 orchestrator — THE
//          existing distributed path: registry/lease/policy/worker/
//          aggregator/retry unchanged)
//       -> Terminal finalize      (quota release, metrics, audit)
//
// Every refusal in the flow is a distinct ServiceStatus with a stable code;
// every accepted submission and every terminal outcome is audited; every
// counter in the metrics is a real event.
//
// JOB LIFECYCLE (service view — REUSING the Phase 12 vocabulary, never a
// new state machine):
//   Queued     — accepted, waiting in the queue
//   Running    — dispatched into the Phase 12 orchestrator (whose own
//                Queued/Planning/Scheduled/Running fine states live in the
//                orchestrator's record; the service view collapses them —
//                the same collapse map_to_platform_job_status performs)
//   Completed / Failed / Cancelled — terminal, immutable.
// The transition set is exactly the Phase 12 distributed job transition
// table restricted to the service-visible states; terminal states accept no
// mutation (cancel on terminal is InvalidInput, the Phase 11 rule).
//
// CANCELLATION RACES (all defined, all tested):
//   - queued job          -> removed from the queue, Cancelled, quota
//                            released exactly once;
//   - dispatched job      -> the orchestrator's cancel flag is set (bounded
//                            handoff retry covers the record-creation
//                            window); the dispatcher finalizes;
//   - cancel vs completion -> one of the two wins by the underlying state
//                            machines; the loser reports InvalidInput
//                            ("already terminal"). No double quota release
//                            is possible (the ledger releases exactly once).
//
// THREADING / LOCK ORDER (documented contract):
//   state_ (service records) -> (quota | queue | audit | metrics internal
//   locks), one at a time, never inverted. The orchestrator, the registry
//   and the transport are NEVER called while holding state_ (the Phase 12
//   rule: no service lock spans a dispatch).
//
// IDEMPOTENCY: the client-supplied job_id is the idempotency key (the Phase
// 11 rule, inherited): same owner + same project + same envelope + same
// shard count -> replay of the existing record (no side effects, no quota
// double-charge); same key with different owner/project/payload ->
// Conflict. The service's check-and-insert is atomic, so concurrent
// duplicates produce exactly one created job and one replay.

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "distributed/distributed.hpp"  // orchestrator + registry/transport/clock
#include "platform/platform.hpp"
#include "service/artifact.hpp"
#include "service/audit.hpp"
#include "service/authz.hpp"
#include "service/health.hpp"
#include "service/metrics.hpp"
#include "service/project.hpp"
#include "service/quota.hpp"
#include "service/queue.hpp"
#include "service/ratelimit.hpp"
#include "service/service_status.hpp"

namespace vortyx::service {

// The service-view record of one job (metadata only — payloads never live
// here; the dispatch request is kept until dispatch starts, then erased).
struct ServiceJobView {
    vortyx::platform::JobId job_id;
    std::string project_id;
    vortyx::platform::UserId submitted_by;

    vortyx::platform::JobEnvelope envelope;   // reused Phase 11 contract
    std::uint32_t requested_shard_count = 1;

    vortyx::distributed::DistributedJobStatus status =
        vortyx::distributed::DistributedJobStatus::Queued;
    std::string error;  // required when Failed (never hidden)

    std::int64_t submitted_at_ms = 0;
    std::int64_t terminal_at_ms = 0;  // stamped once, at the terminal transition

    // Honest aggregate counts at terminal (from the orchestrator's record;
    // -1 = not applicable / not terminal).
    std::int64_t total_shards = -1;
    std::int64_t succeeded_shards = -1;
    std::int64_t failed_shards = -1;
};

// The submission request: project scope + the EXISTING Phase 12 request
// (envelope + local payload + shard count — reused verbatim).
struct SubmitJobRequest {
    std::string project_id;
    vortyx::distributed::DistributedJobRequest distributed;
};

// Service configuration. Defaults are the documented safe starting policy
// (rate limiting ON, finite quotas, finite capacities — nothing unlimited).
struct PlatformServiceConfig {
    std::uint32_t dispatcher_count = 2;                 // 1..8
    bool rate_limit_enabled = true;
    std::uint32_t rate_limit_max_submissions = 60;      // per user per window
    std::int64_t rate_limit_window_ms = 60000;
    ProjectQuota default_project_quota;                 // {4 jobs, 16 shards, 1 GiB}
    std::size_t audit_max_entries = 10000;              // bounded ring
    std::size_t max_queue_depth = 1024;                 // service-level capacity
    std::uint32_t max_requested_shard_count = 64;       // service-level shard cap
};

class PlatformService {
public:
    struct Deps {
        // The Phase 12 execution world (required, non-owning): the service
        // hands jobs to the orchestrator built over these — the same
        // registry/transport/clock instances the cluster uses.
        vortyx::distributed::IDeviceRegistry* registry = nullptr;
        vortyx::distributed::IWorkerTransport* transport = nullptr;
        std::shared_ptr<vortyx::distributed::IClock> clock;  // required

        // Optional Phase 11 mirror (the orchestrator's platform_store).
        vortyx::platform::IPlatformStore* platform_store = nullptr;

        // Optional provider-neutral stores: when null, the service creates
        // the in-memory reference implementations (documented local/mock
        // providers).
        IProjectStore* project_store = nullptr;
        IJobQueue* job_queue = nullptr;
        IAuditStore* audit_store = nullptr;
        IArtifactStore* artifact_store = nullptr;

        // The distributed configuration (policy selection, retry policy).
        vortyx::distributed::DistributedConfig distributed_config;
    };

    // Builds the service and starts its dispatcher threads. Errors:
    // InvalidInput (null registry/transport/clock, bad config) |
    // InvalidInput propagated from orchestrator construction (unknown
    // policy).
    static ServiceStatus create(Deps deps, const PlatformServiceConfig& config,
                                std::unique_ptr<PlatformService>& out, std::string& error);

    ~PlatformService();  // stops the dispatchers and joins the threads

    PlatformService(const PlatformService&) = delete;
    PlatformService& operator=(const PlatformService&) = delete;

    // ---- projects ----------------------------------------------------------

    ServiceStatus create_project(const vortyx::platform::AuthContext& auth,
                                 const std::string& name, ProjectRecord& out);
    ServiceStatus project(const vortyx::platform::AuthContext& auth, const ProjectId& project_id,
                          ProjectRecord& out);
    ServiceStatus projects(const vortyx::platform::AuthContext& auth,
                           std::vector<ProjectRecord>& out);
    ServiceStatus archive_project(const vortyx::platform::AuthContext& auth,
                                  const ProjectId& project_id, ProjectRecord& out);
    ServiceStatus add_member(const vortyx::platform::AuthContext& auth,
                             const ProjectId& project_id,
                             const vortyx::platform::UserId& user_id, ProjectRole role,
                             ProjectMember& out);
    ServiceStatus remove_member(const vortyx::platform::AuthContext& auth,
                                const ProjectId& project_id,
                                const vortyx::platform::UserId& user_id);
    ServiceStatus members(const vortyx::platform::AuthContext& auth, const ProjectId& project_id,
                          std::vector<ProjectMember>& out);

    // Project quota policy (Admin+ per the authz table; audited).
    ServiceStatus set_project_quota(const vortyx::platform::AuthContext& auth,
                                    const ProjectId& project_id, const ProjectQuota& quota);
    // The project's live usage (Member+; real ledger numbers).
    ServiceStatus project_usage(const vortyx::platform::AuthContext& auth,
                                const ProjectId& project_id, QuotaUsage& out);

    // ---- jobs ---------------------------------------------------------------

    // The full submission flow (see the module header). 'out' receives the
    // record (created or replayed).
    ServiceStatus submit_job(const vortyx::platform::AuthContext& auth,
                             const SubmitJobRequest& request, ServiceJobView& out,
                             bool& created);

    ServiceStatus cancel_job(const vortyx::platform::AuthContext& auth,
                             const vortyx::platform::JobId& job_id, ServiceJobView& out);
    ServiceStatus job(const vortyx::platform::AuthContext& auth,
                      const vortyx::platform::JobId& job_id, ServiceJobView& out) const;
    ServiceStatus jobs(const vortyx::platform::AuthContext& auth,
                       const std::optional<ProjectId>& project_id,
                       std::vector<ServiceJobView>& out) const;

    // Blocks until the job is terminal or the timeout elapses. Unavailable
    // with "wait timed out" on timeout (an honest outcome, never a fake
    // terminal). Returns the terminal (or current) view either way.
    ServiceStatus wait_for_terminal(const vortyx::platform::AuthContext& auth,
                                    const vortyx::platform::JobId& job_id,
                                    std::int64_t timeout_ms, ServiceJobView& out);

    // The Phase 12 record of a dispatched job (fine-grained shard states).
    // Unavailable when the job never reached the orchestrator (queued or
    // cancelled-in-queue — the honest distinction).
    ServiceStatus distributed_record(const vortyx::platform::AuthContext& auth,
                                     const vortyx::platform::JobId& job_id,
                                     vortyx::distributed::DistributedJobRecord& out);

    // ---- devices (thin, audited passthrough to the orchestrator) -----------

    ServiceStatus register_device(const vortyx::platform::AuthContext& auth,
                                  const vortyx::platform::DeviceId& device_id,
                                  const vortyx::distributed::DeviceCapabilities& capabilities,
                                  bool& created);
    ServiceStatus set_device_state(const vortyx::platform::AuthContext& auth,
                                   const vortyx::platform::DeviceId& device_id,
                                   vortyx::distributed::DeviceState to);
    // The liveness report (the Phase 12 evidence-based health rule: a
    // heartbeat makes the device Healthy; its absence does not).
    ServiceStatus heartbeat_device(const vortyx::platform::AuthContext& auth,
                                   const vortyx::platform::DeviceId& device_id);

    // ---- artifacts (metadata only — see artifact.hpp) ----------------------

    ServiceStatus register_artifact(const vortyx::platform::AuthContext& auth,
                                    const std::string& project_id, const std::string& name,
                                    std::int64_t declared_byte_size, ArtifactMetadata& out);
    ServiceStatus artifacts(const vortyx::platform::AuthContext& auth,
                            const std::string& project_id,
                            std::vector<ArtifactMetadata>& out) const;

    // ---- observability -------------------------------------------------------

    ServiceMetricsSnapshot metrics() const { return metrics_.snapshot(); }

    // The caller's own audit events (the safe default scope: a user sees
    // the events they caused; cross-user audit access is future work).
    std::vector<AuditEvent> audit_tail_for_actor(const vortyx::platform::AuthContext& auth,
                                                 std::size_t count) const;

    // The health report (auth-scoped device aggregates; see health.hpp).
    HealthReport health_check(const vortyx::platform::AuthContext& auth);

    // The project's role for 'auth' (the authorization query, exposed for
    // the API layer). NotFound for foreign/unknown projects.
    ServiceStatus project_role(const vortyx::platform::AuthContext& auth,
                               const ProjectId& project_id, ProjectRole& out) const;

private:
    PlatformService() = default;

    // The internal record (view + dispatch bookkeeping + the payload until
    // dispatch starts).
    struct ServiceJobRecord {
        ServiceJobView view;
        vortyx::platform::AuthContext auth;  // the submitter (no privileged path)
        bool dispatch_started = false;
        bool cancel_requested = false;
        std::optional<SubmitJobRequest> request;  // erased after dispatch
    };

    // Dispatcher body: drain the queue into the orchestrator.
    void dispatcher_loop();

    // Finalizes a job to a terminal state exactly once (quota release,
    // metrics, audit, waiter notification). 'dist' carries the orchestrator
    // record when the job was dispatched (shard counts); null for
    // cancelled-in-queue jobs.
    void finalize_terminal(const vortyx::platform::JobId& job_id,
                           vortyx::distributed::DistributedJobStatus status,
                           const std::string& error,
                           const vortyx::distributed::DistributedJobRecord* dist);

    ServiceStatus resolve_role_for_job(const vortyx::platform::AuthContext& auth,
                                       const ServiceJobRecord& record, bool for_write,
                                       ProjectRole& out_role) const;

    Deps deps_;
    PlatformServiceConfig config_;

    // Owned components (the providers the deps did not supply).
    std::unique_ptr<IProjectStore> owned_projects_;
    std::unique_ptr<IJobQueue> owned_queue_;
    std::unique_ptr<IAuditStore> owned_audit_;
    std::unique_ptr<IArtifactStore> owned_artifacts_;
    IProjectStore* projects_ = nullptr;
    IJobQueue* queue_ = nullptr;
    IAuditStore* audit_store_ = nullptr;
    IArtifactStore* artifacts_store_ = nullptr;
    std::unique_ptr<AuditTrail> audit_;
    std::unique_ptr<vortyx::distributed::DistributedOrchestrator> orchestrator_;

    QuotaEngine quota_;
    std::unique_ptr<RateLimiter> rate_limiter_;  // built at create()
    ServiceMetrics metrics_;

    mutable std::mutex state_;              // guards records_ + order_
    std::unordered_map<vortyx::platform::JobId, ServiceJobRecord> records_;
    std::vector<vortyx::platform::JobId> order_;  // submission order
    std::condition_variable terminal_cv_;         // wait_for_terminal

    std::vector<std::thread> dispatchers_;
    bool stopping_ = false;
};

}  // namespace vortyx::service
