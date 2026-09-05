# Graph Planning and Memory (Phase 13)

## Planning pipeline

A graph is never executed directly. `make_execution_plan`
(`src/tensor/plan.cpp`) turns a validated `TensorGraph` into a
`GraphExecutionPlan` through four deterministic stages:

1. **Validation** — `validate_graph` (the shared `validate_op` rules plus
   cycle detection via smallest-ready-id-first Kahn). The same graph always
   yields the same order; two plans of the same graph are structurally
   identical (pinned by tests).
2. **Capability check** — every node's op AND its inferred output dtype must
   be covered by the target `TensorCapabilities`; rank/byte limits are checked
   per node. Failures name the node (`unsupported_operation`,
   `unsupported_dtype`, `resource_limit_exceeded`). Planning a graph a device
   cannot run is refused, never guessed into "probably fine".
3. **Ordering** — topological order, smallest-ready-node-id-first.
4. **Memory planning** — liveness-based buffer slots (below).

The plan is a plain value (copyable) — the provider-neutral artifact a future
production scheduler can consume without seeing the graph type.

## Memory planner (correctness first)

Each node output is assigned a SLOT. Reuse rule — a slot may back a new
definition only when ALL of these hold:

- it is **not pinned** (graph outputs outlive the plan; their buffers back the
  returned tensors),
- its **most recent definition's last READ is strictly before** the new
  definition's step (no same-step read/write aliasing — an op that reads a
  slot can never write its output into that same slot),
- its **byte size and dtype match exactly** (no partial overlap, no
  suballocation, no reinterpretation).

Selection is first-fit in ascending slot id (deterministic). The plan reports
`naive_bytes` (all-fresh allocation) vs `planned_bytes` (the plan) honestly.

**Verified by tests**: a three-elementwise-chain graph plans TWO slots (32
bytes) instead of three (48 bytes); the reused-slot execution is bit-identical
to the fresh-slot execution; the graph executor's real allocation count equals
the plan's slot count (measured through the Phase 4 manager's stats).

## Allocation path

Every slot and every tensor is allocated through the Phase 4
`ResourceManager` — the tensor layer has no second allocator. Resource
accounting (`ResourceStats`) therefore sees all tensor memory, the 1 GiB
per-buffer cap applies, and RAII release is the Phase 4 handle semantics.

## Ownership and aliasing

- Views (`reshape`/`transpose`/`broadcast_to`) share storage READ-ONLY.
  Phase 13 exposes no in-place mutation through views, so a view can never
  corrupt a live tensor.
- `read_host`/`write_host` are storage-level whole-storage transfers; on a
  view they address the underlying storage directly (documented).
- Graph outputs hold shared ownership of their (pinned) slot storage — no
  copies, no dangling references.

## Concurrency

No global mutable state exists anywhere in the layer. A `TensorExecutor` or
`GraphExecutor` instance is an explicit context and must be externally
serialized (the same contract as a `Runtime`). Different executors are
independent.
