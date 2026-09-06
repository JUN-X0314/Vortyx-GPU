#pragma once

// Fabric cost model (Phase 16) — a deterministic heuristic scoring rule.
//
// ═══════════════════════════════════════════════════════════════════════
// WHAT THE COST MODEL IS: an explicit, integer-arithmetic preference order
// over CAPABLE devices, computed from metadata the cluster snapshot really
// carries. WHAT IT IS NOT: a measured-performance model. No throughput,
// latency, utilization or transfer-time value exists anywhere in the
// inputs, so none appears in the score — a fabricated number would be a
// performance claim the system never measured (forbidden here like
// everywhere else in Vortyx).
//
// DETERMINISM CONTRACT: all arithmetic is int64. The same (candidate,
// request, weights) always produces the same score and the same breakdown
// on every platform — no floating point, no wall clock, no hash iteration
// order. Weight overflow is REFUSED (checked multiplication), never
// wrapped: a silently wrapped score would silently invert a preference.
//
// SCORE SHAPE (higher = better; every component named and bounded):
//
//   base             — a constant acknowledging an ADMITTED candidate
//                      (capability + state + resource filters passed).
//                      Keeps admitted candidates above every rejection
//                      path in one reading.
//   slack_penalty    — (available_memory_bytes - needed_memory_bytes) per
//                      byte: prefers the TIGHTEST fit (CapabilityFit's
//                      smallest-slack rule, generalized). Weighted.
//   queue_penalty    — running_shards per shard: prefer less-busy devices
//                      (LeastLoaded's rule, weighted). A real observed
//                      state of the snapshot — not a load prediction.
//   locality_bonus   — the descriptor's locality hint (preferred_device)
//                      matched: data is ALREADY resident, so avoiding an
//                      unimplemented cross-device transfer is a metadata
//                      preference, honestly labeled (no transfer engine
//                      exists — see docs/fabric/planning.md).
//   backend_bonus    — the device's preferred backend matches the
//                      requested backend (or the request expressed none
//                      and the device has one): a self-reported claim
//                      alignment, not a measured speed.
//
//   total = base - slack*slack_w - queue*queue_w + locality_w + backend_w
//
// Weights live in FabricPlannerConfig — an immutable per-run value passed
// to every planning call. There is NO global mutable configuration: two
// planners with different configs run concurrently without contaminating
// each other, and a plan records the config fingerprint that produced it.
// ═══════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>

#include "distributed/cluster.hpp"  // DeviceSnapshot
#include "fabric/workload.hpp"
#include "platform/status.hpp"

namespace vortyx::fabric {

// Weight bounds (validated). Bounds keep every weighted term far below the
// int64 overflow line even for maximal snapshots — the checked arithmetic
// below is the second line of defense, not the only one.
inline constexpr std::int64_t kMaxWeight = 1000000000000LL;  // 1e12

struct FabricPlannerConfig {
    // The constant admitted-candidate base score.
    std::int64_t base_score = 1000000;

    // Per byte of memory slack (tighter fit preferred).
    std::int64_t slack_penalty_weight = 1;

    // Per running shard on the device (less-busy preferred).
    std::int64_t queue_penalty_weight = 1000;

    // Flat bonus when the locality hint matches the candidate.
    std::int64_t locality_bonus_weight = 500000;

    // Flat bonus when the backend preference aligns.
    std::int64_t backend_bonus_weight = 100000;

    // Validates the invariants that must hold for planning to be trusted:
    // every weight in [0, kMaxWeight], base_score >= 0. Ok, or
    // InvalidInput with the reason. Pure.
    Status validate(std::string& error) const;
};

// The named score components of one candidate (the structured explanation
// the planner records per decision — see plan.hpp).
struct ScoreBreakdown {
    std::int64_t base = 0;
    std::int64_t slack_penalty = 0;   // <= 0 (already weighted)
    std::int64_t queue_penalty = 0;   // <= 0 (already weighted)
    std::int64_t locality_bonus = 0;  // >= 0
    std::int64_t backend_bonus = 0;   // >= 0
    std::int64_t total = 0;

    friend bool operator==(const ScoreBreakdown& a, const ScoreBreakdown& b) {
        return a.base == b.base && a.slack_penalty == b.slack_penalty &&
               a.queue_penalty == b.queue_penalty && a.locality_bonus == b.locality_bonus &&
               a.backend_bonus == b.backend_bonus && a.total == b.total;
    }
    friend bool operator!=(const ScoreBreakdown& a, const ScoreBreakdown& b) {
        return !(a == b);
    }
};

// The inputs of one scoring decision (everything the score may see — the
// planner fills this; no hidden inputs exist).
struct ScoreInputs {
    const vortyx::distributed::DeviceSnapshot* candidate =
        nullptr;                          // non-owning; must outlive the call
    std::int64_t needed_memory_bytes = 0;  // the shard's computed requirement
    bool locality_match = false;           // descriptor.preferred_device == candidate
    bool backend_match = false;            // preference alignment (see module header)
};

// Checked multiplication: false (with 'error' filled) on overflow, never a
// wrapped product. Pure.
bool checked_scale(std::int64_t value, std::int64_t weight, std::int64_t& out,
                   std::string& error);

// Scores one candidate. Pure function of (inputs, config). On overflow
// returns false with 'error' (the planner refuses the candidate rather
// than ranking it with a wrapped score).
bool score_candidate(const ScoreInputs& inputs, const FabricPlannerConfig& config,
                     ScoreBreakdown& out, std::string& error);

// The deterministic candidate ordering rule, exposed so every consumer
// agrees on it: HIGHER total score first; exact ties broken by the SMALLER
// device id (lexicographic — stable identifiers as tie-breakers, never
// iteration order). Returns true when 'a' ranks before 'b'.
bool candidate_ranks_before(const ScoreBreakdown& a, const std::string& device_a,
                            const ScoreBreakdown& b, const std::string& device_b);

}  // namespace vortyx::fabric
