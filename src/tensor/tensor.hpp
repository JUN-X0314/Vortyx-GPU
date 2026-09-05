#pragma once

// Vortyx Tensor Layer / AI-ML Acceleration Foundation (Phase 13) — umbrella
// header.
//
// Layering (the Phase 12 diagram extended ADDITIVELY — nothing below the
// distributed boundary changed):
//
//   Application / Client
//        ↓
//   Platform / Cloud Control Plane   (vortyx::platform — Phase 11, unchanged)
//        ↓
//   Distributed Orchestrator          (vortyx::distributed — Phase 12,
//        │                             unchanged; the tensor layer reads its
//        │                             snapshots, never its internals)
//        ↓
//   Tensor Layer                      (vortyx::tensor — Phase 13)
//        ├─ Tensor value model        (shape / dtype / layout / placement /
//        │                             storage — storage ONLY through the
//        │                             Phase 4 ResourceManager)
//        ├─ TensorOps                 (matmul, gemm, elementwise, reductions,
//        │                             activations, shape ops — one rule set:
//        │                             validate_op drives execution AND planning)
//        ├─ Backends                  (ITensorBackend: the deterministic CPU
//        │                             reference + the runtime adapter that
//        │                             routes int32 elementwise into the REAL
//        │                             Phase 3-10 engine)
//        ├─ TensorGraph               (DAG, deterministic ids, cycle detection)
//        ├─ Planner                   (validation -> capability check -> order
//        │                             -> memory plan — deterministic, serializable)
//        └─ Placement integration     (capability-based device selection over
//                                      Phase 12 cluster snapshots; Phase 12
//                                      code is NOT modified)
//
// DEPENDENCY RULES (enforced by the build graph):
//   - vortyx_tensor depends on vortyx_distributed (which brings
//     vortyx_platform -> vortyx_core) — the same additive stack pattern
//     Phase 12 established over Phase 11.
//   - The core/platform/distributed layers NEVER include the tensor layer.
//   - With VORTYX_ENABLE_TENSOR=OFF — or PLATFORM=OFF, which disables the
//     whole upper stack — the project builds exactly like Phase 12.
//
// HONEST SCOPE (what Phase 13 IS and IS NOT):
//   IS:    a real tensor abstraction (N-D shapes with checked arithmetic,
//          five explicit dtypes with defined semantics, layouts as data,
//          device placement on the Phase 11/12 identity system), a full
//          validated op surface with a deterministic CPU reference
//          implementation, graph construction/validation/deterministic
//          planning with liveness-safe buffer reuse, capability-based
//          backend dispatch, and the placement basis over Phase 12 devices.
//   IS NOT: hardware GPU tensor kernels (none exist in this repository —
//          nothing claims Tensor Core/CUDA/ROCm), FP64, autograd/training,
//          quantized kernels, model file formats (ONNX/PyTorch/SafeTensors),
//          cross-device tensor transfer, distributed graph partitioning, or
//          any performance claim. Each absence is an explicit, documented
//          non-goal with an extension point — not a TODO disguised as done.

#include "tensor/backend.hpp"
#include "tensor/capability.hpp"
#include "tensor/dtype.hpp"
#include "tensor/executor.hpp"
#include "tensor/graph.hpp"
#include "tensor/graph_executor.hpp"
#include "tensor/op.hpp"
#include "tensor/placement.hpp"
#include "tensor/placement_integration.hpp"
#include "tensor/plan.hpp"
#include "tensor/serialize.hpp"
#include "tensor/shape.hpp"
#include "tensor/status.hpp"
#include "tensor/storage.hpp"
#include "tensor/tensor_value.hpp"
