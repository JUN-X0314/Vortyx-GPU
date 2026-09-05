// Service job lifecycle end-to-end tests (Phase 14).
//
// The full control-plane flow over a REAL local cluster: projects and
// memberships -> authorization -> quota -> rate limit -> queue -> the
// UNCHANGED Phase 12 orchestrator -> 2 virtual devices -> shard scheduling
// -> results -> quota release -> metrics -> audit.
//
// Cancellation races use condition variables and a blocking transport (the
// Phase 12 test rig style) — deterministic, no sleeps-for-timing.

#include <chrono>
#include <map>
#include <iostream>
#include <mutex>
#include <string>
#include <condition_variable>
#include <thread>
#include <vector>

#include "distributed/distributed.hpp"
#include "platform/platform.hpp"   // InMemoryPlatformStore (the local mirror)
#include "service/service.hpp"

using namespace vortyx::service;
using vortyx::distributed::ClusterSnapshot;
using vortyx::distributed::DistributedConfig;
using vortyx::distributed::DistributedJobRecord;
using vortyx::distributed::DistributedJobRequest;
using vortyx::distributed::DistributedJobStatus;
using vortyx::distributed::FakeClock;
using vortyx::distributed::LocalDeviceRegistry;
using vortyx::distributed::LocalInProcessTransport;
using vortyx::distributed::LocalMultiDeviceSimulator;
using vortyx::distributed::ShardResult;
using vortyx::distributed::SimulatorDeviceConfig;
using Op = vortyx::compute::ComputeOp;
using vortyx::platform::AuthContext;
using vortyx::platform::make_authenticated;
using vortyx::platform::Status;
using SS = ServiceStatus;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_status(SS actual, SS expected, const std::string& message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << " (expected " << to_string(expected) << ", got "
                  << to_string(actual) << ")\n";
        ++failures;
    }
}

// A transport that BLOCKS every dispatched shard until released — the
// deterministic way to hold a job mid-flight from another thread.
class BlockingTransport final : public vortyx::distributed::IWorkerTransport {
public:
    ShardResult submit_shard(const vortyx::distributed::ShardExecution& execution) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatched_.push_back(execution.shard_id);
        }
        dispatched_cv_.notify_all();
        std::unique_lock<std::mutex> lock(mutex_);
        released_cv_.wait(lock, [this] { return released_; });
        ShardResult result;
        result.shard_id = execution.shard_id;
        result.parent_job_id = execution.parent_job_id;
        result.shard_index = execution.shard_index;
        result.attempt = execution.attempt;
        result.device_id = execution.device_id;
        result.backend = "cpu";
        result.completed = true;
        return result;
    }
    bool cancel_shard(const std::string&) override { return true; }
    vortyx::distributed::IWorker* worker_for(const vortyx::platform::DeviceId&) override {
        return nullptr;
    }

    void wait_dispatched() {
        std::unique_lock<std::mutex> lock(mutex_);
        dispatched_cv_.wait(lock, [this] { return !dispatched_.empty(); });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        released_cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable dispatched_cv_;
    std::condition_variable released_cv_;
    std::vector<std::string> dispatched_;
    bool released_ = false;
};

// One complete service fixture: registry + transport + 2 virtual devices +
// the platform mirror + the Phase 14 service.
struct Fixture {
    std::shared_ptr<FakeClock> clock;
    LocalDeviceRegistry registry;
    LocalInProcessTransport transport;
    std::unique_ptr<BlockingTransport> blocking;      // set instead of transport
    // One simulator (worker host) per device owner: the Phase 12 cluster is
    // ownership-scoped, so a member's device needs a worker owned by the
    // member (no privileged cross-owner dispatch exists).
    std::map<std::string, std::unique_ptr<LocalMultiDeviceSimulator>> simulators;
    std::unique_ptr<InMemoryProjectStore> projects;
    std::unique_ptr<vortyx::platform::InMemoryPlatformStore> platform_store;
    std::unique_ptr<PlatformService> service;
    const std::string owner = "user-owner";
    AuthContext auth = make_authenticated("user-owner");
    std::string project_id;

    // The registry takes its clock at construction (no default ctor, no
    // move assignment — the fixture constructs both together).
    explicit Fixture(std::shared_ptr<FakeClock> c)
        : clock(std::move(c)), registry(clock) {}

    static std::unique_ptr<Fixture> make(bool use_blocking = false,
                                         const PlatformServiceConfig& config = {}) {
        std::unique_ptr<Fixture> fx(new Fixture(std::make_shared<FakeClock>(1000)));
        if (use_blocking) {
            fx->blocking = std::make_unique<BlockingTransport>();
        }
        fx->projects = std::make_unique<InMemoryProjectStore>();
        fx->projects->set_clock(fx->clock);
        fx->platform_store = std::make_unique<vortyx::platform::InMemoryPlatformStore>();

        PlatformService::Deps deps;
        deps.registry = &fx->registry;
        deps.transport = use_blocking ? static_cast<vortyx::distributed::IWorkerTransport*>(
                                            fx->blocking.get())
                                      : &fx->transport;
        deps.clock = fx->clock;
        deps.platform_store = fx->platform_store.get();
        deps.project_store = fx->projects.get();
        DistributedConfig distributed;
        distributed.enabled = true;
        // round_robin: the deterministic 2-device spread assertion in
        // scenario A needs an alternating placement (least_loaded's
        // snapshot-based tie-breaking legitimately first-fits both shards
        // onto one device).
        distributed.scheduler_policy = "round_robin";
        deps.distributed_config = distributed;

        std::string error;
        if (PlatformService::create(deps, config, fx->service, error) != SS::Ok) {
            std::cerr << "fixture: service creation failed: " << error << "\n";
            return nullptr;
        }

        ProjectRecord project;
        if (fx->service->create_project(fx->auth, "e2e", project) != SS::Ok) return nullptr;
        fx->project_id = project.project_id;
        return fx;
    }

    bool add_device(const vortyx::platform::DeviceId& id, std::int64_t memory_mb,
                    std::int64_t jobs, const std::vector<Op>& ops = {}) {
        return add_device_for(owner, id, memory_mb, jobs, ops);
    }

    bool add_device_for(const vortyx::platform::UserId& device_owner,
                        const vortyx::platform::DeviceId& id, std::int64_t memory_mb,
                        std::int64_t jobs, const std::vector<Op>& ops = {}) {
        if (blocking != nullptr) {
            // The simulator needs the REAL dispatch path; for blocking
            // scenarios devices are registered but the transport is the
            // blocking one (blocking scenarios drive shards through the
            // blocking transport only).
            return false;
        }
        // One simulator per owner (created lazily).
        auto it = simulators.find(device_owner);
        if (it == simulators.end()) {
            it = simulators
                     .emplace(device_owner,
                              std::make_unique<LocalMultiDeviceSimulator>(registry, transport,
                                                                          device_owner))
                     .first;
        }
        LocalMultiDeviceSimulator* simulator = it->second.get();
        SimulatorDeviceConfig device;
        device.device_id = id;
        device.display_name = id;
        device.capacity.memory_bytes = memory_mb * 1024 * 1024;
        device.capacity.concurrent_jobs = jobs;
        device.max_concurrent_shards = jobs;
        device.operations =
            ops.empty() ? std::vector<Op>{Op::VectorAdd, Op::VectorMultiply, Op::VectorScale}
                        : ops;
        bool created = false;
        std::string error;
        return simulator->add_device(device, created, error) == Status::Ok;
    }
};

DistributedJobRequest make_request(const std::string& id, std::size_t n, std::uint32_t shards) {
    DistributedJobRequest request;
    request.envelope.job_id = id;
    request.envelope.operation = Op::VectorAdd;
    request.envelope.element_count = n;
    request.envelope.requested_backend = "cpu";
    request.task.op = Op::VectorAdd;
    request.task.a.resize(n);
    request.task.b.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        request.task.a[i] = static_cast<std::int32_t>(i % 1000);
        request.task.b[i] = static_cast<std::int32_t>((i * 3) % 1000);
    }
    request.requested_shard_count = shards;
    return request;
}

SubmitJobRequest make_submit(const std::string& project, const std::string& id, std::size_t n,
                             std::uint32_t shards) {
    SubmitJobRequest request;
    request.project_id = project;
    request.distributed = make_request(id, n, shards);
    return request;
}

bool verify_result(const DistributedJobRecord& job, const DistributedJobRequest& request) {
    if (job.status != DistributedJobStatus::Completed) return false;
    if (job.result.data.size() != request.task.element_count()) return false;
    for (std::size_t i = 0; i < request.task.element_count(); ++i) {
        if (job.result.data[i] != request.task.a[i] + request.task.b[i]) return false;
    }
    return true;
}

}  // namespace

int main() {
    // =====================================================================
    // Scenario A: the full flow — 2 devices, a 2-shard job, real result
    // =====================================================================
    {
        std::unique_ptr<Fixture> fx = Fixture::make();
        check(fx != nullptr, "fixture A builds");
        check(fx->add_device("device-0", 64, 2), "device-0 registers");
        check(fx->add_device("device-1", 64, 2), "device-1 registers");

        ServiceJobView job;
        bool created = false;
        const SubmitJobRequest request = make_submit(fx->project_id, "job-a", 1000, 2);
        check_status(fx->service->submit_job(fx->auth, request, job, created), SS::Ok,
                     "A: submit accepted");
        check(created && job.status == DistributedJobStatus::Queued, "A: job starts Queued");

        ServiceJobView terminal;
        check_status(fx->service->wait_for_terminal(fx->auth, "job-a", 10000, terminal), SS::Ok,
                     "A: wait_for_terminal reaches a terminal state");
        check(terminal.status == DistributedJobStatus::Completed, "A: job Completed");
        check(terminal.total_shards == 2 && terminal.succeeded_shards == 2 &&
                  terminal.failed_shards == 0,
              "A: honest shard counts");

        // The Phase 12 record: both shards ran on the REAL scheduler.
        // (Per-shard device ids are cleared at terminal by the Phase 12
        // lease-release path — the mid-flight device assignment is checked
        // in scenario F2.)
        DistributedJobRecord dist;
        check_status(fx->service->distributed_record(fx->auth, "job-a", dist), SS::Ok,
                     "A: distributed record reachable");
        check(dist.shards.size() == 2, "A: two shards");
        check(dist.shards[0].state == vortyx::distributed::ShardState::Completed &&
                  dist.shards[1].state == vortyx::distributed::ShardState::Completed,
              "A: both shards executed by the scheduler");
        check(verify_result(dist, request.distributed), "A: the reassembled result is exact");

        // Quota fully released after completion.
        QuotaUsage usage;
        check_status(fx->service->project_usage(fx->auth, fx->project_id, usage), SS::Ok,
                     "A: usage readable");
        check(usage.active_jobs == 0 && usage.running_shards == 0 &&
                  usage.reserved_memory_bytes == 0,
              "A: quota released on completion");

        // The Phase 11 mirror saw the job (user-scoped store integration).
        std::vector<vortyx::platform::JobRecord> mirrored;
        check(fx->platform_store->jobs(fx->auth, mirrored) == Status::Ok && mirrored.size() == 1,
              "A: the job is mirrored into the Phase 11 store");
        check(mirrored[0].status == vortyx::platform::JobStatus::Completed,
              "A: the mirror recorded the terminal status");

        // Metrics: real counters.
        const ServiceMetricsSnapshot metrics = fx->service->metrics();
        check(metrics.submit_attempts == 1 && metrics.jobs_submitted == 1 &&
                  metrics.jobs_completed == 1 && metrics.jobs_failed == 0 &&
                  metrics.jobs_cancelled == 0,
              "A: submission counters");
    }

    // =====================================================================
    // Scenario B: security — authentication, IDOR, anti-enumeration, roles
    // =====================================================================
    {
        std::unique_ptr<Fixture> fx = Fixture::make();
        check(fx != nullptr, "fixture B builds");
        check(fx->add_device("device-0", 64, 2), "device registers");

        ServiceJobView job;
        bool created = false;

        // Unauthenticated.
        check_status(fx->service->submit_job(vortyx::platform::anonymous(),
                                             make_submit(fx->project_id, "job-b0", 10, 1), job,
                                             created),
                     SS::Unauthenticated, "B: anonymous submit refused");

        // A user with no relation to the project: NotFound (never Forbidden
        // — the project's existence is not disclosed).
        const AuthContext mallory = make_authenticated("user-mallory");
        check_status(fx->service->submit_job(mallory, make_submit(fx->project_id, "job-b1", 10, 1),
                                             job, created),
                     SS::NotFound, "B: foreign project invisible (anti-enumeration)");

        // Mallory knows the job id — still nothing (IDOR refusal).
        ServiceJobView stolen;
        check_status(fx->service->job(mallory, "job-b1", stolen), SS::NotFound,
                     "B: foreign job invisible");

        // A viewer of the project cannot submit (role gate).
        ProjectMember member;
        check_status(fx->service->add_member(fx->auth, fx->project_id, "user-viewer",
                                             ProjectRole::Viewer, member),
                     SS::Ok, "B: viewer added");
        check_status(fx->service->submit_job(make_authenticated("user-viewer"),
                                             make_submit(fx->project_id, "job-b2", 10, 1), job,
                                             created),
                     SS::Forbidden, "B: viewer submit forbidden");

        // A member can submit AND run jobs — on THEIR OWN devices (the
        // Phase 12 cluster is ownership-scoped; a service member runs on
        // the cluster they own — no privileged path to someone else's
        // devices exists, which is the point).
        check_status(fx->service->add_member(fx->auth, fx->project_id, "user-member",
                                             ProjectRole::Member, member),
                     SS::Ok, "B: member added");
        const AuthContext member_auth = make_authenticated("user-member");
        // The member's OWN device: registered by a member-owned simulator
        // (a real worker behind it — the service never dispatches across
        // owners; the Phase 12 cluster is ownership-scoped).
        check(fx->add_device_for("user-member", "device-member", 64, 2),
              "B: member registers their own device");
        check_status(fx->service->submit_job(member_auth,
                                             make_submit(fx->project_id, "job-b3", 100, 1), job,
                                             created),
                     SS::Ok, "B: member submit allowed");
        ServiceJobView cancelled;
        check_status(fx->service->wait_for_terminal(member_auth, "job-b3", 10000, cancelled),
                     SS::Ok, "B: member job terminal");
        check(cancelled.status == DistributedJobStatus::Completed, "B: member job completed");
        // Cancel on a terminal job: InvalidInput (the Phase 11 rule).
        check_status(fx->service->cancel_job(member_auth, "job-b3", cancelled), SS::InvalidInput,
                     "B: terminal cancel refused");

        // Malformed requests.
        SubmitJobRequest bad = make_submit(fx->project_id, "job-b4", 0, 1);
        check_status(fx->service->submit_job(fx->auth, bad, job, created), SS::InvalidInput,
                     "B: zero element count refused");
        bad = make_submit(fx->project_id, "job-b5", 10, 0);
        check_status(fx->service->submit_job(fx->auth, bad, job, created), SS::InvalidInput,
                     "B: zero shard count refused");
        bad = make_submit(fx->project_id, "job-b6", 10, 1000);
        check_status(fx->service->submit_job(fx->auth, bad, job, created), SS::InvalidInput,
                     "B: over-cap shard count refused");
        bad = make_submit("", "job-b7", 10, 1);
        check_status(fx->service->submit_job(fx->auth, bad, job, created), SS::InvalidInput,
                     "B: missing project refused");
    }

    // =====================================================================
    // Scenario C: quota policy — exceeded refusal, cancel release,
    //             failure release (all through the service flow)
    // =====================================================================
    {
        PlatformServiceConfig config;
        config.dispatcher_count = 1;
        std::unique_ptr<Fixture> fx = Fixture::make(false, config);
        check(fx != nullptr, "fixture C builds");
        check(fx->add_device("device-0", 64, 4), "device registers");

        // A tight project quota: one 1000-element add job (~12 KB: three
        // 4-byte int32 buffers) per turn.
        ProjectQuota tight;
        tight.max_concurrent_jobs = 1;
        tight.max_running_shards = 4;
        tight.max_memory_bytes = 16384;  // one 1000-element VectorAdd fits
        check_status(fx->service->set_project_quota(fx->auth, fx->project_id, tight), SS::Ok,
                     "C: quota set by owner");

        ServiceJobView job;
        bool created = false;
        check_status(fx->service->submit_job(fx->auth, make_submit(fx->project_id, "job-c1", 1000, 1),
                                             job, created),
                     SS::Ok, "C: first job fits");

        // While job-c1 is in flight, a second job exceeds the job quota.
        ServiceJobView refused;
        check_status(fx->service->submit_job(fx->auth, make_submit(fx->project_id, "job-c2", 1000, 1),
                                             refused, created),
                     SS::QuotaExceeded, "C: concurrent job over quota refused");
        QuotaUsage usage;
        check_status(fx->service->project_usage(fx->auth, fx->project_id, usage), SS::Ok,
                     "C: usage readable");
        check(usage.active_jobs == 1, "C: only the accepted job holds quota");

        ServiceJobView terminal;
        check_status(fx->service->wait_for_terminal(fx->auth, "job-c1", 10000, terminal), SS::Ok,
                     "C: first job terminal");
        check(terminal.status == DistributedJobStatus::Completed, "C: first completed");
        check_status(fx->service->project_usage(fx->auth, fx->project_id, usage), SS::Ok,
                     "C: usage readable");
        check(usage.active_jobs == 0, "C: completion released the reservation");

        // Cancel path: submit + cancel while queued-with-dispatch-pending is
        // racy by nature, so cancel AFTER a clean submit-then-wait is not
        // the interesting case; the deterministic cancel races live in
        // scenarios E/F. Here: submit and cancel immediately (either the
        // cancel wins in-queue or the job completes — both defined), then
        // require the quota to be exactly zero at the end either way.
        check_status(fx->service->submit_job(fx->auth, make_submit(fx->project_id, "job-c3", 100, 1),
                                             job, created),
                     SS::Ok, "C: cancel-race submission accepted");
        ServiceJobView whatever;
        fx->service->cancel_job(fx->auth, "job-c3", whatever);
        check_status(fx->service->wait_for_terminal(fx->auth, "job-c3", 10000, whatever), SS::Ok,
                     "C: cancel-race job terminal");
        check(whatever.status == DistributedJobStatus::Cancelled ||
                  whatever.status == DistributedJobStatus::Completed,
              "C: the race has a defined winner");
        check_status(fx->service->project_usage(fx->auth, fx->project_id, usage), SS::Ok,
                     "C: usage readable");
        check(usage.active_jobs == 0 && usage.reserved_memory_bytes == 0,
              "C: quota consistent after the cancel race (exactly-once release)");

        // Failure path: a device that claims only VectorAdd cannot run
        // VectorScale — the orchestrator fails the job honestly and the
        // service releases the quota.
        PlatformServiceConfig no_rate_limit;
        no_rate_limit.dispatcher_count = 1;
        std::unique_ptr<Fixture> fxFail = Fixture::make(false, no_rate_limit);
        check(fxFail != nullptr, "fixture C2 builds");
        check(fxFail->add_device("device-0", 64, 2, {Op::VectorAdd}),
              "C2: restricted device registers");
        SubmitJobRequest unsupported = make_submit(fxFail->project_id, "job-c4", 100, 1);
        unsupported.distributed.envelope.operation = Op::VectorScale;
        unsupported.distributed.task.op = Op::VectorScale;
        unsupported.distributed.task.b.clear();  // scaling takes exactly one input
        unsupported.distributed.task.scalar = 3;
        ServiceJobView failed;
        check_status(fxFail->service->submit_job(fxFail->auth, unsupported, failed, created),
                     SS::Ok, "C2: submit accepted (the refusal belongs to scheduling)");
        ServiceJobView failed_terminal;
        check_status(
            fxFail->service->wait_for_terminal(fxFail->auth, "job-c4", 10000, failed_terminal),
            SS::Ok, "C2: failed job terminal");
        check(failed_terminal.status == DistributedJobStatus::Failed,
              "C2: unsupported capability -> Failed (honest)");
        check(!failed_terminal.error.empty(), "C2: the failure carries a reason");
        check_status(fxFail->service->project_usage(fxFail->auth, fxFail->project_id, usage),
                     SS::Ok, "C2: usage readable");
        check(usage.active_jobs == 0, "C2: failure released the reservation");
        const ServiceMetricsSnapshot metrics = fxFail->service->metrics();
        check(metrics.jobs_failed == 1 && metrics.jobs_completed == 0,
              "C2: the failure is counted, never faked as success");
    }

    // =====================================================================
    // Scenario D: idempotency — replay, no double quota, conflicts
    // =====================================================================
    {
        std::unique_ptr<Fixture> fx = Fixture::make();
        check(fx != nullptr, "fixture D builds");
        check(fx->add_device("device-0", 64, 2), "device registers");

        ServiceJobView job;
        bool created = false;
        const SubmitJobRequest request = make_submit(fx->project_id, "job-d1", 1000, 2);
        check_status(fx->service->submit_job(fx->auth, request, job, created), SS::Ok,
                     "D: first submission created");
        check(created, "D: created flag set");

        ServiceJobView replay;
        check_status(fx->service->submit_job(fx->auth, request, replay, created), SS::Ok,
                     "D: replay Ok");
        check(!created, "D: replay did not create a second job");
        check(replay.job_id == "job-d1", "D: replay returns the existing record");
        // Exactly one execution: one completed job, one replay counted.
        ServiceJobView terminal;
        check_status(fx->service->wait_for_terminal(fx->auth, "job-d1", 10000, terminal), SS::Ok,
                     "D: the single execution terminal");
        const ServiceMetricsSnapshot metrics = fx->service->metrics();
        check(metrics.jobs_submitted == 1 && metrics.jobs_replayed == 1,
              "D: one submission, one replay");
        check(metrics.jobs_completed == 1, "D: the job ran exactly once");

        // Same id, different payload: Conflict.
        SubmitJobRequest different = make_submit(fx->project_id, "job-d1", 2000, 2);
        ServiceJobView conflict;
        check_status(fx->service->submit_job(fx->auth, different, conflict, created),
                     SS::Conflict, "D: different payload under the same id refused");
        // Same payload, different project: Conflict (scope mismatch).
        ProjectRecord other;
        check_status(fx->service->create_project(fx->auth, "other", other), SS::Ok,
                     "D: second project");
        SubmitJobRequest foreign_scope = make_submit(other.project_id, "job-d1", 1000, 2);
        check_status(fx->service->submit_job(fx->auth, foreign_scope, conflict, created),
                     SS::Conflict, "D: different project under the same id refused");
    }

    // =====================================================================
    // Scenario E: rate limiting over the real submission flow
    // =====================================================================
    {
        PlatformServiceConfig config;
        config.rate_limit_enabled = true;
        config.rate_limit_max_submissions = 2;
        config.rate_limit_window_ms = 60000;
        std::unique_ptr<Fixture> fx = Fixture::make(false, config);
        check(fx != nullptr, "fixture E builds");
        check(fx->add_device("device-0", 64, 4), "device registers");

        ServiceJobView job;
        bool created = false;
        check_status(fx->service->submit_job(fx->auth, make_submit(fx->project_id, "job-e1", 10, 1),
                                             job, created),
                     SS::Ok, "E: attempt 1 allowed");
        check_status(fx->service->submit_job(fx->auth, make_submit(fx->project_id, "job-e2", 10, 1),
                                             job, created),
                     SS::Ok, "E: attempt 2 allowed");
        ServiceJobView limited;
        check_status(fx->service->submit_job(fx->auth, make_submit(fx->project_id, "job-e3", 10, 1),
                                             limited, created),
                     SS::RateLimitExceeded, "E: attempt 3 rate-limited");
        const ServiceMetricsSnapshot metrics = fx->service->metrics();
        check(metrics.rate_limit_rejections == 1, "E: the rejection is counted");
        // A replay is NOT rate-limited (it creates no work).
        check_status(fx->service->submit_job(fx->auth, make_submit(fx->project_id, "job-e1", 10, 1),
                                             job, created),
                     SS::Ok, "E: replay bypasses the limiter");
        check(!created, "E: replay created nothing");
    }

    // =====================================================================
    // Scenario F: deterministic cancellation races (condvar-driven)
    // =====================================================================
    {
        // F1: cancel DURING execution — TWO devices (so the 2-shard request
        // is not coalesced by the fallback policy), 2 shards dispatched
        // SEQUENTIALLY: shard 1 blocks in the transport, the cancel arrives,
        // and Phase 12 honors it before EACH dispatch — shard 2 is cancelled
        // after the in-flight shard finished (the documented in-flight
        // semantics).
        std::unique_ptr<Fixture> fx = Fixture::make(true /* blocking */);
        check(fx != nullptr, "fixture F1 builds");
        // Register a device through the AUDITED service path (the registry
        // record exists; no simulator worker — shards block in transport).
        vortyx::distributed::DeviceCapabilities caps;
        caps.metadata.protocol_version = vortyx::platform::kProtocolVersion;
        caps.metadata.software_version = "0.15.0";
        caps.metadata.backends = {"cpu"};
        caps.metadata.operations = {"vector_add", "vector_multiply", "vector_scale"};
        caps.metadata.display_name = "blocked";
        caps.capacity.memory_bytes = 64 * 1024 * 1024;
        caps.capacity.concurrent_jobs = 2;   // both shards placed on the device
        caps.max_concurrent_shards = 2;
        bool device_created = false;
        check_status(fx->service->register_device(fx->auth, "device-b0", caps, device_created),
                     SS::Ok, "F1: device-b0 registered through the service");
        check_status(fx->service->register_device(fx->auth, "device-b1", caps, device_created),
                     SS::Ok, "F1: device-b1 registered through the service");
        // Activation: Ready + a liveness report (the Phase 12 schedulability
        // rule — Ready AND Healthy — is evidence-based, never assumed).
        for (const char* device : {"device-b0", "device-b1"}) {
            check_status(fx->service->set_device_state(
                             fx->auth, device, vortyx::distributed::DeviceState::Ready),
                         SS::Ok, "F1: device activated");
            check_status(fx->service->heartbeat_device(fx->auth, device), SS::Ok,
                         "F1: device heartbeat");
        }

        ServiceJobView job;
        bool created = false;
        check_status(fx->service->submit_job(fx->auth, make_submit(fx->project_id, "job-f1", 100, 2),
                                             job, created),
                     SS::Ok, "F1: submitted");
        fx->blocking->wait_dispatched();  // wave 1 is now in flight

        ServiceJobView cancelling;
        check_status(fx->service->cancel_job(fx->auth, "job-f1", cancelling), SS::Ok,
                     "F1: cancel accepted during execution");
        fx->blocking->release();  // the in-flight shard finishes
        ServiceJobView terminal;
        check_status(fx->service->wait_for_terminal(fx->auth, "job-f1", 10000, terminal), SS::Ok,
                     "F1: terminal reached");
        check(terminal.status == DistributedJobStatus::Cancelled,
              "F1: the in-flight wave finished, the rest cancelled");
        QuotaUsage usage;
        check_status(fx->service->project_usage(fx->auth, fx->project_id, usage), SS::Ok,
                     "F1: usage readable");
        check(usage.active_jobs == 0, "F1: cancel released the quota");

        // F2: cancel while QUEUED — fill the single device slot with a
        // blocked job, then cancel the queued one.
        std::unique_ptr<Fixture> fx2 = Fixture::make(true /* blocking */, [] {
            PlatformServiceConfig config;
            config.dispatcher_count = 1;
            return config;
        }());
        check(fx2 != nullptr, "fixture F2 builds");
        check_status(fx2->service->register_device(fx2->auth, "device-b1", caps, device_created),
                     SS::Ok, "F2: device registered");
        check_status(fx2->service->set_device_state(fx2->auth, "device-b1",
                                                    vortyx::distributed::DeviceState::Ready),
                     SS::Ok, "F2: device activated");
        check_status(fx2->service->heartbeat_device(fx2->auth, "device-b1"), SS::Ok,
                     "F2: device heartbeat");

        // Job 1 occupies the device (blocked in flight). While it holds, the
        // distributed record shows the REAL scheduler's device assignment.
        check_status(fx2->service->submit_job(fx2->auth,
                                              make_submit(fx2->project_id, "job-f2a", 100, 1), job,
                                              created),
                     SS::Ok, "F2: blocker submitted");
        fx2->blocking->wait_dispatched();
        DistributedJobRecord in_flight;
        check_status(fx2->service->distributed_record(fx2->auth, "job-f2a", in_flight), SS::Ok,
                     "F2: in-flight record reachable");
        check(in_flight.shards.size() == 1 &&
                  in_flight.shards[0].assigned_device == "device-b1",
              "F2: the scheduler really assigned the device mid-flight");

        // Job 2 stays queued while the blocker holds the device.
        check_status(fx2->service->submit_job(fx2->auth,
                                              make_submit(fx2->project_id, "job-f2b", 100, 1), job,
                                              created),
                     SS::Ok, "F2: queued job submitted");
        ServiceJobView queued_view;
        check_status(fx2->service->job(fx2->auth, "job-f2b", queued_view), SS::Ok,
                     "F2: queued job readable");
        check(queued_view.status == DistributedJobStatus::Queued, "F2: still Queued");

        ServiceJobView queue_cancelled;
        check_status(fx2->service->cancel_job(fx2->auth, "job-f2b", queue_cancelled), SS::Ok,
                     "F2: cancel while queued accepted");
        check(queue_cancelled.status == DistributedJobStatus::Cancelled,
              "F2: cancelled without dispatch");
        fx2->blocking->release();
        ServiceJobView blocker_terminal;
        check_status(fx2->service->wait_for_terminal(fx2->auth, "job-f2a", 10000, blocker_terminal),
                     SS::Ok, "F2: blocker terminal");
        check(blocker_terminal.status == DistributedJobStatus::Completed,
              "F2: the blocker completed (never affected by the other cancel)");
        ServiceJobView f2b_terminal;
        check_status(fx2->service->job(fx2->auth, "job-f2b", f2b_terminal), SS::Ok,
                     "F2: cancelled job still queryable");
        check(f2b_terminal.status == DistributedJobStatus::Cancelled,
              "F2: the queued-cancel stays Cancelled");
        check_status(fx2->service->project_usage(fx2->auth, fx2->project_id, usage), SS::Ok,
                     "F2: usage readable");
        check(usage.active_jobs == 0, "F2: both reservations released exactly once");
    }

    // =====================================================================
    // Scenario G: archived projects refuse submissions (UnsupportedOperation)
    // =====================================================================
    {
        std::unique_ptr<Fixture> fx = Fixture::make();
        check(fx != nullptr, "fixture G builds");
        ProjectRecord archived;
        check_status(fx->service->archive_project(fx->auth, fx->project_id, archived), SS::Ok,
                     "G: archived");
        ServiceJobView job;
        bool created = false;
        check_status(fx->service->submit_job(fx->auth, make_submit(fx->project_id, "job-g1", 10, 1),
                                             job, created),
                     SS::UnsupportedOperation, "G: archived submission refused");
    }

    // =====================================================================
    // Scenario H: concurrency — parallel submissions, no overcommit,
    //             consistent end state
    // =====================================================================
    {
        PlatformServiceConfig config;
        config.dispatcher_count = 2;
        std::unique_ptr<Fixture> fx = Fixture::make(false, config);
        check(fx != nullptr, "fixture H builds");
        check(fx->add_device("device-0", 128, 4), "device-0 registers");
        check(fx->add_device("device-1", 128, 4), "device-1 registers");

        // 8 CONCURRENT jobs exceed the default quota (4) — raise the
        // project's policy for this scenario (the honest way to submit a
        // larger storm; refusals past the quota are tested in C).
        ProjectQuota storm;
        storm.max_concurrent_jobs = 8;
        storm.max_running_shards = 8;
        storm.max_memory_bytes = 64LL * 1024 * 1024;
        check_status(fx->service->set_project_quota(fx->auth, fx->project_id, storm), SS::Ok,
                     "H: storm quota set");

        constexpr int kJobs = 8;
        std::atomic<int> ok_count{0};
        std::vector<std::thread> submitters;
        std::vector<std::string> ids(kJobs);
        for (int i = 0; i < kJobs; ++i) ids[i] = "job-h" + std::to_string(i);
        for (int i = 0; i < kJobs; ++i) {
            submitters.emplace_back([&fx, &ids, &ok_count, i]() {
                ServiceJobView job;
                bool created = false;
                // Concurrent duplicate submissions of the SAME id from two
                // of the threads: exactly one created, the other a replay.
                const SubmitJobRequest request = make_submit(fx->project_id, ids[i], 200, 1);
                const SS status = fx->service->submit_job(fx->auth, request, job, created);
                if (status == SS::Ok) ok_count.fetch_add(1);
            });
        }
        // A second wave racing on the SAME ids (idempotency under
        // concurrency: replays, never duplicates).
        for (int i = 0; i < kJobs; ++i) {
            submitters.emplace_back([&fx, &ids, &ok_count, i]() {
                ServiceJobView job;
                bool created = false;
                const SS status =
                    fx->service->submit_job(fx->auth, make_submit(fx->project_id, ids[i], 200, 1),
                                            job, created);
                if (status == SS::Ok) ok_count.fetch_add(1);
            });
        }
        for (std::thread& submitter : submitters) submitter.join();

        // All 8 jobs terminal and exactly-once each.
        int completed = 0;
        for (int i = 0; i < kJobs; ++i) {
            ServiceJobView terminal;
            const SS wait_status =
                fx->service->wait_for_terminal(fx->auth, ids[i], 20000, terminal);
            if (wait_status != SS::Ok) {
                std::cerr << "DEBUG H " << ids[i] << " wait=" << to_string(wait_status)
                          << " status="
                          << vortyx::distributed::to_string(terminal.status)
                          << " queue_depth=" << fx->service->metrics().jobs_queued
                          << " running=" << fx->service->metrics().jobs_running << "\n";
                check(false, "H: job " + ids[i] + " reached terminal");
                continue;
            }
            check(terminal.status == DistributedJobStatus::Completed,
                  "H: " + ids[i] + " completed");
            if (terminal.status == DistributedJobStatus::Completed) ++completed;
        }
        check(completed == kJobs, "H: every job completed exactly once");
        const ServiceMetricsSnapshot metrics = fx->service->metrics();
        check(metrics.jobs_submitted == kJobs && metrics.jobs_replayed == kJobs,
              "H: 8 created + 8 replays (no duplicates)");
        check(metrics.jobs_completed == kJobs && metrics.jobs_failed == 0 &&
                  metrics.jobs_cancelled == 0,
              "H: honest terminal counters");
        QuotaUsage usage;
        check_status(fx->service->project_usage(fx->auth, fx->project_id, usage), SS::Ok,
                     "H: usage readable");
        check(usage.active_jobs == 0 && usage.running_shards == 0 &&
                  usage.reserved_memory_bytes == 0,
              "H: quota fully consistent after the storm");
    }

    if (failures == 0) {
        std::cout << "Service job lifecycle tests passed.\n";
        return 0;
    }
    std::cerr << failures << " service job test(s) failed.\n";
    return 1;
}
