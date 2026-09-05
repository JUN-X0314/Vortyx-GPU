#pragma once

// Phase 12 configuration (Phase 12).
//
// The project's existing configuration pattern (checked against the actual
// repository before writing this): compile-time CMake options
// (VORTYX_ENABLE_*) for build-shape decisions, plus environment variables
// for the platform API layer's runtime configuration
// (platform/api/src/config.ts). Phase 12 follows both: the BUILD shape is
// the CMake option VORTYX_ENABLE_DISTRIBUTED, and the runtime knobs are
// VORTYX_DISTRIBUTED_* environment variables parsed here.
//
// Validation policy: a present-but-invalid value REJECTS (from_environment
// returns false with the reason). The one deliberate silent normalization
// is an ABSENT variable -> documented default (absence is not a wrong
// value). Defaults are safe/local: distributed features off, bounded
// retry, bounded devices.

#include <cstdint>
#include <string>
#include <vector>

#include "distributed/policy.hpp"  // the policy-name vocabulary
#include "platform/status.hpp"

namespace vortyx::distributed {

struct DistributedConfig {
    // Master switch (VORTYX_DISTRIBUTED_ENABLED). Default OFF: a caller
    // that never enables distributed features gets Phase 11 behavior with
    // zero new machinery in its path.
    bool enabled = false;

    // Scheduling policy name (VORTYX_SCHEDULER_POLICY). Must be one of
    // known_scheduling_policies() — an unknown name is a configuration
    // ERROR, never a silent default.
    std::string scheduler_policy = "least_loaded";

    // Cluster bounds (VORTYX_MAX_DEVICES). 0 = unlimited. A positive bound
    // makes the orchestrator refuse registrations beyond it.
    std::uint32_t max_devices = 16;

    // Liveness (VORTYX_HEARTBEAT_TIMEOUT_MS). Devices whose last heartbeat
    // is older are judged Unhealthy/Offline by the heartbeat monitor.
    std::int64_t heartbeat_timeout_ms = 30000;

    // Retry bounds (VORTYX_MAX_RETRIES): EXTRA attempts beyond the first.
    // 0 = a single attempt per shard. Total attempts = 1 + max_retries.
    std::uint32_t max_retries = 3;

    // Backoff base for retry delays (VORTYX_RETRY_BACKOFF_MS).
    std::int64_t retry_backoff_ms = 10;

    // Lease time-to-live (VORTYX_LEASE_TTL_MS). Reservations expire after
    // this long without being released.
    std::int64_t lease_ttl_ms = 600000;

    // Placement fallback (VORTYX_ALLOW_SINGLE_DEVICE_FALLBACK): when the
    // requested shard count exceeds the capable device count, coalesce to
    // the devices that exist instead of rejecting.
    bool allow_single_device_fallback = true;

    // Placement bound per job (VORTYX_MAX_SHARDS_PER_JOB).
    std::uint32_t max_shards_per_job = 64;

    // Execution model (VORTYX_SHARD_THREADS): 0 = execute the plan's shards
    // sequentially on the calling thread; N > 0 = at most N shards execute
    // concurrently on worker threads (one thread per shard, joined before
    // retries are planned). Thread-per-device serialization is guaranteed
    // by the workers themselves (see worker.hpp).
    std::uint32_t shard_threads = 0;

    // Validates the invariants that must hold regardless of the source
    // (environment or a hand-built struct). Ok, or InvalidInput with the
    // reason: unknown policy name, negative timeouts/ttls, an impossible
    // retry bound.
    vortyx::platform::Status validate(std::string& error) const;
};

// Parses the VORTYX_DISTRIBUTED_* environment variables on top of the
// defaults. Returns true and fills 'out' when the environment (if any) is
// valid; returns false with 'error' describing the FIRST invalid variable.
// Absent variables keep their defaults (absence is not an error).
bool distributed_config_from_environment(DistributedConfig& out, std::string& error);

// The exact variable names, exposed for tests and documentation.
const std::vector<std::string>& distributed_config_variables();

}  // namespace vortyx::distributed
