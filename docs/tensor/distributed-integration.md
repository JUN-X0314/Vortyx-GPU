# Tensor × Distributed Integration (Phase 13)

## The connection (Phase 12 code is NOT modified)

`src/tensor/placement_integration.cpp` reads Phase 12 cluster snapshots
READ-ONLY and reuses the established rules — the ownership filter
(`ClusterSnapshot::candidates_for`), the self-reported `ResourceVector`
capacity and the rejection-code style. Nothing in `src/distributed/` changes.

## TensorDeviceProfile — honest derivation

A device's tensor capabilities are derived from ITS OWN backend claims
(`DeviceMetadata.backends`, the Phase 11 self-description):

| Device claims | Derived tensor surface |
|---------------|------------------------|
| nothing | supports nothing (unknown capability is never guessed into support) |
| `"cpu"` | the reference kernel surface (what Vortyx 0.13's software really executes on host memory) |
| `"vulkan"` | adds the int32 elementwise surface routed through the real engine |
| unknown names (`"cuda"`, ...) | contribute nothing — no fake backends |

The derivation is built from the REAL capability tables of the actual backend
implementations, so the profile cannot drift from what the code executes.
`matrix_acceleration` is `not_claimed` for every combination — no hardware
claim exists anywhere. Memory capacity stays the device's own Phase 12
self-report; nothing is fabricated.

## Placement

`plan_tensor_placement(request, snapshot)` is a PURE function (the Phase 12
policy pattern):

1. Request validation (owner, non-empty requirements, canonical backend name,
   positive device count) → `invalid_request`.
2. Ownership-filtered candidates (reused verbatim): none visible →
   `cluster_empty`; visible but none schedulable → `device_unhealthy`.
3. Capability match (requirements satisfied by the derived profile, plus the
   optional backend request) → `unsupported_capability` when nothing matches.
4. Resource fit against FREE capacity (`capacity − allocated`, the scheduler
   accounting is never bypassed) → `insufficient_resource`.
5. Deterministic first-fit by registration order; the plan carries the
   snapshot revision for stale-plan detection (the caller re-checks the
   registry revision — the Phase 12 orchestrator discipline, unchanged).

Phase 13 executes a graph on ONE device; `requested_device_count > 1` plans
ordered candidate lists as the basis for a future distributed phase — it does
not partition anything.

## What is deliberately NOT here

- No cross-device tensor transfer: executing with inputs on two different
  device placements fails with `transfer_unsupported`. Host-placed inputs are
  readable by any local execution (the one universally readable place).
- No automatic replication of tensor data over the Phase 12 transport.
- No distributed graph partitioning and no remote tensor workers.

## Failure semantics (tested)

- A device that goes Offline after planning: the plan's revision no longer
  matches the registry — a FRESH placement deterministically selects the next
  healthy capable device (or refuses deterministically when none remains).
- Retry/idempotency: re-executing the same operation/plan reproduces the
  identical result bit-exactly (tensor execution is deterministic by
  construction), which is exactly what Phase 12's bounded retry semantics
  require of the work it re-runs.

## Local development

```cpp
#include "tensor/tensor.hpp"

vortyx::distributed::LocalDeviceRegistry registry(clock);
// ... register + activate + heartbeat devices (Phase 12 as usual) ...
const vortyx::distributed::ClusterSnapshot snapshot = registry.snapshot();

vortyx::tensor::TensorPlacementRequest request;
request.owner_user_id = owner;
request.requirements.required_ops = {vortyx::tensor::TensorOp::MatMul};
request.requirements.required_dtypes = {vortyx::tensor::DataType::FP32};
request.estimated_memory_bytes = 4096;

const auto plan = vortyx::tensor::plan_tensor_placement(request, snapshot);
if (plan.accepted) {
    // plan.selected_devices[0] is the deterministic placement target.
    // Bind one TensorExecutor (per device context) and run the graph there.
}
```

The tests in `tests/test_tensor_distributed.cpp` exercise the whole flow on a
real `LocalDeviceRegistry` without any GPU hardware.
