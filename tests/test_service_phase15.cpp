// Phase 15 service contract tests.
//
// The Phase 14 contract fixes, pinned end to end:
//   A. PRIVILEGED CROSS-USER CANCELLATION: a project Admin cancels another
//      member's RUNNING job through the explicit trusted-service path — the
//      cancellation actually reaches the executing record (the Phase 14
//      bug: the owner-only orchestrator path answered NotFound and the
//      cancel was silently lost). Identity swapping is impossible (the
//      trusted context cannot be minted outside the service); the action is
//      audited as privileged.
//   B. SINGLE-OWNER INVARIANT at the facade: no membership path grants the
//      Owner role.
//   C. ARTIFACTS bounded per project + deletion with creator/admin
//      authorization and cross-project invisibility.
//   D. CANCELLATION INTENT delivery (the orchestrator contract the service
//      now uses): a cancellation racing the record-creation window is
//      delivered atomically — no sleep-poll loop anywhere.

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "distributed/distributed.hpp"
#include "platform/platform.hpp"
#include "service/service.hpp"

using namespace vortyx::service;
using vortyx::distributed::DistributedConfig;
using vortyx::distributed::DistributedJobRecord;
using vortyx::distributed::DistributedJobRequest;
using vortyx::distributed::DistributedJobStatus;
using vortyx::distributed::FakeClock;
using vortyx::distributed::LocalDeviceRegistry;
using vortyx::distributed::LocalInProcessTransport;
using vortyx::distributed::LocalMultiDeviceSimulator;
using Op = vortyx::compute::ComputeOp;
using vortyx::platform::AuthContext;
using vortyx::platform::make_authenticated;
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

// The same deterministic blocking transport the Phase 14 jobs test uses: a
// dispatched shard blocks until released, so a cancel can arrive while the
// job is genuinely mid-flight.
class BlockingTransport final : public vortyx::distributed::IWorkerTransport {
public:
    vortyx::distributed::ShardResult submit_shard(
        const vortyx::distributed::ShardExecution& execution) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatched_ += 1;
        }
        dispatched_cv_.notify_all();
        std::unique_lock<std::mutex> lock(mutex_);
        released_cv_.wait(lock, [this] { return released_; });
        vortyx::distributed::ShardResult result;
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
        dispatched_cv_.wait(lock, [this] { return dispatched_ > 0; });
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
    int dispatched_ = 0;
    bool released_ = false;
};

struct Fixture {
    std::shared_ptr<FakeClock> clock;
    LocalDeviceRegistry registry;
    LocalInProcessTransport transport;
    std::unique_ptr<BlockingTransport> blocking;
    std::unique_ptr<InMemoryProjectStore> projects;
    std::unique_ptr<vortyx::platform::InMemoryPlatformStore> platform_store;
    std::unique_ptr<PlatformService> service;
    const std::string owner = "user-owner";
    AuthContext auth = make_authenticated(owner);
    std::string project_id;

    explicit Fixture(std::shared_ptr<FakeClock> c) : clock(std::move(c)), registry(clock) {}

    static std::unique_ptr<Fixture> make() {
        std::unique_ptr<Fixture> fx(new Fixture(std::make_shared<FakeClock>(1000)));
        fx->blocking = std::make_unique<BlockingTransport>();
        fx->projects = std::make_unique<InMemoryProjectStore>();
        fx->projects->set_clock(fx->clock);
        fx->platform_store = std::make_unique<vortyx::platform::InMemoryPlatformStore>();

        PlatformService::Deps deps;
        deps.registry = &fx->registry;
        deps.transport = fx->blocking.get();
        deps.clock = fx->clock;
        deps.platform_store = fx->platform_store.get();
        deps.project_store = fx->projects.get();
        DistributedConfig distributed;
        distributed.enabled = true;
        distributed.scheduler_policy = "round_robin";
        deps.distributed_config = distributed;

        std::string error;
        if (PlatformService::create(deps, PlatformServiceConfig{}, fx->service, error) !=
            SS::Ok) {
            std::cerr << "fixture: service creation failed: " << error << "\n";
            return nullptr;
        }

        ProjectRecord project;
        if (fx->service->create_project(fx->auth, "p15", project) != SS::Ok) return nullptr;
        fx->project_id = project.project_id;
        return fx;
    }

    // Registers two schedulable (but transport-blocked) devices.
    bool add_blocked_devices() {
        vortyx::distributed::DeviceCapabilities caps;
        caps.metadata.protocol_version = vortyx::platform::kProtocolVersion;
        caps.metadata.software_version = "0.15.0";
        caps.metadata.backends = {"cpu"};
        caps.metadata.operations = {"vector_add", "vector_multiply", "vector_scale"};
        caps.metadata.display_name = "blocked";
        caps.capacity.memory_bytes = 64 * 1024 * 1024;
        caps.capacity.concurrent_jobs = 2;
        caps.max_concurrent_shards = 2;
        for (const char* device : {"device-b0", "device-b1"}) {
            bool created = false;
            if (service->register_device(auth, device, caps, created) != SS::Ok) return false;
            if (service->set_device_state(auth, device,
                                          vortyx::distributed::DeviceState::Ready) != SS::Ok) {
                return false;
            }
            if (service->heartbeat_device(auth, device) != SS::Ok) return false;
        }
        return true;
    }
};

SubmitJobRequest make_submit(const std::string& project, const std::string& id, std::size_t n,
                             std::uint32_t shards) {
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
    SubmitJobRequest submit;
    submit.project_id = project;
    submit.distributed = request;
    return submit;
}

bool has_audit_event(const std::vector<AuditEvent>& events, AuditAction action,
                     const std::string& reason_substring, const std::string& actor) {
    for (const AuditEvent& event : events) {
        if (event.action != action) continue;
        if (!actor.empty() && event.actor_user_id != actor) continue;
        if (reason_substring.empty() ||
            event.reason_code.find(reason_substring) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    // =====================================================================
    // A. Privileged cross-user cancellation — the Phase 14 contract fix.
    // =====================================================================
    {
        std::unique_ptr<Fixture> fx = Fixture::make();
        check(fx != nullptr, "fixture A builds");
        check(fx->add_blocked_devices(), "A: blocked devices registered");

        // A second user with the Admin role (no ownership, by construction).
        const std::string admin = "user-admin";
        AuthContext admin_auth = make_authenticated(admin);
        ProjectMember admin_member;
        check_status(fx->service->add_member(fx->auth, fx->project_id, admin,
                                             ProjectRole::Admin, admin_member),
                     SS::Ok, "A: admin added");

        // The OWNER submits a 2-shard job; it blocks mid-flight.
        ServiceJobView job;
        bool created = false;
        check_status(fx->service->submit_job(fx->auth,
                                             make_submit(fx->project_id, "job-priv", 100, 2),
                                             job, created),
                     SS::Ok, "A: owner submitted");
        fx->blocking->wait_dispatched();

        // Pre-fix behavior pinned first: a MEMBER (no CancelAnyJob) cannot
        // cancel someone else's job.
        const std::string member = "user-member";
        AuthContext member_auth = make_authenticated(member);
        ProjectMember member_record;
        check_status(fx->service->add_member(fx->auth, fx->project_id, member,
                                             ProjectRole::Member, member_record),
                     SS::Ok, "A: member added");
        ServiceJobView refused;
        check_status(fx->service->cancel_job(member_auth, "job-priv", refused), SS::Forbidden,
                     "A: member foreign-cancel is Forbidden");

        // THE FIX: the admin's foreign cancel now reaches the executing
        // record (Phase 14 lost it as orchestrator NotFound).
        ServiceJobView cancelling;
        check_status(fx->service->cancel_job(admin_auth, "job-priv", cancelling), SS::Ok,
                     "A: admin privileged cancel accepted during execution");
        fx->blocking->release();
        ServiceJobView terminal;
        check_status(fx->service->wait_for_terminal(fx->auth, "job-priv", 10000, terminal),
                     SS::Ok, "A: terminal reached");
        check(terminal.status == DistributedJobStatus::Cancelled,
              "A: the admin's cancel actually cancelled the running job");

        // The privileged action is AUDITED with the admin's real identity.
        const std::vector<AuditEvent> admin_audit =
            fx->service->audit_tail_for_actor(admin_auth, 100);
        check(has_audit_event(admin_audit, AuditAction::JobCancel, "privileged:cancel_any_job",
                              admin),
              "A: the privileged cancel is audited with the acting admin's identity");

        // Cancelling an already-terminal job stays InvalidInput (honest).
        ServiceJobView after_terminal;
        check_status(fx->service->cancel_job(admin_auth, "job-priv", after_terminal),
                     SS::InvalidInput, "A: cancelling a terminal job is refused");

        // A foreign (non-member) user still gets NotFound (anti-enumeration
        // unchanged at the service boundary).
        AuthContext outsider = make_authenticated("user-outsider");
        ServiceJobView invisible;
        check_status(fx->service->cancel_job(outsider, "job-priv", invisible), SS::NotFound,
                     "A: a foreign user's cancel is NotFound, never an error leak");
    }

    // =====================================================================
    // B. The single-owner invariant at the facade.
    // =====================================================================
    {
        std::unique_ptr<Fixture> fx = Fixture::make();
        check(fx != nullptr, "fixture B builds");

        // The Owner role is never grantable — through any role's hands.
        ProjectMember nope;
        check_status(fx->service->add_member(fx->auth, fx->project_id, "user-x",
                                             ProjectRole::Owner, nope),
                     SS::InvalidInput, "B: owner cannot grant Owner");
        const std::string admin = "user-admin";
        ProjectMember admin_member;
        check_status(fx->service->add_member(fx->auth, fx->project_id, admin,
                                             ProjectRole::Admin, admin_member),
                     SS::Ok, "B: admin added");
        AuthContext admin_auth = make_authenticated(admin);
        check_status(fx->service->add_member(admin_auth, fx->project_id, "user-y",
                                             ProjectRole::Owner, nope),
                     SS::InvalidInput, "B: admin cannot grant Owner");
        // The refusal is audited (a denied membership change).
        const std::vector<AuditEvent> admin_audit =
            fx->service->audit_tail_for_actor(admin_auth, 100);
        check(has_audit_event(admin_audit, AuditAction::MembershipChange, "", admin),
              "B: the refused owner grant is audited");

        // Members still get their real role; the project still has exactly
        // one owner.
        ProjectMember member;
        check_status(fx->service->add_member(admin_auth, fx->project_id, "user-m",
                                             ProjectRole::Member, member),
                     SS::Ok, "B: admin adds a member");
        std::vector<ProjectMember> members;
        check_status(fx->service->members(fx->auth, fx->project_id, members), SS::Ok,
                     "B: members listed");
        int owner_count = 0;
        for (const ProjectMember& m : members) {
            if (m.role == ProjectRole::Owner) owner_count += 1;
        }
        check(owner_count == 1, "B: exactly one owner after every attempt");
    }

    // =====================================================================
    // C. Artifacts: bounded registry, deletion authorization, isolation.
    // =====================================================================
    {
        std::unique_ptr<Fixture> fx = Fixture::make();
        check(fx != nullptr, "fixture C builds");
        const std::string member = "user-member";
        AuthContext member_auth = make_authenticated(member);
        ProjectMember member_record;
        check_status(fx->service->add_member(fx->auth, fx->project_id, member,
                                             ProjectRole::Member, member_record),
                     SS::Ok, "C: member added");
        // A second project the member owns (for the cross-project check).
        ProjectRecord other_project;
        check_status(fx->service->create_project(member_auth, "member-own", other_project),
                     SS::Ok, "C: second project created");

        ArtifactMetadata artifact;
        check_status(fx->service->register_artifact(member_auth, fx->project_id, "model-a",
                                                    1234, artifact),
                     SS::Ok, "C: artifact registered");

        // Creator deletion: allowed. Re-deletion: NotFound.
        check_status(fx->service->delete_artifact(member_auth, fx->project_id,
                                                  artifact.artifact_id),
                     SS::Ok, "C: creator deletes own artifact");
        check_status(fx->service->delete_artifact(member_auth, fx->project_id,
                                                  artifact.artifact_id),
                     SS::NotFound, "C: re-deletion is NotFound");

        // Admin may delete someone else's artifact; a member may NOT.
        ArtifactMetadata admin_artifact;
        check_status(
            fx->service->register_artifact(fx->auth, fx->project_id, "owner-file", 10,
                                           admin_artifact),
            SS::Ok, "C: owner artifact registered");
        check_status(fx->service->delete_artifact(member_auth, fx->project_id,
                                                  admin_artifact.artifact_id),
                     SS::Forbidden, "C: member cannot delete another's artifact");
        check_status(fx->service->delete_artifact(fx->auth, fx->project_id,
                                                  admin_artifact.artifact_id),
                     SS::Ok, "C: owner (Admin+) deletes another's artifact");

        // Cross-project reference: invisible (NotFound, never a leak).
        ArtifactMetadata foreign;
        check_status(fx->service->register_artifact(fx->auth, fx->project_id, "in-p1", 5,
                                                    foreign),
                     SS::Ok, "C: artifact in first project");
        check_status(fx->service->delete_artifact(member_auth, other_project.project_id,
                                                  foreign.artifact_id),
                     SS::NotFound,
                     "C: a cross-project artifact reference is invisible (NotFound)");
        AuthContext outsider = make_authenticated("user-outsider");
        check_status(fx->service->delete_artifact(outsider, fx->project_id,
                                                  foreign.artifact_id),
                     SS::NotFound, "C: a foreign user gets NotFound (anti-enumeration)");

        // The bound: kMaxArtifactsPerProject per project, honestly reported.
        for (std::size_t i = 0; i < kMaxArtifactsPerProject; ++i) {
            ArtifactMetadata filler;
            const SS status = fx->service->register_artifact(
                member_auth, other_project.project_id, "filler-" + std::to_string(i), 1,
                filler);
            if (status != SS::Ok) {
                check(false, "C: filler registration " + std::to_string(i) + " failed");
                break;
            }
        }
        ArtifactMetadata overflow;
        check_status(fx->service->register_artifact(member_auth, other_project.project_id,
                                                    "one-too-many", 1, overflow),
                     SS::Unavailable, "C: the per-project capacity refusal is honest");
        // Deletion frees capacity — the bound is a policy, not a corruption.
        std::vector<ArtifactMetadata> list;
        check_status(fx->service->artifacts(member_auth, other_project.project_id, list),
                     SS::Ok, "C: artifacts listed");
        check_status(fx->service->delete_artifact(member_auth, other_project.project_id,
                                                  list.front().artifact_id),
                     SS::Ok, "C: one filler deleted");
        ArtifactMetadata after_delete;
        check_status(fx->service->register_artifact(member_auth, other_project.project_id,
                                                    "fits-again", 1, after_delete),
                     SS::Ok, "C: capacity is available again after deletion");

        // Deletions are audited.
        const std::vector<AuditEvent> member_audit =
            fx->service->audit_tail_for_actor(member_auth, 500);
        check(has_audit_event(member_audit, AuditAction::ArtifactDelete, "", member),
              "C: artifact deletions are audited");
    }

    // =====================================================================
    // D. Cancellation-intent delivery at the orchestrator (the mechanism
    //    the service now uses instead of the Phase 14 sleep-poll loop).
    // =====================================================================
    {
        std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>(1000);
        LocalDeviceRegistry registry(clock);
        LocalInProcessTransport transport;
        // One schedulable virtual device (the D2 job never reaches it — the
        // intent cancels the job at its first boundary — but a deviceless
        // cluster would fail placement for a different reason).
        LocalMultiDeviceSimulator simulator(registry, transport, "device-owner");
        std::string error;
        {
            vortyx::distributed::SimulatorDeviceConfig device;
            device.device_id = "device-d0";
            device.display_name = "d0";
            device.capacity.memory_bytes = 64 * 1024 * 1024;
            device.capacity.concurrent_jobs = 2;
            device.max_concurrent_shards = 2;
            bool created_device = false;
            check(simulator.add_device(device, created_device, error) ==
                      vortyx::platform::Status::Ok,
                  "D: simulator device registered");
        }

        vortyx::distributed::DistributedOrchestrator::Deps deps;
        deps.registry = &registry;
        deps.transport = &transport;
        deps.clock = clock;
        vortyx::distributed::DistributedConfig config;
        config.enabled = true;
        std::unique_ptr<vortyx::distributed::DistributedOrchestrator> orchestrator;
        check(vortyx::distributed::DistributedOrchestrator::create(deps, config,
                                                                   orchestrator, error) ==
                  vortyx::platform::Status::Ok,
              "D: orchestrator built");

        const AuthContext owner = make_authenticated("user-owner");

        // D1: the Phase 12 contract is UNCHANGED under the default delivery:
        // an unknown job is NotFound (RefuseUnknown).
        DistributedJobRecord record;
        check(orchestrator->cancel_job(owner, "job-unknown", record) ==
                  vortyx::platform::Status::NotFound,
              "D1: default cancel on an unknown job stays NotFound");

        // D2: RecordIntent delivers a cancellation that races the
        // record-creation window — the intent is applied at record
        // creation and the job completes as Cancelled with ZERO shards
        // executed.
        check(orchestrator->cancel_job(owner, "job-intent", record,
                                       vortyx::distributed::CancelDelivery::RecordIntent) ==
                  vortyx::platform::Status::Ok,
              "D2: RecordIntent accepts the not-yet-visible job");
        DistributedJobRequest request;
        request.envelope.job_id = "job-intent";
        request.envelope.operation = Op::VectorAdd;
        request.envelope.element_count = 100;
        request.envelope.requested_backend = "cpu";
        request.task.op = Op::VectorAdd;
        request.task.a.assign(100, 1);
        request.task.b.assign(100, 2);
        request.requested_shard_count = 1;
        bool created = false;
        check(orchestrator->submit(owner, request, record, created) ==
                  vortyx::platform::Status::Ok,
              "D2: the racing submit is accepted");
        check(record.status == DistributedJobStatus::Cancelled,
              "D2: the intent turned the job Cancelled at its first boundary");
        bool shard_executed = false;
        for (const vortyx::distributed::JobShard& shard : record.shards) {
            if (shard.state == vortyx::distributed::ShardState::Completed) shard_executed = true;
        }
        check(!shard_executed, "D2: no shard actually executed");
    }

    if (failures == 0) {
        std::cout << "All Phase 15 service contract tests passed\n";
        return 0;
    }
    std::cerr << failures << " Phase 15 service contract test(s) failed\n";
    return 1;
}
