// Distributed worker/transport/simulator tests (Phase 12) — the
// LocalWorker adapter over the existing Runtime, slicing correctness
// (bit-exact against the full-range reference), the lifecycle, the
// loopback transport and the local multi-device simulator.
//
// Runs on EVERY system: the CPU backend is always available, so the tests
// never assume a GPU and never assume its absence.

#include <iostream>
#include <string>
#include <vector>

#include "distributed/distributed.hpp"

using namespace vortyx::distributed;
using Op = vortyx::compute::ComputeOp;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

vortyx::compute::ComputeTask make_task(Op op, std::size_t n) {
    vortyx::compute::ComputeTask task;
    task.op = op;
    task.a.resize(n);
    if (op != Op::VectorScale) task.b.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::int32_t x = static_cast<std::int32_t>((i * 37 + 11) % 5000) - 2500;
        task.a[i] = x;
        if (op != Op::VectorScale) task.b[i] = static_cast<std::int32_t>(i % 97);
    }
    // The strict operand policy: the scalar is an operand of VectorScale
    // ONLY — the two-input ops must keep it 0.
    task.scalar = op == Op::VectorScale ? 3 : 0;
    return task;
}

ShardExecution make_execution(const std::string& shard_id, const DeviceId& device,
                              const vortyx::compute::ComputeTask& task, std::uint64_t begin,
                              std::uint64_t end, const std::string& backend = "") {
    ShardExecution execution;
    execution.shard_id = shard_id;
    execution.parent_job_id = "job";
    execution.shard_index = 0;
    execution.attempt = 1;
    execution.device_id = device;
    execution.work.kind = PartitionKind::ElementRange;
    execution.work.element_range.begin = begin;
    execution.work.element_range.end = end;
    execution.task = task;
    execution.backend = backend;
    return execution;
}

}  // namespace

int main() {
    // =====================================================================
    // 1. LocalWorker lifecycle
    // =====================================================================
    {
        LocalWorker worker("dev-1", {Op::VectorAdd, Op::VectorMultiply, Op::VectorScale});
        check(worker.device_id() == "dev-1", "the worker is bound to its device");
        check(worker.state() == WorkerState::Starting, "a fresh worker is Starting");

        // Execution before start is an honest refusal, not a crash.
        vortyx::compute::ComputeTask task = make_task(Op::VectorAdd, 16);
        const ShardResult early =
            worker.execute_shard(make_execution("job-s0", "dev-1", task, 0, 16));
        check(!early.completed && early.failure_code == FailureCode::WorkerExecutionFailed,
              "an unstarted worker refuses execution with the real reason");

        std::string error;
        check(worker.start(error) == vortyx::compute::Status::Ok, "start succeeds");
        check(worker.state() == WorkerState::Ready, "started -> Ready");
        check(worker.start(error) == vortyx::compute::Status::Ok, "start is idempotent");

        check(worker.drain() == vortyx::compute::Status::Ok, "drain succeeds");
        check(worker.state() == WorkerState::Draining, "draining");
        const ShardResult drained =
            worker.execute_shard(make_execution("job-s0", "dev-1", task, 0, 16));
        check(!drained.completed && drained.error.find("draining") != std::string::npos,
              "a draining worker accepts no new assignments");

        worker.stop();
        check(worker.state() == WorkerState::Stopped, "stop is terminal");
        worker.stop();  // double stop is safe
        const ShardResult stopped =
            worker.execute_shard(make_execution("job-s0", "dev-1", task, 0, 16));
        check(!stopped.completed, "a stopped worker refuses execution");
    }

    // =====================================================================
    // 2. Slicing correctness: reassembly is BIT-EXACT against the full run
    // =====================================================================
    {
        const std::size_t n = 100003;  // deliberately not a multiple of 4
        const std::uint32_t kShards = 4;

        for (const Op op : {Op::VectorAdd, Op::VectorMultiply, Op::VectorScale}) {
            const vortyx::compute::ComputeTask task = make_task(op, n);

            // The full-range reference through the worker itself (whole
            // domain, one shard).
            LocalWorker reference("dev-ref", {op});
            std::string error;
            reference.start(error);
            const ShardResult whole =
                reference.execute_shard(make_execution("ref", "dev-ref", task, 0, n));
            check(whole.completed, "the full-range reference executes");

            // Sliced execution across 4 ranges.
            std::vector<ElementRange> ranges;
            partition_element_count(n, kShards, ranges, error);
            LocalWorker worker("dev-slice", {op});
            worker.start(error);
            std::vector<std::int32_t> reassembled(n, 0);
            for (std::size_t s = 0; s < ranges.size(); ++s) {
                const ShardResult slice =
                    worker.execute_shard(make_execution("job-s" + std::to_string(s), "dev-slice",
                                                        task, ranges[s].begin, ranges[s].end));
                check(slice.completed, "the slice executes");
                check(slice.data.size() == ranges[s].size(), "the slice output size matches");
                check(slice.element_begin == ranges[s].begin,
                      "the result records where the slice lives");
                for (std::size_t i = 0; i < slice.data.size(); ++i) {
                    reassembled[static_cast<std::size_t>(ranges[s].begin) + i] = slice.data[i];
                }
            }
            check(reassembled == whole.data,
                  "slice + reassembly is BIT-EXACT against the full run (op " +
                      std::string(vortyx::compute::workload_label(op)) + ")");
        }
    }

    // =====================================================================
    // 3. Assignment validation: every refusal carries its real reason
    // =====================================================================
    {
        LocalWorker worker("dev-1", {Op::VectorAdd});
        std::string error;
        worker.start(error);
        const vortyx::compute::ComputeTask task = make_task(Op::VectorAdd, 100);

        // Wrong device.
        ShardResult wrong = worker.execute_shard(make_execution("s", "other-device", task, 0, 10));
        check(!wrong.completed && wrong.failure_code == FailureCode::InvalidAssignment &&
                  wrong.error.find("serves") != std::string::npos,
              "an assignment for another device is refused");

        // Empty range.
        ShardResult empty = worker.execute_shard(make_execution("s", "dev-1", task, 5, 5));
        check(!empty.completed && empty.error.find("empty") != std::string::npos,
              "an empty range is refused");

        // Out-of-domain range.
        ShardResult over = worker.execute_shard(make_execution("s", "dev-1", task, 50, 200));
        check(!over.completed && over.error.find("exceeds") != std::string::npos,
              "a range beyond the domain is refused");

        // Operation outside the claim.
        const vortyx::compute::ComputeTask scale = make_task(Op::VectorScale, 100);
        ShardResult unclaimed = worker.execute_shard(make_execution("s", "dev-1", scale, 0, 10));
        check(!unclaimed.completed &&
                  unclaimed.error.find("claim") != std::string::npos,
              "an operation the device never claimed is refused (no guessed capability)");

        // Explicit unknown backend: refused, never remapped.
        ShardResult unknown_backend =
            worker.execute_shard(make_execution("s", "dev-1", task, 0, 10, "quantum"));
        check(!unknown_backend.completed &&
                  unknown_backend.failure_code == FailureCode::WorkerExecutionFailed &&
                  unknown_backend.error.find("quantum") != std::string::npos,
              "an unknown explicit backend is refused (no silent fallback)");

        // Explicit "cpu" (always honestly available) is honored.
        ShardResult cpu = worker.execute_shard(make_execution("s", "dev-1", task, 0, 10, "cpu"));
        check(cpu.completed && cpu.backend == "cpu", "an available explicit backend is honored");

        // Success reports its identity.
        ShardResult ok = worker.execute_shard(make_execution("job-s3", "dev-1", task, 0, 10));
        check(ok.completed && ok.shard_id == "job-s3" && ok.parent_job_id == "job" &&
                  ok.attempt == 1,
              "the result carries the shard's identity");
    }

    // =====================================================================
    // 4. Loopback transport: dispatch, unknown devices, injection
    // =====================================================================
    {
        LocalInProcessTransport transport;
        LocalWorker worker("dev-1", {Op::VectorAdd, Op::VectorMultiply, Op::VectorScale});
        std::string error;
        worker.start(error);
        check(transport.attach(&worker), "attach succeeds");
        check(!transport.attach(&worker), "a duplicate device attach is refused");

        const vortyx::compute::ComputeTask task = make_task(Op::VectorAdd, 100);
        ShardResult result = transport.submit_shard(make_execution("job-s0", "dev-1", task, 0, 50));
        check(result.completed, "the transport dispatches to the device's worker");

        ShardResult nowhere =
            transport.submit_shard(make_execution("job-s1", "ghost", task, 0, 50));
        check(!nowhere.completed && nowhere.failure_code == FailureCode::DeviceLost,
              "a device with no worker reports device_lost (honest, not silent)");

        check(transport.worker_for("dev-1") == &worker, "worker_for resolves the device");
        check(transport.worker_for("ghost") == nullptr, "worker_for is nullptr for unknowns");

        // Deterministic failure injection (no sleeps, no threads).
        transport.inject_failure("dev-1", 2, FailureCode::DeviceLost);
        check(transport.injected_failures("dev-1") == 2, "injection is observable");
        ShardResult injected = transport.submit_shard(make_execution("s", "dev-1", task, 0, 10));
        check(!injected.completed && injected.failure_code == FailureCode::DeviceLost,
              "the injected failure fires BEFORE the worker runs");
        check(transport.injected_failures("dev-1") == 1, "the counter decrements");
        injected = transport.submit_shard(make_execution("s", "dev-1", task, 0, 10));
        check(!injected.completed, "the second injected failure fires");
        injected = transport.submit_shard(make_execution("s", "dev-1", task, 0, 10));
        check(injected.completed, "the worker runs again after the injection is exhausted");
        check(transport.injected_failures("dev-1") == 0, "injection is spent");

        transport.inject_failure("dev-1", 0, FailureCode::None);
        check(transport.injected_failures("dev-1") == 0, "injection clears with count 0");

        // Cancellation requests are recorded (the loopback seam).
        check(transport.cancel_shard("job-s0"), "cancel request recorded");
        check(transport.cancel_shard("job-s0"), "cancel is idempotent");
    }

    // =====================================================================
    // 5. Local multi-device simulator: honest registration
    // =====================================================================
    {
        auto clock = std::make_shared<FakeClock>(100);
        LocalDeviceRegistry registry(clock);
        LocalInProcessTransport transport;
        LocalMultiDeviceSimulator simulator(registry, transport, "owner");

        SimulatorDeviceConfig config;
        config.device_id = "device-0";
        config.display_name = "sim";
        config.capacity.memory_bytes = 8 * 1024 * 1024;
        config.capacity.concurrent_jobs = 1;
        config.max_concurrent_shards = 1;

        bool created = false;
        std::string error;
        check(simulator.add_device(config, created, error) == vortyx::platform::Status::Ok &&
                  created,
              "the first simulated device registers");
        DeviceDescriptor descriptor;
        check(registry.device("owner", "device-0", descriptor) == vortyx::platform::Status::Ok,
              "the device is in the registry");
        check(descriptor.state == DeviceState::Ready &&
                  descriptor.health == DeviceHealth::Healthy,
              "the device is activated Ready + Healthy");
        check(descriptor.capabilities.metadata.backends.size() >= 1 &&
                  descriptor.capabilities.metadata.backends[0] == "cpu",
              "the backend claim is the runtime's HONEST answer (cpu always first)");
        for (const std::string& backend : descriptor.capabilities.metadata.backends) {
            check(backend == "cpu" || backend == "vulkan",
                  "only canonical backend names are claimed");
        }
        check(descriptor.capabilities.capacity.memory_bytes == 8 * 1024 * 1024,
              "the configured capacity is self-reported verbatim");

        // Duplicate registration with a different payload -> Conflict.
        SimulatorDeviceConfig conflict = config;
        conflict.capacity.memory_bytes = 16 * 1024 * 1024;
        check(simulator.add_device(conflict, created, error) == vortyx::platform::Status::Conflict,
              "a conflicting re-registration is refused (registry rule)");

        // The identical payload is an idempotent replay.
        check(simulator.add_device(config, created, error) == vortyx::platform::Status::Ok &&
                  !created,
              "an identical re-registration is the idempotent replay");

        // Invalid configuration is refused before anything is created.
        SimulatorDeviceConfig bad = config;
        bad.device_id = "device-bad";
        bad.capacity.concurrent_jobs = 4;
        bad.max_concurrent_shards = 1;
        check(simulator.add_device(bad, created, error) == vortyx::platform::Status::InvalidInput,
              "an inconsistent concurrency declaration is refused");

        check(simulator.workers().size() == 1, "one worker serves one device");
    }

    if (failures == 0) {
        std::cout << "Distributed worker tests passed.\n";
        return 0;
    }
    std::cerr << failures << " failure(s)\n";
    return 1;
}
