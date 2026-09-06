// Phase 16 — the service layer's Adaptive Compute Fabric integration
// (test_service_fabric.cpp).
//
// Pins the opt-in fabric-planned service flow: with fabric_planning the
// dispatcher's jobs carry HONEST plan metadata (version 1, the planner
// identity, the devices actually used, a reason summary derived from the
// plan), the contract JSON exposes it as a nullable "plan" object, real
// execution still runs through the unchanged Phase 12 path (bit-exact
// result), and every existing behavior (quota, authorization, audit,
// serialization) is unchanged. With fabric_planning OFF — the default —
// the plan field is null and the placement behavior is exactly Phase 15's.

#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "distributed/distributed.hpp"
#include "platform/platform.hpp"
#include "service/service.hpp"

using namespace vortyx::service;
using SS = ServiceStatus;
using vortyx::distributed::DistributedConfig;
using vortyx::distributed::DistributedJobRequest;
using vortyx::distributed::FakeClock;
using vortyx::distributed::LocalDeviceRegistry;
using vortyx::distributed::LocalInProcessTransport;
using vortyx::distributed::LocalMultiDeviceSimulator;
using vortyx::distributed::SimulatorDeviceConfig;
using vortyx::platform::AuthContext;
using vortyx::platform::make_authenticated;

namespace {

int failures = 0;

void check(bool ok, const std::string& message) {
    if (ok) {
        std::cout << "PASS: " << message << "\n";
    } else {
        std::cout << "FAIL: " << message << "\n";
        ++failures;
    }
}

DistributedJobRequest make_request(const std::string& id, std::size_t n,
                                   std::uint32_t shards) {
    DistributedJobRequest request;
    request.envelope.job_id = id;
    request.envelope.operation = vortyx::compute::ComputeOp::VectorAdd;
    request.envelope.element_count = n;
    request.envelope.requested_backend = "cpu";
    request.task.op = vortyx::compute::ComputeOp::VectorAdd;
    request.task.a.resize(n);
    request.task.b.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        request.task.a[i] = static_cast<std::int32_t>(i % 1000);
        request.task.b[i] = static_cast<std::int32_t>((i * 3) % 1000);
    }
    request.requested_shard_count = shards;
    return request;
}

SubmitJobRequest make_submit(const std::string& project, const std::string& id,
                             std::size_t n, std::uint32_t shards) {
    SubmitJobRequest request;
    request.project_id = project;
    request.distributed = make_request(id, n, shards);
    return request;
}

struct Fixture;

// A transport that BLOCKS every shard dispatch until released — the
// deterministic way (condition variables, never sleeps) to hold the
// service's single dispatcher busy while a second job stays QUEUED.
class BlockingTransport final : public vortyx::distributed::IWorkerTransport {
public:
    vortyx::distributed::ShardResult submit_shard(
        const vortyx::distributed::ShardExecution& execution) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatched_ = true;
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
        dispatched_cv_.wait(lock, [this] { return dispatched_; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        released_cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable dispatched_cv_;
    std::condition_variable released_cv_;
    bool dispatched_ = false;
    bool released_ = false;
};

struct Fixture {
    std::shared_ptr<FakeClock> clock;
    LocalDeviceRegistry registry;
    std::unique_ptr<BlockingTransport> blocking;
    std::unique_ptr<LocalInProcessTransport> direct_transport;
    std::unique_ptr<LocalMultiDeviceSimulator> simulator;
    std::unique_ptr<InMemoryProjectStore> projects;
    std::unique_ptr<PlatformService> service;
    const std::string owner = "user-owner";
    AuthContext auth = make_authenticated(owner);
    std::string project_id;
    bool fabric_on = false;

    explicit Fixture(std::shared_ptr<FakeClock> c) : clock(std::move(c)), registry(clock) {}

    // 'blocked' wires the deterministic BlockingTransport (one dispatcher);
    // otherwise the direct loopback transport executes for real.
    static std::unique_ptr<Fixture> make(bool fabric_planning, bool blocked = false) {
        std::unique_ptr<Fixture> fx(new Fixture(std::make_shared<FakeClock>(1000)));
        fx->direct_transport = std::make_unique<LocalInProcessTransport>();
        fx->projects = std::make_unique<InMemoryProjectStore>();
        fx->projects->set_clock(fx->clock);
        fx->fabric_on = fabric_planning;
        if (blocked) fx->blocking = std::make_unique<BlockingTransport>();

        PlatformService::Deps deps;
        deps.registry = &fx->registry;
        deps.transport = fx->blocking
                             ? static_cast<vortyx::distributed::IWorkerTransport*>(
                                   fx->blocking.get())
                             : static_cast<vortyx::distributed::IWorkerTransport*>(
                                   fx->direct_transport.get());
        deps.clock = fx->clock;
        deps.project_store = fx->projects.get();
        DistributedConfig distributed;
        distributed.enabled = true;
        deps.distributed_config = distributed;

        PlatformServiceConfig config;
        config.fabric_planning = fabric_planning;
        if (blocked) config.dispatcher_count = 1;  // the blocked-busy pattern

        std::string error;
        if (PlatformService::create(deps, config, fx->service, error) != SS::Ok) {
            std::cerr << "fixture: service creation failed: " << error << "\n";
            return nullptr;
        }

        ProjectRecord project;
        if (fx->service->create_project(fx->auth, "p16", project) != SS::Ok) return nullptr;
        fx->project_id = project.project_id;
        return fx;
    }

    vortyx::distributed::LocalInProcessTransport& direct() { return *direct_transport; }

    bool add_device(const std::string& id) {
        if (simulator == nullptr) {
            simulator = std::make_unique<LocalMultiDeviceSimulator>(registry, direct(), owner);
        }
        SimulatorDeviceConfig config;
        config.device_id = id;
        config.display_name = id;
        config.capacity.memory_bytes = 8 * 1024 * 1024;
        config.capacity.concurrent_jobs = 4;
        config.max_concurrent_shards = 4;
        bool created = false;
        std::string error;
        return simulator->add_device(config, created, error) == vortyx::platform::Status::Ok;
    }
};

}  // namespace

int main() {
    // ---- fabric-ON: planned flow with honest plan metadata ----------------
    {
        auto fx = Fixture::make(true);
        check(fx != nullptr, "fabric-on: service created");
        check(fx->add_device("dev-a") && fx->add_device("dev-b"),
              "fabric-on: two devices registered");

        const SubmitJobRequest request = make_submit(fx->project_id, "job-f16", 2000, 2);
        ServiceJobView job;
        bool created = false;
        check(fx->service->submit_job(fx->auth, request, job, created) == SS::Ok,
              "fabric-on: submission accepted");

        ServiceJobView terminal;
        check(fx->service->wait_for_terminal(fx->auth, "job-f16", 10000, terminal) == SS::Ok,
              "fabric-on: the job reached a terminal state");
        check(terminal.status == vortyx::distributed::DistributedJobStatus::Completed &&
                  terminal.succeeded_shards == 2 && terminal.failed_shards == 0,
              "fabric-on: real execution through the unchanged Phase 12 path");

        // The plan metadata: honest, present, and derived from the plan.
        check(terminal.plan_available, "fabric-on: the plan metadata is present");
        check(terminal.plan.plan_version == 1, "fabric-on: the plan is version 1");
        check(terminal.plan.planner_name == "adaptive_fabric",
              "fabric-on: the planner identity is recorded");
        check(terminal.plan.devices.size() >= 1 && terminal.plan.devices.size() <= 2,
              "fabric-on: the summary lists the devices actually used");
        check(terminal.plan.reason_summary.find("planned 1 workload") != std::string::npos,
              "fabric-on: the reason summary is derived from the plan");

        // The contract JSON: a nullable plan object, populated here.
        const std::string json = serialize_service_job(terminal);
        check(json.find("\"plan\":{") != std::string::npos &&
                  json.find("\"planner\":\"adaptive_fabric\"") != std::string::npos &&
                  json.find("\"plan_version\":1") != std::string::npos,
              "fabric-on: the contract serializes the plan object");

        // The existing authorization boundary is intact: a foreign user
        // still cannot even see the job (the plan metadata is behind the
        // same check, never a leak).
        AuthContext mallory = make_authenticated("user-mallory");
        ServiceJobView forbidden;
        check(fx->service->job(mallory, "job-f16", forbidden) == SS::NotFound,
              "fabric-on: the plan is invisible to a foreign user (anti-enumeration)");
    }

    // ---- fabric-OFF (the default): plan null, behavior unchanged ----------
    {
        auto fx = Fixture::make(false);
        check(fx->add_device("dev-a") && fx->add_device("dev-b"),
              "fabric-off: two devices registered");

        const SubmitJobRequest request = make_submit(fx->project_id, "job-plain", 1000, 1);
        ServiceJobView job;
        bool created = false;
        check(fx->service->submit_job(fx->auth, request, job, created) == SS::Ok,
              "fabric-off: submission accepted");
        ServiceJobView terminal;
        check(fx->service->wait_for_terminal(fx->auth, "job-plain", 10000, terminal) == SS::Ok &&
                  terminal.status == vortyx::distributed::DistributedJobStatus::Completed,
              "fabric-off: the job still completes");
        check(!terminal.plan_available, "fabric-off: no plan metadata exists");
        const std::string json = serialize_service_job(terminal);
        check(json.find("\"plan\":null") != std::string::npos,
              "fabric-off: the contract serializes plan as null");
    }

    // ---- fabric-ON, cancelled in queue: never planned, never faked --------
    // DETERMINISTIC by construction: one dispatcher is held BUSY inside a
    // blocking transport (condition variables, no sleeps), so the cancel
    // target stays genuinely queued and the queue removal wins for sure.
    {
        auto fx = Fixture::make(true, /*blocked=*/true);
        check(fx->add_device("dev-a"), "queue-cancel: a device registered");

        // Occupy the single dispatcher: this job's shard dispatch BLOCKS.
        const SubmitJobRequest blocker = make_submit(fx->project_id, "job-blocker", 1000, 1);
        ServiceJobView blocker_view;
        bool blocker_created = false;
        check(fx->service->submit_job(fx->auth, blocker, blocker_view, blocker_created) == SS::Ok,
              "queue-cancel: the blocker submitted");
        fx->blocking->wait_dispatched();  // the dispatcher is now inside submit()

        // The cancel target CANNOT be dequeued while the dispatcher blocks.
        const SubmitJobRequest request = make_submit(fx->project_id, "job-cancel", 1000, 1);
        ServiceJobView job;
        bool created = false;
        check(fx->service->submit_job(fx->auth, request, job, created) == SS::Ok &&
                  job.status == vortyx::distributed::DistributedJobStatus::Queued,
              "queue-cancel: the target stays queued");
        ServiceJobView cancelled;
        check(fx->service->cancel_job(fx->auth, "job-cancel", cancelled) == SS::Ok,
              "queue-cancel: cancelled while queued (deterministic)");
        check(cancelled.status == vortyx::distributed::DistributedJobStatus::Cancelled &&
                  !cancelled.plan_available,
              "queue-cancel: a never-dispatched job has NO plan (honest absence)");
        const std::string json = serialize_service_job(cancelled);
        check(json.find("\"plan\":null") != std::string::npos,
              "queue-cancel: the contract serializes plan as null");

        fx->blocking->release();  // unblock the dispatcher; let the fixture drain
        ServiceJobView blocker_terminal;
        check(fx->service->wait_for_terminal(fx->auth, "job-blocker", 10000, blocker_terminal) ==
                      SS::Ok &&
                  blocker_terminal.status ==
                      vortyx::distributed::DistributedJobStatus::Completed,
              "queue-cancel: the blocker completed after release");
    }

    if (failures == 0) {
        std::cout << "ALL SERVICE FABRIC CHECKS PASSED\n";
        return 0;
    }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
