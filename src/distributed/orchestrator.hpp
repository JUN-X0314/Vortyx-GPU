#pragma once

// DistributedOrchestrator (Phase 12) — the facade that makes the cluster
// actually execute distributed jobs, and the ONLY component that sees the
// whole flow:
//
//   submit(auth, request)
//     -> validate (Phase 11 envelope rules + local payload consistency)
//     -> [platform store: create_job (idempotent)]           (optional)
//     -> Planning    (shard creation from the placement plan)
//     -> Scheduled   (atomic leases on the registry)
//     -> Running     (shards dispatched through the transport to workers)
//     -> retry waves (Retrying -> re-place -> re-execute; succeeded shards
//                     are NEVER re-run — checkpoint semantics)
//     -> terminal    (Completed | Failed | Cancelled — partial failure is
//                     a Failed with honest succeeded/failed counts)
//     -> [platform store: update_job + put_result]           (optional)
//
// BOUNDARY RULES (the ones Phase 11 established and Phase 12 keeps):
//   - ComputeTask stays LOCAL: it is handed to workers through the
//     in-process transport; the control plane (platform store) receives
//     metadata only. No serialization of the task exists anywhere.
//   - OWNERSHIP end to end: every job carries the submitter's identity;
//     status/cancel queries go through is_owner, and foreign jobs are
//     NotFound (anti-enumeration). The registry and every snapshot view
//     are ownership-scoped, so a scheduler literally cannot see another
//     user's devices.
//   - NO Supabase SDK, no HTTP client, no JSON in the execution path: the
//     optional platform integration speaks only IPlatformStore.
//
// STALE-PLAN POLICY: a plan records the cluster revision it was based on.
// Before executing, the orchestrator re-checks the revision; on mismatch
// (or a failed reservation — capacity lost under us) it RE-PLANS, up to
// kMaxPlanAttempts, then fails the job with the stale/no-capacity reason.
// Stale plans are never force-executed.
//
// BACKOFF: retry delays are computed by the policy's pure retry_delay_ms
// and STAMPED on the shard (next_attempt_eligible_ms) for observability.
// Phase 12's synchronous in-process executor re-places immediately and
// does not block on the stamped delay (documented; real waiting belongs to
// a future async executor — the delay itself is unit-tested).
//
// SYNCHRONOUS SUBMIT: submit() returns when the job is terminal. Calling
// submit() from several threads is supported (jobs run concurrently;
// the registry's atomic reservation prevents overcommit); cancel() from
// another thread sets a cancellation flag that the executing submit
// observes at wave/dispatch boundaries.

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "distributed/aggregator.hpp"
#include "distributed/clock.hpp"
#include "distributed/config.hpp"
#include "distributed/heartbeat.hpp"
#include "distributed/job.hpp"
#include "distributed/policy.hpp"
#include "distributed/registry.hpp"
#include "distributed/retry.hpp"
#include "distributed/transport.hpp"
#include "platform/auth.hpp"
#include "platform/store.hpp"

namespace vortyx::distributed {

using vortyx::platform::JobId;  // reused platform identity (see device.hpp)
using vortyx::platform::UserId;

// How many times the orchestrator tolerates the cluster changing under a
// plan before it reports the instability honestly (stale-plan bound).
inline constexpr std::uint32_t kMaxPlanAttempts = 8;

// The submission contract of one distributed job. The control-plane part
// is the Phase 11 JobEnvelope (reused verbatim — job id, operation,
// element count, requested backend, priority, protocol version); the
// LOCAL payload is the ComputeTask that stays on this machine.
struct DistributedJobRequest {
    vortyx::platform::JobEnvelope envelope;  // validated (validate_job_envelope)
    vortyx::compute::ComputeTask task;       // validated (validate_compute_task)
    std::uint32_t requested_shard_count = 1; // >= 1; 1 = single-device execution
};

// One shard's observable record (mirrors JobShard plus scheduling facts).
// NOTE on job-level vs shard-level state machines: the record's `status`
// is the DISTRIBUTED job status (job.hpp); each shard carries its own
// ShardState. They are related by derive_job_status, never by sharing a
// vocabulary.
struct DistributedJobRecord {
    JobId job_id;
    UserId owner_user_id;

    vortyx::compute::ComputeOp operation = vortyx::compute::ComputeOp::VectorAdd;
    std::uint64_t element_count = 0;
    std::string requested_backend;
    std::uint32_t requested_shard_count = 1;

    DistributedJobStatus status = DistributedJobStatus::Queued;
    std::string error;  // required when Failed (never hidden)

    std::vector<JobShard> shards;
    DistributedResult result;  // filled at terminal state

    std::int64_t created_at_ms = 0;     // orchestrator clock
    std::int64_t completed_at_ms = 0;   // set on terminal

    // Non-fatal platform-store synchronization failure (the local outcome
    // stands; the sync problem is reported, never swallowed).
    std::string platform_error;

    // The last status MIRRORED to the platform store (the mirror skips
    // redundant transitions — the store's transition table refuses a
    // Running -> Running).
    vortyx::platform::JobStatus platform_status = vortyx::platform::JobStatus::Queued;

    // Set by cancel(); observed by the executing submit at wave boundaries.
    // Raw atomic: this flag is the one cross-thread message of the record.
    std::atomic<bool> cancel_requested{false};

    // Copy support (defined out-of-line in the .cpp): a copy is a snapshot
    // of the record, including the flag's VALUE. Non-copyable by default
    // because of the atomic member; the explicit declarations restore the
    // snapshot semantics every consumer (submit/job/cancel out-params)
    // needs.
    DistributedJobRecord() = default;
    DistributedJobRecord(const DistributedJobRecord& other);
    DistributedJobRecord& operator=(const DistributedJobRecord& other);
};

class DistributedOrchestrator {
public:
    struct Deps {
        // Required, non-owning. Must outlive the orchestrator.
        IDeviceRegistry* registry = nullptr;
        IWorkerTransport* transport = nullptr;
        std::shared_ptr<IClock> clock;  // required (determinism by injection)

        // Optional Phase 11 integration: when present, job submissions and
        // outcomes are mirrored into the control plane through the
        // provider-neutral store (create_job / update_job / put_result)
        // using the submitter's own AuthContext — no privileged path.
        vortyx::platform::IPlatformStore* platform_store = nullptr;
    };

    // Constructs the orchestrator. 'policy' comes from the config name via
    // make_scheduling_policy; an unknown config policy is refused at
    // construction (never silently defaulted): Status::InvalidInput.
    static vortyx::platform::Status create(Deps deps, const DistributedConfig& config,
                                           std::unique_ptr<DistributedOrchestrator>& out,
                                           std::string& error);

    ~DistributedOrchestrator() = default;

    DistributedOrchestrator(const DistributedOrchestrator&) = delete;
    DistributedOrchestrator& operator=(const DistributedOrchestrator&) = delete;

    // ---- devices (ownership-scoped conveniences over the registry) -------

    vortyx::platform::Status register_device(const UserId& owner_user_id,
                                             const DeviceId& device_id,
                                             const DeviceCapabilities& capabilities,
                                             DeviceDescriptor& out, bool& created);
    vortyx::platform::Status devices(const UserId& owner_user_id,
                                     std::vector<DeviceDescriptor>& out);
    vortyx::platform::Status device(const UserId& owner_user_id, const DeviceId& device_id,
                                    DeviceDescriptor& out);
    vortyx::platform::Status heartbeat_device(const UserId& owner_user_id,
                                              const DeviceId& device_id);
    vortyx::platform::Status set_device_state(const UserId& owner_user_id,
                                              const DeviceId& device_id, DeviceState to);

    // Runs one liveness check over this owner's devices (the heartbeat
    // monitor's judgment — see heartbeat.hpp).
    std::size_t check_heartbeats(const UserId& owner_user_id);

    // The owner-visible cluster view (immutable snapshot; ownership filtered).
    ClusterSnapshot cluster_snapshot(const UserId& owner_user_id);

    // ---- jobs --------------------------------------------------------------

    // Submits and SYNCHRONOUSLY executes a distributed job. Idempotent by
    // job_id (same owner + same request -> Ok with created == false and
    // the EXISTING record; different payload or owner -> Conflict — the
    // Phase 11 submission rule). Errors: Unauthenticated | InvalidInput |
    // Conflict | Internal.
    vortyx::platform::Status submit(const vortyx::platform::AuthContext& auth,
                                    const DistributedJobRequest& request,
                                    DistributedJobRecord& out, bool& created);

    // Fetches one own job (any state). Foreign/unknown -> NotFound.
    vortyx::platform::Status job(const vortyx::platform::AuthContext& auth, const JobId& job_id,
                                 DistributedJobRecord& out);

    // Lists own jobs in submission order.
    vortyx::platform::Status jobs(const vortyx::platform::AuthContext& auth,
                                  std::vector<DistributedJobRecord>& out);

    // Owner-initiated cancellation. Non-terminal jobs only (a terminal job
    // is InvalidInput — the Phase 11 rule). Pending/Assigned/Retrying
    // shards are cancelled; in-flight shards finish and are recorded; the
    // derived status applies. Errors: Unauthenticated | InvalidInput |
    // NotFound.
    vortyx::platform::Status cancel_job(const vortyx::platform::AuthContext& auth,
                                        const JobId& job_id, DistributedJobRecord& out);

    // The active configuration (observability; never re-parsed implicitly).
    const DistributedConfig& config() const { return config_; }

private:
    DistributedOrchestrator(Deps deps, DistributedConfig config, RetryPolicy retry,
                            std::unique_ptr<ISchedulingPolicy> policy);

    // The synchronous execution pipeline (caller holds no lock; the
    // orchestrator's mutex guards record state transitions only).
    void run_job(DistributedJobRecord& job, const vortyx::platform::AuthContext& auth);

    // Places the job's Pending shards from one placement plan and reserves
    // their leases atomically (stale-plan aware, bounded re-planning).
    // Returns false with 'reason' when placement is refused or the
    // attempts run out.
    bool place_new_shards(DistributedJobRecord& job, std::string& reason);

    // Re-places ONE retrying shard: a single-shard placement request of
    // the shard's own range size, re-based onto the shard's original
    // range. Returns false with 'reason' when no placement is possible.
    bool replace_shard(DistributedJobRecord& job, JobShard& shard, std::string& reason);

    // Executes one wave of assigned shards (sequential or threaded per
    // config), applies results (aggregator, lease release, retry stamp).
    void execute_wave(DistributedJobRecord& job, const vortyx::platform::AuthContext& auth);

    // One shard dispatch + result handling (sequential path; shared by the
    // threaded path's result application). build_and_submit copies the
    // execution input under the lock and submits WITHOUT it.
    ShardResult build_and_submit(const DistributedJobRecord& job, const JobShard& shard);
    void dispatch_and_apply(DistributedJobRecord& job, JobShard& shard,
                            const vortyx::platform::AuthContext& auth);
    void apply_result(DistributedJobRecord& job, JobShard& shard, const ShardResult& result);

    // Applies the derived status through the transition table; stamps
    // terminal time and fills the aggregate result.
    void apply_terminal(DistributedJobRecord& job);
    DistributedResult assemble_result(const DistributedJobRecord& job);

    // Returns the shard's lease capacity to the registry (best-effort; an
    // expired lease is reclaimed lazily by the registry anyway).
    void release_shard_lease(const DistributedJobRecord& job, JobShard& shard);

    // Platform-store mirroring (best effort, failures recorded in the
    // record's platform_error — never fatal, never silent).
    void mirror_status(const vortyx::platform::AuthContext& auth, DistributedJobRecord& job,
                       vortyx::platform::JobStatus to, const std::string& error_reason);
    void mirror_running_once(const vortyx::platform::AuthContext& auth,
                             DistributedJobRecord& job);
    void mirror_terminal(const vortyx::platform::AuthContext& auth, DistributedJobRecord& job);

    Deps deps_;
    DistributedConfig config_;
    RetryPolicy retry_;
    std::unique_ptr<ISchedulingPolicy> policy_;
    std::unique_ptr<HeartbeatMonitor> heartbeat_;

    std::vector<std::unique_ptr<DistributedJobRecord>> jobs_;  // submission order

    // Per-job side state (records above stay metadata-only; these hold the
    // LOCAL payload and execution bookkeeping, erased at terminal state).
    std::unordered_map<JobId, DistributedJobRequest> requests_;  // idempotency + payload
    std::unordered_map<JobId, ResultAggregator> aggregators_;    // per-job aggregation
    std::unordered_map<std::string, DeviceLease> shard_leases_;  // shard_id -> lease

    // Guards the jobs table and record state transitions. NEVER held
    // during transport submits or registry calls (lock ordering: the
    // registry has its own lock and is always taken without this one).
    mutable std::mutex mutex_;
};

}  // namespace vortyx::distributed
