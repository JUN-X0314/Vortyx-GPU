// Native executor (Phase 15) — implementation.

#include "worker/native_executor.hpp"

#include <string>
#include <vector>

namespace vortyx::worker {

namespace {

// FNV-1a 64 (the project's string-key derivation; deterministic everywhere).
std::uint64_t fnv1a(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : text) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// splitmix64 — a tiny deterministic bit mixer for payload synthesis.
std::uint64_t splitmix64(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// The synthesized operand range: small enough that VectorAdd sums can never
// overflow int32 (the Phase 3 policy) and every op stays bit-exact.
constexpr std::int64_t kOperandSpan = 1000003;  // prime; deterministic spread

std::int32_t bounded_operand(std::uint64_t raw) {
    return static_cast<std::int32_t>(static_cast<std::int64_t>(raw % (2 * kOperandSpan)) -
                                     kOperandSpan);
}

}  // namespace

Status synthesize_task(const JobId& job_id, vortyx::compute::ComputeOp operation,
                       std::uint64_t element_count, vortyx::compute::ComputeTask& out,
                       std::string& error) {
    // Bounds (the same order the rest of the stack enforces): the element
    // count must fit the resource layer's documented 1 GiB per-buffer cap
    // and the contract's int32 element ceiling.
    constexpr std::uint64_t kMaxElements = 2147483647ULL;
    constexpr std::uint64_t kBufferCapElements = 268435456ULL;  // 1 GiB / 4 bytes
    if (element_count == 0 || element_count > kMaxElements) {
        error = "element_count out of range (1..2147483647)";
        return Status::InvalidInput;
    }
    if (element_count > kBufferCapElements) {
        error = "element_count exceeds the resource layer's per-buffer capacity";
        return Status::InvalidInput;
    }
    if (job_id.empty()) {
        error = "job_id is required for deterministic payload synthesis";
        return Status::InvalidInput;
    }

    out.op = operation;
    out.a.clear();
    out.b.clear();
    out.scalar = 0;
    try {
        out.a.resize(static_cast<std::size_t>(element_count));
    } catch (...) {  // allocation refusal is an honest failure, not a crash
        error = "payload allocation failed for element_count " + std::to_string(element_count);
        return Status::Internal;
    }

    std::uint64_t state_a = fnv1a(job_id) ^ 0xA5A5A5A5A5A5A5A5ULL;
    for (std::uint64_t i = 0; i < element_count; ++i) {
        out.a[static_cast<std::size_t>(i)] = bounded_operand(splitmix64(state_a));
    }

    switch (operation) {
        case vortyx::compute::ComputeOp::VectorAdd:
        case vortyx::compute::ComputeOp::VectorMultiply: {
            try {
                out.b.resize(static_cast<std::size_t>(element_count));
            } catch (...) {
                error = "payload allocation failed for element_count " +
                        std::to_string(element_count);
                return Status::Internal;
            }
            std::uint64_t state_b = fnv1a(job_id) ^ 0x5A5A5A5A5A5A5A5AULL;
            for (std::uint64_t i = 0; i < element_count; ++i) {
                out.b[static_cast<std::size_t>(i)] = bounded_operand(splitmix64(state_b));
            }
            break;
        }
        case vortyx::compute::ComputeOp::VectorScale: {
            // The scalar is part of the synthesized payload (execution
            // detail; the control plane carries metadata only). A zero
            // scale would waste the run and is never produced.
            std::uint64_t state_s = fnv1a(job_id) ^ 0x0F0F0F0F0F0F0F0FULL;
            const std::int32_t scalar =
                static_cast<std::int32_t>(splitmix64(state_s) % (2 * kOperandSpan)) -
                static_cast<std::int32_t>(kOperandSpan);
            out.scalar = scalar == 0 ? 1 : scalar;
            break;
        }
    }

    std::string validation;
    if (vortyx::compute::validate_compute_task(out, validation) !=
        vortyx::compute::Status::Ok) {
        error = "synthesized task failed validation: " + validation;
        return Status::Internal;
    }
    error.clear();
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// SimulatorNativeExecutor
// ---------------------------------------------------------------------------

Status SimulatorNativeExecutor::create(const NativeExecutorConfig& config,
                                       std::unique_ptr<INativeExecutor>& out,
                                       std::string& error) {
    if (config.owner_user_id.empty()) {
        error = "owner_user_id is required (the agent's local identity)";
        return Status::InvalidInput;
    }
    if (config.device_count == 0 || config.device_count > 64) {
        error = "device_count must be 1..64";
        return Status::InvalidInput;
    }
    if (config.device_memory_bytes <= 0 || config.device_concurrent_jobs <= 0) {
        error = "device capacity must be positive";
        return Status::InvalidInput;
    }

    std::unique_ptr<SimulatorNativeExecutor> executor(new SimulatorNativeExecutor());
    executor->config_ = config;
    executor->clock_ = std::make_shared<vortyx::distributed::SteadyClock>();
    executor->registry_ =
        std::make_unique<vortyx::distributed::LocalDeviceRegistry>(executor->clock_);
    executor->transport_ = std::make_unique<vortyx::distributed::LocalInProcessTransport>();

    // Keep the simulator alive: the workers own their runtimes, and the
    // transport dispatches into them for the orchestrator's lifetime. The
    // member's declaration order guarantees the simulator dies BEFORE the
    // registry/transport it references (see native_executor.hpp).
    executor->simulator_ = std::make_unique<vortyx::distributed::LocalMultiDeviceSimulator>(
        *executor->registry_, *executor->transport_, config.owner_user_id);
    for (std::uint32_t i = 0; i < config.device_count; ++i) {
        vortyx::distributed::SimulatorDeviceConfig device;
        device.device_id = "vortyx-worker-device-" + std::to_string(i);
        device.display_name = "vortyx native worker (simulated device)";
        device.capacity.compute_units = 0;  // compute units are not fabricated
        device.capacity.memory_bytes = config.device_memory_bytes;
        device.capacity.concurrent_jobs = config.device_concurrent_jobs;
        device.max_concurrent_shards = config.device_concurrent_jobs;
        bool created = false;
        if (executor->simulator_->add_device(device, created, error) != Status::Ok) {
            error = "device '" + device.device_id + "' failed: " + error;
            return Status::InvalidInput;
        }
    }

    vortyx::distributed::DistributedOrchestrator::Deps deps;
    deps.registry = executor->registry_.get();
    deps.transport = executor->transport_.get();
    deps.clock = executor->clock_;
    deps.platform_store = nullptr;  // the control plane is the worker protocol's job

    vortyx::distributed::DistributedConfig distributed;
    distributed.enabled = true;
    if (vortyx::distributed::DistributedOrchestrator::create(std::move(deps), distributed,
                                                             executor->orchestrator_,
                                                             error) != Status::Ok) {
        error = "orchestrator construction failed: " + error;
        return Status::InvalidInput;
    }

    out = std::move(executor);
    return Status::Ok;
}

SimulatorNativeExecutor::~SimulatorNativeExecutor() = default;

Status SimulatorNativeExecutor::execute(vortyx::distributed::DistributedJobRequest& request,
                                        vortyx::distributed::DistributedJobRecord& out) {
    // The AGENT maps the claimed operation label into envelope.operation
    // (the shared enum); the executor validates what it can run.
    std::string error;
    vortyx::compute::ComputeTask task;
    if (synthesize_task(request.envelope.job_id, request.envelope.operation,
                        request.envelope.element_count, task, error) != Status::Ok) {
        error = "claimed job cannot be executed: " + error;
        return Status::InvalidInput;
    }
    request.task = task;
    if (request.requested_shard_count == 0) request.requested_shard_count = 1;

    std::lock_guard<std::mutex> lock(execute_mutex_);
    const vortyx::platform::AuthContext auth =
        vortyx::platform::make_authenticated(config_.owner_user_id);
    bool created = false;
    const Status submit_status = orchestrator_->submit(auth, request, out, created);
    if (submit_status != Status::Ok) {
        // The orchestrator refuses before executing (validation/conflict) —
        // an honest failure the agent reports to the control plane.
        return submit_status;
    }
    // submit() is synchronous: 'out' is terminal here.
    return Status::Ok;
}

Status SimulatorNativeExecutor::request_cancel(const JobId& job_id) {
    if (job_id.empty()) return Status::InvalidInput;
    // The agent owns the records it submits (single owner = the agent's
    // local identity), so the ordinary ownership path delivers the flag.
    // RecordIntent covers the record-creation window without polling.
    const vortyx::platform::AuthContext auth =
        vortyx::platform::make_authenticated(config_.owner_user_id);
    vortyx::distributed::DistributedJobRecord record;
    return orchestrator_->cancel_job(auth, job_id, record,
                                     vortyx::distributed::CancelDelivery::RecordIntent);
}

}  // namespace vortyx::worker
