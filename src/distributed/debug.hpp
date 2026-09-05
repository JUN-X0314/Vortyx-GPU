#pragma once

// Debug / diagnostic text rendering (Phase 12) — the minimum visibility a
// distributed system needs (nothing here is a logging framework; it renders
// deterministic multi-line text for the diagnostic tool, tests and logs).
//
// Every renderer is a pure function of its input: no clock reads, no
// hidden state, no timestamps beyond the fields the records already carry
// (so test assertions on the output are deterministic).

#include <string>

#include "distributed/cluster.hpp"
#include "distributed/orchestrator.hpp"  // DistributedJobRecord

namespace vortyx::distributed {

// Multi-line human-readable dump of one cluster view (owner-filtered
// snapshot): revision, device states/health/capacities/allocations.
std::string to_debug_string(const ClusterSnapshot& snapshot);

// Multi-line dump of one distributed job: status, error, per-shard table.
std::string to_debug_string(const DistributedJobRecord& job);

}  // namespace vortyx::distributed
