# Tensor Layer Architecture (Phase 13)

## What Phase 13 IS

`vortyx::tensor` (`src/tensor/`) is a new static library layered **on top of** the
Phase 12 distributed system:

```
vortyx_tensor → vortyx_distributed → vortyx_platform → vortyx_core
```

The build graph enforces the direction: nothing below the tensor layer includes
anything above it. The layer is additive — `VORTYX_ENABLE_TENSOR=OFF` (or
`PLATFORM=OFF`, which disables the whole upper stack) reproduces the Phase 12
build exactly.

## Components

| Module | Responsibility |
|--------|----------------|
| `status.*` | `TensorStatus` — the tensor result vocabulary with stable snake_case codes (`invalid_shape`, `unsupported_dtype`, `transfer_unsupported`, ...) and a documented bidirectional mapping to `vortyx::compute::Status` (the local execution model stays untouched) |
| `dtype.*` | `DataType`: `fp32`, `fp16`, `bf16`, `int32`, `int8` — a closed, named vocabulary. Byte widths and numeric classes are total functions. fp64 is deliberately absent (no implementation exists; adding it later is an additive enum extension) |
| `shape.*` | `TensorShape` (dynamic rank, explicit `kMaxTensorRank = 8` guard), canonical row-major strides, `TensorLayout` (`row_major_contiguous` / `strided` with stride validation against the storage span), checked linear indexing, NumPy-style broadcasting (`broadcast_shapes`, `broadcast_strides`) |
| `placement.*` | `TensorPlacement`: `Host` or `Device(<DeviceId>, <backend>)`. The device id is the Phase 11 `platform::DeviceId` — one identity system, no duplicates. Non-canonical backend names are refused |
| `storage.*` | `TensorStorage`: allocation happens EXCLUSIVELY through the Phase 4 `ResourceManager` → `IBufferProvider` path. Every tensor byte is a tracked Phase 4 buffer resource (stats, the 1 GiB cap, overflow validation). Transfers are whole-storage, explicit, synchronous |
| `tensor_value.*` | `Tensor`: metadata (shape/dtype/layout/placement) + a shared storage reference. Views (`reshape`, `transpose`, `broadcast_to`) share storage READ-ONLY (Phase 13 exposes no in-place mutation through views); `read_host`/`write_host` are storage-level transfers |
| `op.*` | `TensorOp` (14 ops) + `validate_op` — THE one rule set: arity, per-op shape rules, dtype compatibility and output shape inference. The executor, the graph planner and the tests all use the same function; the rules cannot drift |
| `capability.*` | `TensorCapabilities` (supported ops/dtypes, limits, preferred tile, `matrix_acceleration` — `not_claimed` everywhere by construction) and `TensorRequirements` with the pure `satisfied_by` compatibility decision |
| `backend.*` | `ITensorBackend` + the two Phase 13 backends (see below) + deterministic first-fit `select_backend` |
| `executor.*` | `TensorExecutor`: validate → enforce placement rules → allocate the output through the resource system → capability dispatch → materialize strided inputs (explicit, documented copies) → execute |
| `graph.*` | `TensorGraph`: named input slots with declared shape/dtype contracts, two-phase node creation (which is what makes cycles expressible — and detectable), deterministic insertion-order node ids, duplicate-output refusal, `validate_graph` (full inference + cycle detection), `graph_topological_order` (smallest-ready-id-first Kahn) |
| `plan.*` | `make_execution_plan`: validate → capability check per node (op AND inferred dtype) → topological order → memory planning. `MemoryPlannerConfig` is the planner's only tuning surface |
| `graph_executor.*` | Runs a plan: exact binding contracts, slot allocation through the resource system, steps in plan order writing directly into slot storage (no second buffer), per-step traces with real steady-clock measurements |
| `placement_integration.*` | The Phase 12 bridge (see `distributed-integration.md`) — read-only over cluster snapshots |
| `serialize.*` | Tensor METADATA JSON via the Phase 11 strict JSON module. Payload data never serializes |

## The two backends

1. **`CpuReferenceTensorBackend`** (`tensor_reference`) — deterministic
   host-memory reference kernels for the full op set. fp16/bf16 run the
   documented promote-compute-round semantics (exact fp32 math,
   round-to-nearest-even back). MatMul/GEMM accumulate in fp32 with
   per-output-element k-ascending order; the blocked iteration structure does
   not change any accumulation sequence, so results are tile-independent and
   bit-deterministic. Claims `matrix_acceleration = not_claimed`.
2. **`RuntimeElementwiseTensorBackend`** (`tensor_runtime`) — the bridge INTO
   the existing engine: int32 same-shape contiguous add/multiply route through
   `vortyx::compute::Runtime::execute(ComputeTask)`, i.e. the real Phase 3–10
   path (CPU, and the real Vulkan GPU where a device exists). It does not
   claim broadcast (the `ComputeTask` contract requires equal sizes) — a
   broadcast request capability-dispatches to the reference backend.

Dispatch is first-fit over the executor's backend list (external backends →
runtime adapter when provided → reference). No scoring, no silent fallback:
a request no backend lists is a precise refusal.

## What Phase 13 IS NOT (explicit non-goals)

- Hardware tensor kernels: nothing claims Tensor Core/CUDA/ROCm/Metal.
- FP64, autograd/training, quantized kernels (no scale/zero-point vocabulary
  exists — quantization is an explicit future extension).
- Model file formats (ONNX/PyTorch/SafeTensors): a future extension point only.
- Cross-device tensor transfer: two different device placements are refused
  with `transfer_unsupported`; host-placed inputs are readable by any local
  execution (the one universally readable place).
- Distributed graph partitioning: a Phase 13 graph executes on ONE device.
- Performance claims: no number in this layer is a claim; the only timings are
  real steady-clock measurements attached to graph step traces.
