#pragma once

// vortyx::fabric (Phase 16) — the Adaptive Compute Fabric.
//
// The adaptive planning layer ON TOP of the unchanged Phase 12 distributed
// system: workload metadata (workload.hpp), dependency graphs (graph.hpp),
// the deterministic planning core (planner.hpp + cost.hpp), the plan
// artifact with its structured explanation (plan.hpp), the lineage and
// safe replanning rules (replan.hpp), and the bridge that carries plans
// into the existing execution path (policy_bridge.hpp).
//
// Build shape: a separate static library (vortyx_fabric) linking
// vortyx_distributed — the build graph enforces the direction
// fabric -> distributed -> platform -> core. The fabric never sees the
// service, web or worker layers; the service and the tensor layer's
// adapter see the fabric.

#include "fabric/cost.hpp"
#include "fabric/contract_fabric.hpp"
#include "fabric/graph.hpp"
#include "fabric/plan.hpp"
#include "fabric/planner.hpp"
#include "fabric/policy_bridge.hpp"
#include "fabric/replan.hpp"
#include "fabric/workload.hpp"
