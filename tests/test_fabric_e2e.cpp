// Phase 16 — Adaptive Compute Fabric: end-to-end acceptance tests
// (test_fabric_e2e.cpp).
//
// THE ACCEPTANCE CONTRACT: fabric plans are not decorations — they drive
// the UNCHANGED Phase 12 orchestrator over REAL virtual devices, and the
// execution results are real (bit-exact against the host reference):
//
//   E2E A: 2 devices, different capability claims, one 2-shard job —
//          deterministic plan version 1, real sharded execution, the
//          structured explanation present in the lineage.
//   E2E B: device failure mid-flight — the failed shard is re-planned
//          (version exactly 2), the succeeded shard is NEVER re-run
//          (attempt counts pinned), the job completes with a bit-exact
//          result.
//   E2E C: capability mismatch — the fabric refuses (the orchestrator
//          reports the stable code), no shard dispatches.
//   E2E D: stale cluster — a never-settling revision makes every plan
//          stale; the orchestrator re-plans a bounded number of times and
//          then FAILS the job honestly (stale plans are never forced).
//   E2E E: cancellation — a cancelled job is terminal; the fabric records
//          no new version after terminal (terminal semantics unchanged).
//
// Determinism: the injected FakeClock, condition-variable-free
// synchronous execution, deterministic failure injection — no sleeps
// anywhere.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "distributed/distributed.hpp"
#include "fabric/fabric.hpp"
#include "platform/platform.hpp"

using namespace vortyx;
using namespace vortyx::distributed;
using Op = vortyx::compute::ComputeOp;
using vortyx::platform::AuthContext;
using vortyx::platform::Status;

namespace {

int failures = 0;

void check(bool ok, const std::string& name, const std::string& detail = "") {
    if (ok) {
        std::cout << "PASS: " << name << "\n";
    } else {
        std::cout << "FAIL: " << name;
        if (!detail.empty()) std::cout << "  [" << detail << "]";
        std::cout << "\n";
        ++failures;
    }
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

// A registry whose snapshots always report a DIFFERENT revision than the
// true one — every plan is stale forever (the Phase 12 test pattern).
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

    ClusterSnapshot snapshot() override {
        ClusterSnapshot view = inner_.snapshot();
        view.revision = inner_.revision() + 1000;
        return view;
    }
    std::uint64_t revision() const override { return inner_.revision(); }

private:
    IDeviceRegistry& inner_;
};

// One complete fabric-planned local cluster fixture.
struct FabricCluster {
    std::shared_ptr<FakeClock> clock;
    LocalDeviceRegistry registry;
    LocalInProcessTransport transport;
    std::unique_ptr<LocalMultiDeviceSimulator> simulator;
    std::shared_ptr<vortyx::fabric::FabricPolicy> policy;
    std::unique_ptr<DistributedOrchestrator> orchestrator;
    const UserId owner = "user-a";
    AuthContext auth = vortyx::platform::make_authenticated("user-a");

    FabricCluster() : clock(std::make_shared<FakeClock>(1000)), registry(clock) {}

    static std::unique_ptr<FabricCluster> make(bool skew = false) {
        std::unique_ptr<FabricCluster> cluster(new FabricCluster());
        DistributedConfig config;
        config.enabled = true;
        config.shard_threads = 0;
        DistributedOrchestrator::Deps deps;
        deps.registry = cluster->registry_for(skew);
        deps.transport = &cluster->transport;
        deps.clock = cluster->clock;
        cluster->policy = vortyx::fabric::make_fabric_policy(vortyx::fabric::FabricPlannerConfig{});
        deps.policy_override = cluster->policy;
        std::string error;
        if (DistributedOrchestrator::create(std::move(deps), config, cluster->orchestrator,
                                            error) != Status::Ok) {
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
        config.operations = ops.empty()
                                ? std::vector<Op>{Op::VectorAdd, Op::VectorMultiply,
                                                  Op::VectorScale}
                                : ops;
        bool created = false;
        std::string error;
        return simulator->add_device(config, created, error) == Status::Ok;
    }

private:
    // A raw pointer is not stored — the skew wrapper lives as a member.
    IDeviceRegistry* registry_for(bool skew) {
        if (skew) {
            skewing = std::make_unique<SkewingRegistry>(registry);
            return skewing.get();
        }
        return &registry;
    }
    std::unique_ptr<SkewingRegistry> skewing;
};



}  // namespace

int main() {
    // ---- E2E A: deterministic plan, real sharded execution ----------------
    {
        auto cluster = FabricCluster::make();
        check(cluster != nullptr, "e2e A: cluster constructed");
        // Two devices with DIFFERENT claims: dev-a only vector_scale, so
        // the vector_add workload must land on dev-b (capability honesty
        // through the fabric). Big enough for both shards' memory.
        check(cluster->add_device("dev-a", 8, 4, {Op::VectorScale}),
              "e2e A: dev-a registered (scale-only)");
        check(cluster->add_device("dev-b", 8, 4),
              "e2e A: dev-b registered (full claim)");

        const std::size_t n = 4000;
        const DistributedJobRequest request = make_request("job-e2e-a", n, 2);
        DistributedJobRecord record;
        bool created = false;
        std::string error;
        check(cluster->orchestrator->submit(cluster->auth, request, record, created) ==
                      Status::Ok &&
                  created,
              "e2e A: submission accepted");
        check(verify(record, request.task), "e2e A: real execution, bit-exact result");

        // The plan lineage: exactly version 1 (no re-plans happened).
        const vortyx::fabric::PlanLineage* lineage = cluster->policy->lineage_for("job-e2e-a");
        check(lineage != nullptr && lineage->current_version() == 1,
              "e2e A: plan lineage records exactly version 1");
        const vortyx::fabric::ComputePlan* plan = cluster->policy->last_plan_for("job-e2e-a");
        check(plan != nullptr && plan->planner_name == "adaptive_fabric",
              "e2e A: the plan names its planner");
        check(plan != nullptr && plan->nodes.size() == 1 &&
                  plan->nodes[0].shards.size() == 2,
              "e2e A: the plan carries the 2-shard assignment");
        // The scale-only device was NEVER a candidate.
        bool scale_only_used = false;
        for (const vortyx::fabric::PlanShardAssignment& shard : plan->nodes[0].shards) {
            if (shard.device_id == "dev-a") scale_only_used = true;
        }
        check(!scale_only_used, "e2e A: the capability-mismatched device was never chosen");
        check(plan->nodes[0].decision.score.total ==
                      plan->nodes[0].decision.score.base +
                      plan->nodes[0].decision.score.slack_penalty +
                      plan->nodes[0].decision.score.queue_penalty +
                      plan->nodes[0].decision.score.locality_bonus +
                      plan->nodes[0].decision.score.backend_bonus,
              "e2e A: the explanation's components sum to the recorded score");
        // The explanation's reason summary serializes deterministically.
        const vortyx::fabric::PlanSummary summary = vortyx::fabric::summarize_plan(*plan);
        check(summary.devices.size() == 1 && summary.devices[0] == "dev-b",
              "e2e A: the summary lists exactly the used device");
        check(summary.reason_summary.find("planned 1 workload") != std::string::npos,
              "e2e A: the reason summary is derived from the plan");
    }

    // ---- E2E B: device failure mid-flight -> replan v2, no double run -----
    {
        auto cluster = FabricCluster::make();
        check(cluster->add_device("dev-a", 8, 4), "e2e B: dev-a registered");
        check(cluster->add_device("dev-b", 8, 4), "e2e B: dev-b registered");

        // The deterministic injection: dev-a's first dispatch fails with a
        // device-level failure code. Identical devices tie-break to dev-a,
        // so shard 0 (deterministically placed there) fails; shard 1 runs
        // on dev-b and SUCCEEDS. The retry wave must re-place ONLY shard 0.
        cluster->transport.inject_failure("dev-a", 1, FailureCode::DeviceLost);

        const std::size_t n = 4000;
        const DistributedJobRequest request = make_request("job-e2e-b", n, 2);
        DistributedJobRecord record;
        bool created = false;
        check(cluster->orchestrator->submit(cluster->auth, request, record, created) == Status::Ok,
              "e2e B: submission accepted");
        check(verify(record, request.task),
              "e2e B: the job completed with a bit-exact result after the failure");

        // The lineage: exactly version 2 (the mid-flight re-placement).
        const vortyx::fabric::PlanLineage* lineage = cluster->policy->lineage_for("job-e2e-b");
        check(lineage != nullptr && lineage->current_version() == 2,
              "e2e B: the re-placement is plan version 2");
        check(lineage != nullptr && lineage->entries().size() == 2,
              "e2e B: the history holds both versions");

        // The succeeded shard was NEVER re-run: shard 1's attempt count is
        // exactly 1; the failed shard's is exactly 2 (one failure + one
        // retry, no third attempt, no re-run of shard 1). (The terminal
        // record intentionally clears assigned_device — the devices are
        // verified through the plan lineage below.)
        bool shard0 = false, shard1 = false;
        for (const JobShard& shard : record.shards) {
            if (shard.index == 0) {
                shard0 = shard.attempt == 2 && shard.state == ShardState::Completed;
            }
            if (shard.index == 1) {
                shard1 = shard.attempt == 1 && shard.state == ShardState::Completed;
            }
        }
        check(shard0, "e2e B: the failed shard was retried exactly once");
        check(shard1, "e2e B: the succeeded shard ran exactly once (checkpoint kept)");

        // The re-placement's device: version 2's plan content carries the
        // retried shard's new assignment — NOT the failed device.
        bool retried_elsewhere = false;
        const vortyx::fabric::ComputePlan* v2 = cluster->policy->last_plan_for("job-e2e-b");
        if (v2 != nullptr) {
            for (const vortyx::fabric::PlanShardAssignment& shard : v2->nodes[0].shards) {
                if (shard.shard_index == 0) retried_elsewhere = shard.device_id != "dev-a";
            }
        }
        check(retried_elsewhere,
              "e2e B: version 2 places the failed shard away from the failed device");

        // Version 2's content differs from version 1's (a real new plan).
        if (lineage != nullptr && lineage->entries().size() == 2) {
            check(!(lineage->entries()[0].plan == lineage->entries()[1].plan),
                  "e2e B: the two versions name different assignment sets");
        }
    }

    // ---- E2E C: capability mismatch -> structured refusal, zero dispatch --
    {
        auto cluster = FabricCluster::make();
        check(cluster->add_device("dev-a", 8, 4, {Op::VectorScale}),
              "e2e C: a scale-only device registered");

        const DistributedJobRequest request = make_request("job-e2e-c", 1000, 1);
        DistributedJobRecord record;
        bool created = false;
        const Status status = cluster->orchestrator->submit(cluster->auth, request, record, created);
        // The record is created and honestly FAILED at the placement step
        // (the Phase 12 semantics: submit reports the record; the record's
        // status carries the stable refusal).
        check(status == Status::Ok && created &&
                  record.status == DistributedJobStatus::Failed &&
                  record.error.find("unsupported_capability") != std::string::npos,
              "e2e C: the unplannable workload fails with the stable code");
        check(record.shards.empty(), "e2e C: no shard ever dispatched");
        check(cluster->policy->last_plan_for("job-e2e-c") == nullptr,
              "e2e C: no plan version was published for a refusal");
        check(cluster->policy->counters().plan_rejections >= 1,
              "e2e C: the rejection counter recorded the event");
    }

    // ---- E2E D: a never-settling cluster fails the job honestly -----------
    {
        auto cluster = FabricCluster::make(true /* skew */);
        check(cluster->add_device("dev-a", 8, 4), "e2e D: dev-a registered");

        const DistributedJobRequest request = make_request("job-e2e-d", 1000, 1);
        DistributedJobRecord record;
        bool created = false;
        cluster->orchestrator->submit(cluster->auth, request, record, created);
        check(record.status == DistributedJobStatus::Failed,
              "e2e D: a stale-forever cluster FAILS the job (plans are never forced)");
        check(record.shards.empty(), "e2e D: no shard ever dispatched");
        // Every placement proposal was IDENTICAL content (the skewed view is
        // constant) — the lineage keeps exactly version 1, and nothing
        // executed under it.
        const vortyx::fabric::PlanLineage* lineage = cluster->policy->lineage_for("job-e2e-d");
        check(lineage != nullptr && lineage->current_version() == 1,
              "e2e D: identical stale proposals keep one version; no new versions");
    }

    // ---- E2E E: cancellation stays terminal; the fabric adds nothing ------
    {
        auto cluster = FabricCluster::make();
        check(cluster->add_device("dev-a", 8, 4), "e2e E: dev-a registered");
        const DistributedJobRequest request = make_request("job-e2e-e", 1000, 1);
        DistributedJobRecord record;
        bool created = false;
        check(cluster->orchestrator->submit(cluster->auth, request, record, created) == Status::Ok,
              "e2e E: the job ran to completion");
        check(vortyx::distributed::distributed_job_status_is_terminal(record.status),
              "e2e E: the job is terminal");
        // A terminal job accepts no mutation (the Phase 11/12 rule, still
        // enforced through the fabric-planned path).
        DistributedJobRecord cancelled;
        check(cluster->orchestrator->cancel_job(cluster->auth, "job-e2e-e", cancelled) ==
                  Status::InvalidInput,
              "e2e E: the terminal job refuses cancellation");
        // The lineage did not change after terminal (no new versions).
        const vortyx::fabric::PlanLineage* lineage = cluster->policy->lineage_for("job-e2e-e");
        check(lineage != nullptr && lineage->current_version() == 1,
              "e2e E: no plan version appeared after terminal");
    }

    if (failures == 0) {
        std::cout << "ALL FABRIC E2E CHECKS PASSED\n";
        return 0;
    }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
