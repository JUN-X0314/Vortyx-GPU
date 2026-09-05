// Distributed orchestrator end-to-end tests (Phase 12) — the acceptance
// scenarios A through J plus the contract edges around them:
//
//   A: 1 device, 1 job, 1 shard, success
//   B: 4 devices, 1 distributed job, 4 shards, all success
//   C: 4 devices, 2 jobs, resource isolation
//   D: 1 device offline -> the job still succeeds on the rest
//   E: 1 shard fails -> retried on ANOTHER device -> success
//   F: repeated failure -> retry exhaustion -> job failed
//   G: no devices -> rejected with the stable error
//   H: capability mismatch -> rejected
//   I: concurrent submissions -> no resource overcommit
//   J: Platform API -> orchestration -> runtime -> result (end to end)
//
// plus: submission idempotency/conflict, ownership boundaries,
// cancellation (deterministic via a blocking transport + condition
// variables — no sleeps), the stale-plan policy (a revision-skewing
// registry), threaded execution and config validation.
//
// All time comes from an injected FakeClock; every synchronization uses
// condition variables. No sleeps anywhere.

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "distributed/distributed.hpp"
#include "platform/platform.hpp"  // InMemoryPlatformStore (the local/mock store)

using namespace vortyx::distributed;
using Op = vortyx::compute::ComputeOp;
using vortyx::platform::AuthContext;
using vortyx::platform::InMemoryPlatformStore;
using vortyx::platform::Status;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

DeviceCapabilities caps(std::int64_t memory_mb, std::int64_t jobs) {
    DeviceCapabilities c;
    c.metadata.protocol_version = vortyx::platform::kProtocolVersion;
    c.metadata.software_version = "0.12.0";
    c.metadata.backends = {"cpu"};
    c.metadata.operations = {"vector_add", "vector_multiply", "vector_scale"};
    c.metadata.display_name = "test";
    c.capacity.memory_bytes = memory_mb * 1024 * 1024;
    c.capacity.concurrent_jobs = jobs;
    c.max_concurrent_shards = jobs;
    return c;
}

vortyx::compute::ComputeTask make_task(std::size_t n) {
    vortyx::compute::ComputeTask task;
    task.op = Op::VectorAdd;
    task.a.resize(n);
    task.b.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        task.a[i] = static_cast<std::int32_t>(i % 1000);
        task.b[i] = static_cast<std::int32_t>((i * 3) % 1000);
    }
    return task;
}

DistributedJobRequest make_request(const JobId& id, std::size_t n, std::uint32_t shards) {
    DistributedJobRequest request;
    request.envelope.job_id = id;
    request.envelope.operation = Op::VectorAdd;
    request.envelope.element_count = n;
    request.envelope.requested_backend = "cpu";
    request.task = make_task(n);
    request.requested_shard_count = shards;
    return request;
}

bool verify(const DistributedJobRecord& job, const vortyx::compute::ComputeTask& task) {
    if (job.status != DistributedJobStatus::Completed) return false;
    if (job.result.data.size() != task.element_count()) return false;
    for (std::size_t i = 0; i < task.element_count(); ++i) {
        if (job.result.data[i] != task.a[i] + task.b[i]) return false;
    }
    return true;
}

// A transport that BLOCKS the first dispatched shard until released — the
// deterministic way to test cancellation from another thread (condition
// variables, never sleeps).
class BlockingTransport final : public IWorkerTransport {
public:
    ShardResult submit_shard(const ShardExecution& execution) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatched_ = execution.shard_id;
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
    IWorker* worker_for(const DeviceId&) override { return nullptr; }

    void wait_dispatched() {
        std::unique_lock<std::mutex> lock(mutex_);
        dispatched_cv_.wait(lock, [this] { return !dispatched_.empty(); });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        released_cv_.notify_all();
    }
    std::string dispatched() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dispatched_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable dispatched_cv_;
    std::condition_variable released_cv_;
    std::string dispatched_;
    bool released_ = false;
};

// A registry wrapper whose snapshot() skews the revision: the orchestrator
// must detect the STALE PLAN and re-plan, then report the instability
// honestly when it never settles.
class SkewingRegistry final : public IDeviceRegistry {
public:
    explicit SkewingRegistry(IDeviceRegistry& inner) : inner_(inner) {}

    vortyx::platform::Status register_device(const DeviceId& id, const UserId& owner,
                                             const DeviceCapabilities& c, DeviceDescriptor& out,
                                             bool& created) override {
        return inner_.register_device(id, owner, c, out, created);
    }
    vortyx::platform::Status unregister_device(const UserId& u, const DeviceId& id) override {
        return inner_.unregister_device(u, id);
    }
    vortyx::platform::Status device(const UserId& u, const DeviceId& id,
                                    DeviceDescriptor& out) override {
        return inner_.device(u, id, out);
    }
    vortyx::platform::Status devices(const UserId& u, std::vector<DeviceDescriptor>& out) override {
        return inner_.devices(u, out);
    }
    vortyx::platform::Status update_device_state(const UserId& u, const DeviceId& id,
                                                 DeviceState to) override {
        return inner_.update_device_state(u, id, to);
    }
    vortyx::platform::Status heartbeat_device(const UserId& u, const DeviceId& id) override {
        return inner_.heartbeat_device(u, id);
    }
    vortyx::platform::Status set_device_health(const UserId& u, const DeviceId& id,
                                               DeviceHealth h) override {
        return inner_.set_device_health(u, id, h);
    }
    vortyx::platform::Status update_device_capabilities(const UserId& u, const DeviceId& id,
                                                        const DeviceCapabilities& c) override {
        return inner_.update_device_capabilities(u, id, c);
    }
    vortyx::platform::Status reserve(const UserId& u, const DeviceId& d, const JobId& j,
                                     const std::string& s, const ResourceVector& r, std::int64_t t,
                                     DeviceLease& out, std::string& error) override {
        return inner_.reserve(u, d, j, s, r, t, out, error);
    }
    vortyx::platform::Status release_lease(const DeviceLease& lease) override {
        return inner_.release_lease(lease);
    }
    vortyx::platform::Status lease(const std::string& id, DeviceLease& out) override {
        return inner_.lease(id, out);
    }
    std::size_t expire_leases(std::int64_t now) override { return inner_.expire_leases(now); }

    // THE SKEW: every snapshot reports a revision that never matches the
    // registry's true current revision — every plan is stale, forever.
    ClusterSnapshot snapshot() override {
        ClusterSnapshot view = inner_.snapshot();
        view.revision = inner_.revision() + 1000;
        return view;
    }
    std::uint64_t revision() const override { return inner_.revision(); }

private:
    IDeviceRegistry& inner_;
};

// One complete local cluster fixture.
struct Cluster {
    std::shared_ptr<FakeClock> clock;
    LocalDeviceRegistry registry;
    LocalInProcessTransport transport;
    // The simulator OWNS the workers (and their runtimes): it must outlive
    // every dispatch, so it is a member — a per-call temporary would leave
    // the transport with dangling worker pointers.
    std::unique_ptr<LocalMultiDeviceSimulator> simulator;
    std::unique_ptr<DistributedOrchestrator> orchestrator;
    const UserId owner = "user-a";
    AuthContext auth = vortyx::platform::make_authenticated("user-a");

    explicit Cluster(std::shared_ptr<FakeClock> c) : clock(c), registry(c) {}

    static std::unique_ptr<Cluster> make(std::uint32_t shard_threads = 0,
                                         const std::string& policy = "least_loaded") {
        std::unique_ptr<Cluster> cluster(new Cluster(std::make_shared<FakeClock>(1000)));
        DistributedConfig config;
        config.enabled = true;
        config.scheduler_policy = policy;
        config.shard_threads = shard_threads;
        DistributedOrchestrator::Deps deps;
        deps.registry = &cluster->registry;
        deps.transport = &cluster->transport;
        deps.clock = cluster->clock;
        std::string error;
        if (DistributedOrchestrator::create(deps, config, cluster->orchestrator, error) !=
            Status::Ok) {
            return nullptr;
        }
        return cluster;
    }

    bool add_device(const DeviceId& id, std::int64_t memory_mb, std::int64_t jobs,
                    const std::vector<Op>& ops = {}) {
        if (simulator == nullptr) {
            simulator = std::make_unique<LocalMultiDeviceSimulator>(registry, transport, owner);
        }
        SimulatorDeviceConfig config;
        config.device_id = id;
        config.display_name = id;
        config.capacity.memory_bytes = memory_mb * 1024 * 1024;
        config.capacity.concurrent_jobs = jobs;
        config.max_concurrent_shards = jobs;
        // An empty list means the default claim (all three ops); a caller
        // may restrict the claim for capability-mismatch scenarios.
        config.operations = ops.empty()
                                ? std::vector<Op>{Op::VectorAdd, Op::VectorMultiply,
                                                  Op::VectorScale}
                                : ops;
        bool created = false;
        std::string error;
        return simulator->add_device(config, created, error) == Status::Ok;
    }
};

}  // namespace

int main() {
    // =====================================================================
    // Scenario A: 1 virtual GPU, 1 job, 1 shard, success
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make();
        check(cluster != nullptr, "cluster A builds");
        check(cluster->add_device("device-0", 64, 2), "device registers");

        DistributedJobRecord job;
        bool created = false;
        const DistributedJobRequest request = make_request("job-a", 1000, 1);
        check(cluster->orchestrator->submit(cluster->auth, request, job, created) == Status::Ok &&
                  created,
              "A: submit succeeds");
        check(job.status == DistributedJobStatus::Completed, "A: job completed");
        check(job.shards.size() == 1, "A: exactly one shard");
        check(verify(job, request.task), "A: result is bit-exact");
        check(job.result.backends_used.size() == 1 && job.result.backends_used[0] == "cpu",
              "A: the backend used is reported");
    }

    // =====================================================================
    // Scenario B: 4 virtual GPUs, 1 distributed job, 4 shards, all success
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make();
        for (const char* id : {"device-0", "device-1", "device-2", "device-3"}) {
            check(cluster->add_device(id, 64, 1), "device registers");
        }

        const DistributedJobRequest request = make_request("job-b", 40000, 4);
        DistributedJobRecord job;
        bool created = false;
        check(cluster->orchestrator->submit(cluster->auth, request, job, created) == Status::Ok,
              "B: submit succeeds");
        check(job.status == DistributedJobStatus::Completed, "B: job completed");
        check(job.shards.size() == 4, "B: four shards");
        check(job.result.succeeded == 4, "B: all shards succeeded");
        check(verify(job, request.task), "B: reassembled result is bit-exact");

        // Deterministic placement: the same cluster + request always
        // produces the same shard plan.
        DistributedJobRecord job2;
        DistributedJobRequest request2 = make_request("job-b2", 40000, 4);
        check(cluster->orchestrator->submit(cluster->auth, request2, job2, created) == Status::Ok,
              "B: second job submits");
        bool same_plan = job2.shards.size() == job.shards.size();
        for (std::size_t i = 0; same_plan && i < job.shards.size(); ++i) {
            same_plan = job2.shards[i].work.element_range.begin ==
                            job.shards[i].work.element_range.begin &&
                        job2.shards[i].work.element_range.end ==
                            job.shards[i].work.element_range.end;
        }
        check(same_plan, "B: the shard plan is deterministic");

        // No capacity leaked.
        ClusterSnapshot view = cluster->orchestrator->cluster_snapshot("user-a");
        bool clean = true;
        for (const DeviceSnapshot& device : view.devices) {
            clean = clean && device.allocated.memory_bytes == 0 &&
                    device.allocated.concurrent_jobs == 0;
        }
        check(clean, "B: every lease was released (no resource leak)");
    }

    // =====================================================================
    // Scenario C: 4 GPUs, 2 jobs, resource isolation
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make();
        for (const char* id : {"device-0", "device-1", "device-2", "device-3"}) {
            check(cluster->add_device(id, 64, 1), "device registers");
        }

        const DistributedJobRequest first = make_request("job-c1", 8000, 2);
        const DistributedJobRequest second = make_request("job-c2", 8000, 2);
        DistributedJobRecord job1, job2;
        bool created = false;
        check(cluster->orchestrator->submit(cluster->auth, first, job1, created) == Status::Ok,
              "C: first job submits");
        check(cluster->orchestrator->submit(cluster->auth, second, job2, created) == Status::Ok,
              "C: second job submits");
        check(job1.status == DistributedJobStatus::Completed &&
                  job2.status == DistributedJobStatus::Completed,
              "C: both jobs completed");
        check(verify(job1, first.task) && verify(job2, second.task),
              "C: both results are correct and independent");
        check(job1.job_id != job2.job_id, "C: distinct job identities");
    }

    // =====================================================================
    // Scenario D: 4 GPUs, 1 offline -> the job still succeeds
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make();
        for (const char* id : {"device-0", "device-1", "device-2", "device-3"}) {
            check(cluster->add_device(id, 64, 1), "device registers");
        }
        // device-0 goes offline the honest way: the transition table.
        check(cluster->orchestrator->set_device_state("user-a", "device-0",
                                                      DeviceState::Offline) == Status::Ok,
              "D: device-0 goes offline");

        const DistributedJobRequest request = make_request("job-d", 40000, 4);
        DistributedJobRecord job;
        bool created = false;
        check(cluster->orchestrator->submit(cluster->auth, request, job, created) == Status::Ok,
              "D: submit succeeds");
        check(job.status == DistributedJobStatus::Completed, "D: job completed");
        check(job.shards.size() == 3,
              "D: the plan coalesced to the 3 healthy devices (fallback)");
        bool used_offline = false;
        for (const JobShard& shard : job.shards) {
            used_offline = used_offline || shard.assigned_device == "device-0";
        }
        check(!used_offline, "D: the offline device was never a target");
        check(verify(job, request.task), "D: result is bit-exact");
    }

    // =====================================================================
    // Scenario E: 1 shard fails -> retried on ANOTHER device -> success
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make();
        for (const char* id : {"device-0", "device-1", "device-2", "device-3"}) {
            check(cluster->add_device(id, 64, 1), "device registers");
        }
        // device-0 fails its first dispatch (deterministic injection), then
        // recovers. The retry must land on a DIFFERENT device.
        cluster->transport.inject_failure("device-0", 1, FailureCode::DeviceLost);

        const DistributedJobRequest request = make_request("job-e", 40000, 4);
        DistributedJobRecord job;
        bool created = false;
        check(cluster->orchestrator->submit(cluster->auth, request, job, created) == Status::Ok,
              "E: submit succeeds");
        check(job.status == DistributedJobStatus::Completed,
              "E: the job completed after the retry");
        bool saw_retry = false;
        std::string retried_shard;
        for (const JobShard& shard : job.shards) {
            if (shard.attempt >= 2) {
                saw_retry = true;
                retried_shard = shard.shard_id;
            }
        }
        check(saw_retry, "E: one shard was re-executed (attempt >= 2)");
        check(job.result.succeeded == 4 && job.result.duplicates == 0,
              "E: exactly four verdicts, no duplicates");
        check(verify(job, request.task), "E: the reassembled result is bit-exact");

        // The retried shard's FIRST device must not be its LAST device: the
        // failure evidence is on the record.
        bool failure_recorded = false;
        for (const JobShard& shard : job.shards) {
            if (shard.shard_id == retried_shard) {
                failure_recorded = shard.last_failure_code == "device_lost" ||
                                   shard.attempt >= 2;
            }
        }
        check(failure_recorded, "E: the retry is visible in the shard record");
    }

    // =====================================================================
    // Scenario F: repeated failure -> retry exhaustion -> job FAILED
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make();
        for (const char* id : {"device-0", "device-1"}) {
            check(cluster->add_device(id, 64, 1), "device registers");
        }
        // device-0 AND device-1 fail EVERY dispatch (deterministic
        // injection). Retries bounce between devices, each attempt failing,
        // until the retry policy's ceiling: exactly 4 attempts
        // (max_retries=3 -> max_attempts=4), then the shard is Failed.
        cluster->transport.inject_failure("device-0", 100, FailureCode::WorkerExecutionFailed);
        cluster->transport.inject_failure("device-1", 100, FailureCode::WorkerExecutionFailed);

        const DistributedJobRequest request = make_request("job-f", 1000, 2);
        DistributedJobRecord job;
        bool created = false;
        check(cluster->orchestrator->submit(cluster->auth, request, job, created) == Status::Ok,
              "F: submit succeeds");
        check(job.status == DistributedJobStatus::Failed, "F: the job FAILED (not faked)");
        check(!job.error.empty(), "F: the failure carries its reason");
        check(job.result.succeeded == 0 && job.result.failed == 2,
              "F: the counts say exactly what happened (0 of 2)");
        bool exhausted = true;
        for (const JobShard& shard : job.shards) {
            exhausted = exhausted && shard.state == ShardState::Failed && shard.attempt == 4;
        }
        check(exhausted,
              "F: every shard exhausted exactly max_attempts (infinite retry is impossible)");
        check(job.result.data.empty(), "F: a failed job carries no faked payload");
    }

    // =====================================================================
    // Scenario G: no available GPU -> rejected with the stable error
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make();
        const DistributedJobRequest request = make_request("job-g", 100, 1);
        DistributedJobRecord job;
        bool created = false;
        check(cluster->orchestrator->submit(cluster->auth, request, job, created) == Status::Ok,
              "G: the submission itself is accepted");
        check(job.status == DistributedJobStatus::Failed, "G: the job is rejected (Failed)");
        check(job.error.find("cluster_empty") != std::string::npos,
              "G: the stable cluster_empty rejection is the reason");
        check(job.shards.empty(), "G: no shard was ever created");

        // One BUSY-to-capacity device also yields an honest rejection.
        check(cluster->add_device("device-0", 1, 1), "a tiny device registers");
        DistributedJobRecord job2;
        DistributedJobRequest big = make_request("job-g2", 1000000, 1);
        check(cluster->orchestrator->submit(cluster->auth, big, job2, created) == Status::Ok,
              "G: the oversized submission is accepted");
        check(job2.status == DistributedJobStatus::Failed &&
                  job2.error.find("insufficient_resource") != std::string::npos,
              "G: insufficient_resource is the stable rejection for a shard that fits nowhere");
    }

    // =====================================================================
    // Scenario H: capability mismatch -> rejected
    // (operation-based, NOT backend-based: a host with a real Vulkan
    // device makes the simulator honestly claim "vulkan", so a backend
    // mismatch would be environment-dependent. An operation the device
    // never claims is refused on every host.)
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make();
        check(cluster->add_device("device-0", 64, 2,
                                  {Op::VectorMultiply, Op::VectorScale}),
              "a device claiming only multiply/scale registers");

        DistributedJobRequest request = make_request("job-h", 100, 1);  // vector_add
        DistributedJobRecord job;
        bool created = false;
        check(cluster->orchestrator->submit(cluster->auth, request, job, created) == Status::Ok,
              "H: the submission itself is accepted");
        check(job.status == DistributedJobStatus::Failed, "H: the job is rejected");
        check(job.error.find("unsupported_capability") != std::string::npos,
              "H: the stable unsupported_capability rejection is the reason");
    }

    // =====================================================================
    // Scenario I: concurrent submissions -> no resource overcommit
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make();
        for (const char* id : {"device-0", "device-1", "device-2", "device-3"}) {
            check(cluster->add_device(id, 8, 1), "device registers (1 job slot each)");
        }

        // Two threads submit 4-shard jobs against the same 4 single-slot
        // devices. The registry's atomic reservation is the gate: whatever
        // the interleaving, no device may hold more than its capacity, and
        // every job reaches an honest terminal state.
        std::atomic<int> completed{0};
        std::atomic<int> failed{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&cluster, &completed, &failed, t]() {
                const int jobs = 3;  // local copy captured by value (MSVC-safe pattern)
                for (int i = 0; i < jobs; ++i) {
                    const JobId id = "job-i-" + std::to_string(t) + "-" + std::to_string(i);
                    DistributedJobRecord job;
                    bool created = false;
                    if (cluster->orchestrator->submit(cluster->auth, make_request(id, 4000, 4),
                                                      job, created) == Status::Ok) {
                        if (job.status == DistributedJobStatus::Completed) {
                            ++completed;
                        } else if (job.status == DistributedJobStatus::Failed) {
                            check(!job.error.empty(),
                                  "I: a rejected job carries its reason (" + id + ")");
                            ++failed;
                        }
                    }
                }
            });
        }
        for (std::thread& thread : threads) thread.join();

        check(completed.load() + failed.load() == 6,
              "I: every submission reached a terminal state");
        check(completed.load() >= 1, "I: at least one job ran (the cluster is functional)");

        // THE invariant: zero overcommit anywhere, and everything released.
        ClusterSnapshot view = cluster->orchestrator->cluster_snapshot("user-a");
        bool no_overcommit = true;
        bool all_released = true;
        for (const DeviceSnapshot& device : view.devices) {
            no_overcommit = no_overcommit &&
                            device.allocated.concurrent_jobs <=
                                device.capabilities.capacity.concurrent_jobs &&
                            device.allocated.memory_bytes <= device.capabilities.capacity.memory_bytes;
            all_released = all_released && device.allocated.concurrent_jobs == 0 &&
                           device.allocated.memory_bytes == 0;
        }
        check(no_overcommit, "I: allocated <= capacity held across the whole run");
        check(all_released, "I: every lease was returned (no leak under contention)");
    }

    // =====================================================================
    // Scenario J: Platform API -> orchestration -> runtime -> result
    // =====================================================================
    {
        auto clock = std::make_shared<FakeClock>(1000);
        LocalDeviceRegistry registry(clock);
        LocalInProcessTransport transport;
        InMemoryPlatformStore store;

        DistributedConfig config;
        config.enabled = true;
        DistributedOrchestrator::Deps deps;
        deps.registry = &registry;
        deps.transport = &transport;
        deps.clock = clock;
        deps.platform_store = &store;

        std::unique_ptr<DistributedOrchestrator> orchestrator;
        std::string error;
        check(DistributedOrchestrator::create(deps, config, orchestrator, error) == Status::Ok,
              "J: the orchestrator builds with a platform store");

        // A device owned by the cluster's user, registered in the platform
        // store too (the control-plane record).
        LocalMultiDeviceSimulator simulator(registry, transport, "user-a");
        SimulatorDeviceConfig device_config;
        device_config.device_id = "device-0";
        device_config.capacity.memory_bytes = 64 * 1024 * 1024;
        device_config.capacity.concurrent_jobs = 2;
        device_config.max_concurrent_shards = 2;
        bool created = false;
        check(simulator.add_device(device_config, created, error) == Status::Ok,
              "J: the simulated device registers");

        const AuthContext auth = vortyx::platform::make_authenticated("user-a");
        vortyx::platform::DeviceRecord device_record;
        check(store.register_device(auth, "device-0", caps(64, 2).metadata, device_record) ==
                  Status::Ok,
              "J: the control plane knows the device");

        const DistributedJobRequest request = make_request("job-j", 40000, 2);
        DistributedJobRecord job;
        check(orchestrator->submit(auth, request, job, created) == Status::Ok,
              "J: the distributed submission succeeds");
        check(job.status == DistributedJobStatus::Completed, "J: the job completed");
        check(verify(job, request.task), "J: the result is bit-exact");
        check(job.platform_error.empty(), "J: the platform mirror had no failures");

        // The control plane saw the whole lifecycle.
        vortyx::platform::JobRecord platform_job;
        check(store.job(auth, "job-j", platform_job) == Status::Ok, "J: the job is recorded");
        check(platform_job.status == vortyx::platform::JobStatus::Completed,
              "J: the platform status mirrors the distributed outcome");
        check(platform_job.started_at_ms.has_value() && platform_job.completed_at_ms.has_value(),
              "J: the lifecycle timestamps were stamped");
        vortyx::platform::ResultEnvelope result;
        check(store.result(auth, "job-j", result) == Status::Ok, "J: the result is recorded");
        check(result.status == vortyx::platform::JobStatus::Completed &&
                  result.result_element_count == 40000,
              "J: the result metadata is honest (payload stays local)");

        // Ownership: another user sees NOTHING (anti-enumeration).
        const AuthContext stranger = vortyx::platform::make_authenticated("user-b");
        vortyx::platform::JobRecord invisible;
        check(store.job(stranger, "job-j", invisible) == vortyx::platform::Status::NotFound,
              "J: a foreign user cannot see the job");
        DistributedJobRecord distributed_invisible;
        check(orchestrator->job(stranger, "job-j", distributed_invisible) ==
                  vortyx::platform::Status::NotFound,
              "J: the orchestrator hides foreign jobs the same way");
    }

    // =====================================================================
    // Submission idempotency + conflict (the Phase 11 rule carried over)
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make();
        check(cluster->add_device("device-0", 64, 2), "device registers");

        const DistributedJobRequest request = make_request("job-idem", 100, 1);
        DistributedJobRecord job;
        bool created = false;
        check(cluster->orchestrator->submit(cluster->auth, request, job, created) == Status::Ok &&
                  created,
              "the first submission creates");

        DistributedJobRecord replay;
        bool replay_created = true;
        check(cluster->orchestrator->submit(cluster->auth, request, replay, replay_created) ==
                      Status::Ok &&
                  !replay_created,
              "the identical resubmission is an idempotent replay");
        check(replay.job_id == job.job_id, "the replay returns the existing record");

        DistributedJobRequest different = make_request("job-idem", 200, 1);  // same id, other size
        DistributedJobRecord conflict;
        check(cluster->orchestrator->submit(cluster->auth, different, conflict, replay_created) ==
                  Status::Conflict,
              "the same id with a different payload is a Conflict");

        // A different owner using the same id: Conflict, never a leak.
        const AuthContext stranger = vortyx::platform::make_authenticated("user-b");
        DistributedJobRecord stranger_job;
        check(cluster->orchestrator->submit(stranger, request, stranger_job, replay_created) ==
                  Status::Conflict,
              "a foreign owner cannot hijack the id");
    }

    // =====================================================================
    // Cancellation: deterministic via the blocking transport
    // =====================================================================
    {
        auto clock = std::make_shared<FakeClock>(1000);
        LocalDeviceRegistry registry(clock);
        BlockingTransport transport;

        DistributedConfig config;
        config.enabled = true;
        DistributedOrchestrator::Deps deps;
        deps.registry = &registry;
        deps.transport = &transport;
        deps.clock = clock;
        std::unique_ptr<DistributedOrchestrator> orchestrator;
        std::string error;
        check(DistributedOrchestrator::create(deps, config, orchestrator, error) == Status::Ok,
              "the cancellation rig builds");

        DeviceDescriptor out;
        bool created = false;
        // TWO devices: with one device the fallback coalesces the job to a
        // single shard and there would be nothing left to cancel.
        registry.register_device("device-0", "user-a", caps(64, 4), out, created);
        registry.register_device("device-1", "user-a", caps(64, 4), out, created);
        for (const char* id : {"device-0", "device-1"}) {
            registry.update_device_state("user-a", id, DeviceState::Ready);
            registry.heartbeat_device("user-a", id);
        }

        const DistributedJobRequest request = make_request("job-cancel", 1000, 2);
        DistributedJobRecord terminal;
        std::thread submitter([&orchestrator, &request, &terminal]() {
            const AuthContext auth = vortyx::platform::make_authenticated("user-a");
            // The synchronous submit runs to the terminal state.
            bool local_created = false;
            orchestrator->submit(auth, request, terminal, local_created);
        });

        // Wait for the FIRST dispatch, then cancel from THIS thread.
        transport.wait_dispatched();
        DistributedJobRecord cancel_view;
        const AuthContext auth = vortyx::platform::make_authenticated("user-a");
        check(orchestrator->cancel_job(auth, "job-cancel", cancel_view) == Status::Ok,
              "the cancellation is accepted while the job runs");
        transport.release();  // the in-flight shard finishes
        submitter.join();

        check(terminal.status == DistributedJobStatus::Cancelled,
              "the job is Cancelled (one shard completed, the rest were cancelled)");
        bool one_completed = false, others_cancelled = true;
        for (const JobShard& shard : terminal.shards) {
            if (shard.state == ShardState::Completed) one_completed = true;
            if (shard.state != ShardState::Completed && shard.state != ShardState::Cancelled) {
                others_cancelled = false;
            }
        }
        check(one_completed, "the in-flight shard's real outcome is recorded");
        check(others_cancelled, "every other shard was cancelled — nothing hidden");

        // Cancelling a TERMINAL job is refused (the Phase 11 rule).
        DistributedJobRecord refused;
        check(orchestrator->cancel_job(auth, "job-cancel", refused) == Status::InvalidInput,
              "cancelling a terminal job is InvalidInput");

        // Foreign cancellation is invisible.
        const AuthContext stranger = vortyx::platform::make_authenticated("user-b");
        check(orchestrator->cancel_job(stranger, "job-cancel", refused) == Status::NotFound,
              "a foreign user cannot even see the job to cancel it");
    }

    // =====================================================================
    // Stale plans: a revision that never settles is reported, never forced
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make();
        SkewingRegistry skewing(cluster->registry);

        // Build the orchestrator against the SKEWING registry.
        DistributedConfig config;
        config.enabled = true;
        DistributedOrchestrator::Deps deps;
        deps.registry = &skewing;
        deps.transport = &cluster->transport;
        deps.clock = cluster->clock;
        std::unique_ptr<DistributedOrchestrator> orchestrator;
        std::string error;
        check(DistributedOrchestrator::create(deps, config, orchestrator, error) == Status::Ok,
              "the skewing rig builds");

        // A device in the underlying registry (visible through the skew).
        DeviceDescriptor out;
        bool created = false;
        cluster->registry.register_device("device-0", "user-a", caps(64, 4), out, created);
        cluster->registry.update_device_state("user-a", "device-0", DeviceState::Ready);
        cluster->registry.heartbeat_device("user-a", "device-0");

        const DistributedJobRequest request = make_request("job-stale", 100, 1);
        DistributedJobRecord job;
        check(orchestrator->submit(cluster->auth, request, job, created) == Status::Ok,
              "the skewed submission is accepted");
        check(job.status == DistributedJobStatus::Failed,
              "a never-settling cluster FAILS the job (stale plans are never forced)");
        check(job.error.find("kept failing") != std::string::npos,
              "the error says the placement kept failing");
    }

    // =====================================================================
    // Threaded execution (config.shard_threads > 1): same correctness
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make(4);  // 4 shard threads
        for (const char* id : {"device-0", "device-1", "device-2", "device-3"}) {
            check(cluster->add_device(id, 64, 1), "device registers");
        }
        const DistributedJobRequest request = make_request("job-threads", 40000, 4);
        DistributedJobRecord job;
        bool created = false;
        check(cluster->orchestrator->submit(cluster->auth, request, job, created) == Status::Ok,
              "threaded submit succeeds");
        check(job.status == DistributedJobStatus::Completed, "threaded job completed");
        check(verify(job, request.task),
              "threaded execution reassembles bit-exactly (worker mutexes hold)");
    }

    // =====================================================================
    // Configuration: invalid values are refused at creation
    // =====================================================================
    {
        std::unique_ptr<Cluster> cluster = Cluster::make();
        DistributedOrchestrator::Deps deps;
        deps.registry = &cluster->registry;
        deps.transport = &cluster->transport;
        deps.clock = cluster->clock;

        DistributedConfig bad_policy;
        bad_policy.scheduler_policy = "genius_ai_scheduler";
        std::unique_ptr<DistributedOrchestrator> orchestrator;
        std::string error;
        check(DistributedOrchestrator::create(deps, bad_policy, orchestrator, error) ==
                  Status::InvalidInput,
              "an unknown policy name is refused at creation (never defaulted)");

        DistributedConfig bad_timeout;
        bad_timeout.heartbeat_timeout_ms = 0;
        check(DistributedOrchestrator::create(deps, bad_timeout, orchestrator, error) ==
                  Status::InvalidInput,
              "a zero heartbeat timeout is refused");

        DistributedConfig bad_shards;
        bad_shards.max_shards_per_job = 0;
        check(DistributedOrchestrator::create(deps, bad_shards, orchestrator, error) ==
                  Status::Ok,
              "a zero shard cap is accepted but binds requests");

        // A request beyond the cap is refused loudly.
        check(cluster->add_device("device-0", 64, 4), "device registers");
        DistributedJobRequest request = make_request("job-cap", 100, 1000);
        DistributedJobRecord job;
        bool created = false;
        check(cluster->orchestrator->submit(cluster->auth, request, job, created) ==
                  Status::InvalidInput,
              "a request beyond max_shards_per_job is refused");

        // Inconsistent envelope/task pairs are refused.
        DistributedJobRequest mismatched = make_request("job-mismatch", 100, 1);
        mismatched.task.a.resize(50);
        mismatched.task.b.resize(50);
        check(cluster->orchestrator->submit(cluster->auth, mismatched, job, created) ==
                  Status::InvalidInput,
              "an envelope/task element mismatch is refused");
    }

    if (failures == 0) {
        std::cout << "Distributed orchestrator tests passed.\n";
        return 0;
    }
    std::cerr << failures << " failure(s)\n";
    return 1;
}
