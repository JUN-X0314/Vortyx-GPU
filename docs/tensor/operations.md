# Tensor Operations (Phase 13)

The op set is explicit and closed (the Phase 10 `ComputeOp` precedent). Every
op's rules live in ONE function — `validate_op` (`src/tensor/op.cpp`) — used by
the executor, the graph planner and the tests.

## Op surface and semantics

| Op | Arity | Dtypes | Semantics (exactly as implemented) |
|----|-------|--------|-------------------------------------|
| `matmul` | 2 | fp32/fp16/bf16 | 2-D×2-D and 3-D batched (B×M×K · B×K×N). K mismatch → `invalid_shape`; rank < 2 → `invalid_shape`. fp32 accumulation, k-ascending per output element |
| `gemm` | 3 | fp32/fp16/bf16 | `alpha·A(M,K)·B(K,N) + beta·C(M,N)`; C must be M×N with the shared dtype |
| `add` | 2 | all five | Elementwise add with NumPy-style right-aligned broadcasting (equal dims, or one side is 1; missing leading dims behave as 1). Integer arithmetic is DEFINED two's-complement modular |
| `subtract` | 2 | all five | Same rules as `add` |
| `multiply` | 2 | all five | Same rules as `add` |
| `divide` | 2 | all five | Same broadcasting. Integer division: C++ truncation semantics; division by zero → `numerical_validation_failure` (a defined domain error). Float division: IEEE semantics (inf/NaN defined and tested) |
| `reduce_sum` | 1 | all five | Sum over ONE axis (`params.reduce_axis`), the axis is dropped. Sequential accumulation in memory order; integer sums accumulate in int64 and store modularly |
| `reduce_mean` | 1 | fp32/fp16/bf16 | Mean over one axis (float-only by definition; integer mean would truncate — refused with `unsupported_dtype`) |
| `relu` | 1 | fp32/fp16/bf16 | `max(0, x)`; NaN propagates (documented and tested) |
| `sigmoid` | 1 | fp32/fp16/bf16 | `1/(1+exp(-x))` |
| `tanh` | 1 | fp32/fp16/bf16 | `std::tanh` |
| `softmax` | 1 | fp32/fp16/bf16 | Last-axis softmax with max-subtraction for numerical stability. Non-finite inputs → `numerical_validation_failure` (the defined domain) |
| `transpose` | 1 | all five | Rank-2 only; materialized contiguous output (the zero-copy form is `Tensor::transpose()` — a read-only view) |
| `reshape` | 1 | all five | Element-count-preserving; requires a row-major contiguous input (`unsupported_layout` otherwise — no hidden copy) |

**Broadcasting statement**: the implemented semantics are NumPy-style
right-aligned broadcasting — verified by tests (same-rank, rank expansion,
singleton stretching, incompatible-dimension refusals). No claim is made
beyond what is tested.

**A separate `broadcast_add` label deliberately does not exist** — `add` covers
it (documented in `op.hpp`).

## fp16 / bf16: promote-compute-round

The reference kernels define fp16/bf16 execution as: convert each stored
element to fp32 (exact), compute in fp32, round the result back with
round-to-nearest-even. This is a real, fully defined deterministic semantics —
it is NOT a claim of native fp16/bf16 hardware compute. Tests verify the
behaviour against an FP32 baseline (`19/22/43/50` matmul case) and exact
binary encodings (`bf16` `1+4=5` → `0x40A0`).

## Error vocabulary

Every failure carries a `TensorStatus` with a stable snake_case code:
`invalid_input`, `invalid_shape`, `invalid_stride`, `dtype_mismatch`,
`unsupported_dtype`, `unsupported_operation`, `unsupported_layout`,
`invalid_placement`, `resource_limit_exceeded`, `memory_allocation_failure`,
`invalid_state`, `device_capability_mismatch`, `transfer_unsupported`,
`execution_failure`, `numerical_validation_failure`, `not_initialized`,
`internal`. Codes are round-trip parseable and pinned by tests, together with
the documented mapping onto `vortyx::compute::Status`.

## Resource limits (explicit, enforced)

| Limit | Value | Where |
|-------|-------|-------|
| `kMaxTensorRank` | 8 | shape validation, capabilities |
| `kMaxTensorBytes` | 1 GiB | per-tensor byte budget (mirrors the Phase 4 per-buffer cap) |
| `kMaxGraphNodes` | 256 | graph construction |
| `kMaxGraphInputs` / `kMaxGraphOutputs` | 16 / 16 | graph construction |

Zero-element tensors are refused everywhere (the project-wide zero-element
rule from Phase 3/4): a shape with any zero dimension never reaches storage.
