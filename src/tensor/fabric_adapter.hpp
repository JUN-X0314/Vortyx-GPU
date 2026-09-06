#pragma once

// TensorGraph -> WorkloadGraph adapter (Phase 16) — the Tensor integration.
//
// WHAT THIS IS: the bridge the Phase 16 architecture review chose — the
// fabric grows NO second graph type and no second op vocabulary; the
// tensor layer converts its validated TensorGraph into a fabric
// WorkloadGraph so the SAME deterministic planner plans both worlds.
//
// THE HONEST MAPPING (the one rule set, reused): a TensorGraph node maps
// to a fabric workload node ONLY where a real executable vocabulary
// equivalent EXISTS — the same rule the Phase 13 runtime adapter already
// established: int32 CONTIGUOUS elementwise Add / Multiply route into the
// real engine as VectorAdd / VectorMultiply (see tensor/backend.hpp).
// The adapter uses exactly that mapping:
//
//   TensorOp::Add      (int32, contiguous)  -> ComputeOp::VectorAdd
//   TensorOp::Multiply (int32, contiguous)  -> ComputeOp::VectorMultiply
//   everything else                          -> REFUSED (named below)
//
// A node whose op/dtype has no executable equivalent is not "planned on a
// maybe" — the adapter REFUSES the whole conversion with the node's name
// and op label (all-or-nothing: a partial mapping would silently drop
// dependencies the fabric cannot see). The refusal is the honest
// statement that the fabric cannot place work no device can claim.
//
// WHAT THE ADAPTER DOES NOT CLAIM: this is graph planning and placement
// abstraction ONLY. No GPU tensor acceleration appears (the reference
// kernels are host implementations — Phase 13's honesty rules stand), no
// cross-device transfer exists (a plan needing one is refused by the
// planner's locality rules, not faked), and execution still flows through
// the existing tensor executor / distributed stack — the adapter creates
// planning metadata, not a new execution path.
//
// ELEMENT COUNTS: each node's data-parallel domain is its validated
// output shape's element count (the same numbers validate_graph inferred
// — no second inference). Overflow is refused, never wrapped.

#include <string>

#include "core/compute/task.hpp"  // ComputeOp (the executable vocabulary)
#include "fabric/graph.hpp"
#include "platform/status.hpp"
#include "tensor/graph.hpp"
#include "tensor/op.hpp"  // TensorOpOutputDesc

namespace vortyx::tensor {

// The executable equivalent of one validated node, or false when none
// exists (the same rule the runtime adapter implements; exposed for tests
// and documentation).
bool fabric_equivalent_operation(const GraphNode& node, const TensorOpOutputDesc& output,
                                 vortyx::compute::ComputeOp& out_op);

// Converts a VALIDATED TensorGraph into a fabric WorkloadGraph. The graph
// must pass validate_graph first (the caller validates; the adapter
// re-checks defensively and refuses an invalid graph — the same double-
// check pattern the dispatch path uses).
//
// The produced WorkloadGraph preserves the TensorGraph's structure:
//   - node ids are the TensorGraph's node ids (insertion order, 1-based —
//     the two id spaces are the same by construction)
//   - dependencies are the node's data inputs (graph-input-consuming
//     nodes have no fabric dependencies; node-output-consuming nodes
//     depend on their producers)
//   - each workload's element_count is the node's inferred output size
//   - workload_id is "t<node_id>" (deterministic, derived from the
//     graph's own id space; uniqueness is the graph's own rule)
//   - owner/priority/shard preference are caller-provided (submission
//     context — a TensorGraph does not carry them)
vortyx::platform::Status tensor_graph_to_workload(
    const TensorGraph& graph, const GraphValidation& validation,
    const vortyx::platform::UserId& owner_user_id, std::uint32_t preferred_shard_count,
    vortyx::fabric::WorkloadGraph& out, std::string& error);

}  // namespace vortyx::tensor
