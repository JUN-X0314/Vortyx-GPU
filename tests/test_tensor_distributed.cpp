// Tensor <-> distributed integration tests (Phase 13) — the Phase 12
// connection WITHOUT modifying any Phase 12 code:
//
//   - TensorDeviceProfile derivation from a device's own backend claims
//     (honest, per-backend surfaces);
//   - capability-based tensor placement over a REAL LocalDeviceRegistry +
//     cluster snapshot (two devices with DIFFERENT claims);
//   - deterministic rejection codes (cluster_empty / unsupported_capability /
//     insufficient_resource / device_unhealthy);
//   - the offline-mid-flight scenario: a device that goes Offline before
//     execution is excluded by a FRESH placement decision (the Phase 12
//     stale-plan discipline applied to tensor work);
//   - deterministic/idempotent tensor execution (a retry would recompute the
//     identical result — pinned bit-exact);
//   - transfer honesty: two different device placements are refused, never
//     faked.
//
// Convention: plain main() + check(), like every other test in this project.

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "distributed/registry.hpp"
#include "tensor/tensor.hpp"

using namespace vortyx::tensor;
using namespace vortyx::distributed;
using ST = TensorStatus;
using vortyx::platform::Status;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_status(ST actual, ST expected, const std::string& message) {
    check(actual == expected,
          message + " (expected " + tensor_status_code(expected) + ", got " +
              tensor_status_code(actual) + ")");
}

DeviceCapabilities caps(const std::vector<std::string>& backends, std::int64_t memory_mb,
                        std::int64_t jobs) {
    DeviceCapabilities c;
    c.metadata.protocol_version = vortyx::platform::kProtocolVersion;
    c.metadata.software_version = "0.13.0";
    c.metadata.operating_system = "linux";
    c.metadata.architecture = "x86_64";
    c.metadata.backends = backends;
    c.metadata.operations = {"vector_add", "vector_multiply", "vector_scale"};
    c.metadata.display_name = "tensor-test-device";
    c.capacity.memory_bytes = memory_mb * 1024 * 1024;
    c.capacity.concurrent_jobs = jobs;
    c.max_concurrent_shards = jobs;
    return c;
}

}  // namespace

int main() {
    // The manager MUST live in a shared_ptr (the Phase 4 contract: Buffer
    // handles observe it weakly).
    auto manager = std::make_shared<vortyx::resource::ResourceManager>();
    vortyx::resource::CpuBufferProvider cpu_provider;
    check(manager->register_provider(&cpu_provider), "cpu provider registers");

    // =====================================================================
    // 1. Profile derivation: honest per-backend surfaces
    // =====================================================================
    {
        std::string error;
        const vortyx::distributed::ResourceVector capacity{4, 1 << 20, 2};

        const TensorDeviceProfile empty =
            tensor_profile_for_backends("d0", {}, capacity);
        check(empty.capabilities.supported_ops.empty(),
              "a device claiming nothing supports nothing (never guessed)");

        const TensorDeviceProfile unknown =
            tensor_profile_for_backends("d0", {"cuda"}, capacity);
        check(unknown.capabilities.supported_ops.empty(),
              "unknown backend claims contribute nothing (no fake backends)");

        const TensorDeviceProfile cpu_only =
            tensor_profile_for_backends("d0", {"cpu"}, capacity);
        check(cpu_only.capabilities.supports_op(TensorOp::MatMul),
              "a cpu device runs the reference surface");
        check(cpu_only.capabilities.supports_dtype(DataType::FP16),
              "the reference surface's dtypes are honest");

        const TensorDeviceProfile both =
            tensor_profile_for_backends("d0", {"cpu", "vulkan"}, capacity);
        check(both.capabilities.supports_op(TensorOp::MatMul), "union keeps the reference set");
        check(both.capabilities.supports_op(TensorOp::Add) &&
                  both.capabilities.supports_dtype(DataType::INT32),
              "the int32 elementwise surface is part of the union");
        check(both.capabilities.matrix_acceleration == MatrixAcceleration::NotClaimed,
              "NO hardware acceleration is claimed for any backend claim");
        check(both.capacity.memory_bytes == capacity.memory_bytes,
              "memory capacity stays the device's own self-report (Phase 12)");
        check(both.capabilities.validate(error) == ST::Ok, "derived profiles validate");
    }

    // =====================================================================
    // 2. Placement over a REAL registry: two devices, different claims
    // =====================================================================
    {
        auto clock = std::make_shared<FakeClock>(1000);
        LocalDeviceRegistry registry(clock);
        const UserId owner = "tensor-owner";

        // Device A: cpu only (reference surface). Device B: cpu + vulkan
        // (union surface). Both are self-reported claims — the Phase 12
        // registration vocabulary, unmodified.
        DeviceDescriptor desc_a;
        bool created = false;
        check(registry.register_device("device-a", owner, caps({"cpu"}, 8, 2), desc_a,
                                       created) == Status::Ok && created,
              "device-a registered");
        DeviceDescriptor desc_b;
        check(registry.register_device("device-b", owner, caps({"cpu", "vulkan"}, 8, 2),
                                       desc_b, created) == Status::Ok && created,
              "device-b registered");
        check(registry.update_device_state(owner, "device-a", DeviceState::Ready) == Status::Ok,
              "device-a ready");
        check(registry.update_device_state(owner, "device-b", DeviceState::Ready) == Status::Ok,
              "device-b ready");
        // Activation completes with a heartbeat: only Healthy devices are
        // placement candidates (the Phase 12 rule, reused verbatim).
        check(registry.heartbeat_device(owner, "device-a") == Status::Ok, "device-a heartbeat");
        check(registry.heartbeat_device(owner, "device-b") == Status::Ok, "device-b heartbeat");

        const ClusterSnapshot snapshot = registry.snapshot();

        // A workload needing int32 add/multiply: BOTH devices satisfy it.
        TensorPlacementRequest int_req;
        int_req.owner_user_id = owner;
        int_req.requirements.required_ops = {TensorOp::Add, TensorOp::Multiply};
        int_req.requirements.required_dtypes = {DataType::INT32};
        int_req.estimated_memory_bytes = 1024;
        const TensorPlacementPlan int_plan = plan_tensor_placement(int_req, snapshot);
        check(int_plan.accepted, "int32 workload places");
        check(int_plan.selected_devices.size() == 1 &&
                  int_plan.selected_devices[0] == "device-a",
              "first-fit by registration order (deterministic)");
        check(int_plan.cluster_revision == snapshot.revision, "plan carries the revision");

        // A workload needing FP32 MatMul: only the reference surface has it —
        // both devices claim cpu, so both qualify (honest: the surface is per
        // software backend, not per hardware).
        TensorPlacementRequest matmul_req;
        matmul_req.owner_user_id = owner;
        matmul_req.requirements.required_ops = {TensorOp::MatMul};
        matmul_req.requirements.required_dtypes = {DataType::FP32};
        matmul_req.estimated_memory_bytes = 1024;
        const TensorPlacementPlan matmul_plan =
            plan_tensor_placement(matmul_req, snapshot);
        check(matmul_plan.accepted, "matmul workload places");

        // Backend-restricted request: vulkan only -> only device-b matches.
        TensorPlacementRequest vulkan_req;
        vulkan_req.owner_user_id = owner;
        vulkan_req.requirements.required_ops = {TensorOp::Add};
        vulkan_req.requirements.required_dtypes = {DataType::INT32};
        vulkan_req.requested_backend = "vulkan";
        vulkan_req.estimated_memory_bytes = 1024;
        const TensorPlacementPlan vulkan_plan =
            plan_tensor_placement(vulkan_req, snapshot);
        check(vulkan_plan.accepted && vulkan_plan.selected_devices[0] == "device-b",
              "the backend request restricts candidates to the claiming device");

        // A non-canonical backend name is an INVALID REQUEST (the canonical
        // check fires first — the same order the platform contract uses).
        TensorPlacementRequest impossible_backend;
        impossible_backend.owner_user_id = owner;
        impossible_backend.requirements.required_ops = {TensorOp::Add};
        impossible_backend.requirements.required_dtypes = {DataType::INT32};
        impossible_backend.requested_backend = "cuda";
        const TensorPlacementPlan backend_refused =
            plan_tensor_placement(impossible_backend, snapshot);
        check(!backend_refused.accepted &&
                  backend_refused.rejection == TensorPlacementRejection::InvalidRequest,
              "a non-canonical backend is an invalid request");
        check(std::string(to_string(TensorPlacementRejection::UnsupportedCapability)) ==
                  "unsupported_capability",
              "stable rejection code vocabulary");

        // Insufficient resource: request more memory than the devices have.
        TensorPlacementRequest huge_memory;
        huge_memory.owner_user_id = owner;
        huge_memory.requirements.required_ops = {TensorOp::Add};
        huge_memory.requirements.required_dtypes = {DataType::INT32};
        huge_memory.estimated_memory_bytes = 1LL << 40;  // beyond any 8 MiB device
        const TensorPlacementPlan resource_refused =
            plan_tensor_placement(huge_memory, snapshot);
        check(!resource_refused.accepted &&
                  resource_refused.rejection == TensorPlacementRejection::InsufficientResource,
              "memory fit enforced against the scheduler accounting");

        // Foreign owner: the Phase 12 ownership filter applies unchanged.
        TensorPlacementRequest foreign;
        foreign.owner_user_id = "someone-else";
        foreign.requirements.required_ops = {TensorOp::Add};
        foreign.requirements.required_dtypes = {DataType::INT32};
        foreign.estimated_memory_bytes = 1024;
        const TensorPlacementPlan foreign_plan = plan_tensor_placement(foreign, snapshot);
        check(!foreign_plan.accepted &&
                  foreign_plan.rejection == TensorPlacementRejection::ClusterEmpty,
              "a foreign owner sees no devices (anti-enumeration, reused)");

        // Malformed requests.
        TensorPlacementRequest malformed;
        malformed.owner_user_id = owner;
        malformed.requirements.required_ops = {};
        const TensorPlacementPlan malformed_plan = plan_tensor_placement(malformed, snapshot);
        check(!malformed_plan.accepted &&
                  malformed_plan.rejection == TensorPlacementRejection::InvalidRequest,
              "empty requirements refused");

        // Unhealthy devices: drain BOTH -> device_unhealthy (devices exist,
        // none schedulable).
        check(registry.update_device_state(owner, "device-a", DeviceState::Draining) ==
                  Status::Ok, "device-a draining");
        check(registry.update_device_state(owner, "device-b", DeviceState::Draining) ==
                  Status::Ok, "device-b draining");
        const ClusterSnapshot drained = registry.snapshot();
        const TensorPlacementPlan unhealthy =
            plan_tensor_placement(int_req, drained);
        check(!unhealthy.accepted &&
                  unhealthy.rejection == TensorPlacementRejection::DeviceUnhealthy,
              "drained cluster reports device_unhealthy");
    }

    // =====================================================================
    // 3. The offline-mid-flight scenario: placement excludes the lost device
    // =====================================================================
    {
        auto clock = std::make_shared<FakeClock>(1000);
        LocalDeviceRegistry registry(clock);
        const UserId owner = "failover-owner";
        DeviceDescriptor desc_a;
        bool created = false;
        check(registry.register_device("primary", owner, caps({"cpu"}, 8, 2), desc_a,
                                       created) == Status::Ok, "primary registered");
        DeviceDescriptor desc_b;
        check(registry.register_device("backup", owner, caps({"cpu"}, 8, 2), desc_b,
                                       created) == Status::Ok, "backup registered");
        check(registry.update_device_state(owner, "primary", DeviceState::Ready) == Status::Ok,
              "primary ready");
        check(registry.update_device_state(owner, "backup", DeviceState::Ready) == Status::Ok,
              "backup ready");
        check(registry.heartbeat_device(owner, "primary") == Status::Ok, "primary heartbeat");
        check(registry.heartbeat_device(owner, "backup") == Status::Ok, "backup heartbeat");

        TensorPlacementRequest request;
        request.owner_user_id = owner;
        request.requirements.required_ops = {TensorOp::MatMul};
        request.requirements.required_dtypes = {DataType::FP32};
        request.estimated_memory_bytes = 4096;

        const TensorPlacementPlan before = plan_tensor_placement(request, registry.snapshot());
        check(before.accepted && before.selected_devices[0] == "primary",
              "the first plan selects the primary");

        // The primary goes Offline BEFORE execution (the mid-flight failure).
        check(registry.update_device_state(owner, "primary", DeviceState::Offline) ==
                  Status::Ok, "primary offline");

        // The OLD plan is now stale (its revision differs from the registry).
        const ClusterSnapshot fresh = registry.snapshot();
        check(before.cluster_revision != fresh.revision,
              "the cluster moved: the old plan's revision no longer matches");
        const TensorPlacementPlan after = plan_tensor_placement(request, fresh);
        check(after.accepted && after.selected_devices[0] == "backup",
              "a FRESH placement deterministically selects the healthy backup");

        // Both offline: the deterministic refusal.
        check(registry.update_device_state(owner, "backup", DeviceState::Offline) == Status::Ok,
              "backup offline");
        const TensorPlacementPlan none = plan_tensor_placement(request, registry.snapshot());
        check(!none.accepted &&
                  none.rejection == TensorPlacementRejection::DeviceUnhealthy,
              "no healthy device left: deterministic refusal");

        // A canonical backend NO device claims: unsupported_capability.
        check(registry.update_device_state(owner, "primary", DeviceState::Ready) == Status::Ok,
              "primary back online");
        check(registry.heartbeat_device(owner, "primary") == Status::Ok, "primary heartbeat");
        TensorPlacementRequest vulkan_request;
        vulkan_request.owner_user_id = owner;
        vulkan_request.requirements.required_ops = {TensorOp::Add};
        vulkan_request.requirements.required_dtypes = {DataType::INT32};
        vulkan_request.requested_backend = "vulkan";
        vulkan_request.estimated_memory_bytes = 1024;
        const TensorPlacementPlan capability_refused =
            plan_tensor_placement(vulkan_request, registry.snapshot());
        check(!capability_refused.accepted &&
                  capability_refused.rejection ==
                      TensorPlacementRejection::UnsupportedCapability,
              "a canonical backend no device claims is a capability rejection");
    }

    // =====================================================================
    // 4. Deterministic/idempotent tensor execution on the placed device
    // =====================================================================
    {
        // The placement selected "backup"-like devices; the tensor executor
        // bound to this process runs the work and a RETRY (re-execution of
        // the same plan) must reproduce the identical result bit-exact.
        TensorExecutor::Deps deps;
        deps.resources = manager.get();
        std::unique_ptr<TensorExecutor> executor;
        std::string error;
        check(TensorExecutor::create(deps, executor, error) == ST::Ok, "executor");

        const float a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        const float b[4] = {5.0f, 6.0f, 7.0f, 8.0f};
        Tensor ta;
        Tensor tb;
        check(Tensor::from_host(*manager, TensorShape::make({2, 2}), DataType::FP32, a,
                                sizeof(a), ta, error) == ST::Ok, "A");
        check(Tensor::from_host(*manager, TensorShape::make({2, 2}), DataType::FP32, b,
                                sizeof(b), tb, error) == ST::Ok, "B");

        Tensor first;
        Tensor second;
        check(executor->execute_op(TensorOp::MatMul, TensorOpParams{}, {ta, tb}, first, error) ==
                  ST::Ok, "attempt 1");
        check(executor->execute_op(TensorOp::MatMul, TensorOpParams{}, {ta, tb}, second,
                                   error) == ST::Ok, "attempt 2 (a retry)");
        std::vector<float> first_bytes(4);
        std::vector<float> second_bytes(4);
        check(first.read_host(first_bytes.data(), 16, error) == ST::Ok, "read attempt 1");
        check(second.read_host(second_bytes.data(), 16, error) == ST::Ok, "read attempt 2");
        check(std::memcmp(first_bytes.data(), second_bytes.data(), 16) == 0,
              "retry determinism: identical bytes (idempotent re-execution)");
        check(first_bytes[0] == 19.0f && first_bytes[3] == 50.0f, "values correct");
    }

    // =====================================================================
    // 5. Transfer honesty: cross-device placements are refused
    // =====================================================================
    {
        TensorExecutor::Deps deps;
        deps.resources = manager.get();
        std::unique_ptr<TensorExecutor> executor;
        std::string error;
        check(TensorExecutor::create(deps, executor, error) == ST::Ok, "executor");

        Tensor host_a;
        Tensor device_b;
        const std::int32_t data[4] = {1, 2, 3, 4};
        check(Tensor::from_host(*manager, TensorShape::make({4}), DataType::INT32, data,
                                sizeof(data), host_a, error) == ST::Ok, "host tensor");
        check(Tensor::create(*manager, TensorShape::make({4}), DataType::INT32,
                             TensorPlacement::on_device("device-b", "cpu"), device_b, error) ==
                  ST::Ok,
              "device tensor");

        // Host inputs are readable by ANY local execution (the documented
        // rule), so host + device executes; the output inherits the device
        // placement target.
        Tensor out;
        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {host_a, device_b},
                                          out, error),
                     ST::Ok, "host + device inputs execute (host is universally readable)");
        check(out.placement().device_id == "device-b",
              "the output inherits the device placement target");

        Tensor device_a2;
        check(Tensor::create(*manager, TensorShape::make({4}), DataType::INT32,
                             TensorPlacement::on_device("device-a", "cpu"), device_a2, error) ==
                  ST::Ok,
              "second device tensor");
        Tensor refused2;
        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {device_b, device_a2},
                                          refused2, error),
                     ST::TransferUnsupported,
                     "two different device placements refused");

        // Same-device placements agree (both target device-b).
        Tensor device_b2;
        check(Tensor::create(*manager, TensorShape::make({4}), DataType::INT32,
                             TensorPlacement::on_device("device-b", "cpu"), device_b2, error) ==
                  ST::Ok,
              "same device tensor");
        Tensor same_ok;
        check_status(executor->execute_op(TensorOp::Add, TensorOpParams{}, {device_b, device_b2},
                                          same_ok, error),
                     ST::Ok, "same-device placements execute");
    }

    if (failures == 0) {
        std::cout << "Tensor distributed integration tests passed.\n";
        return 0;
    }
    std::cerr << failures << " tensor distributed integration test(s) failed.\n";
    return 1;
}
