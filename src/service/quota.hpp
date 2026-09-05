#pragma once

// Project quota engine (Phase 14).
//
// TWO SEPARATE RESPONSIBILITIES, deliberately two separate systems (the
// Phase 12 resource reservation is NOT merged with this one):
//
//   Project Quota (HERE)   — user/project-level POLICY: how much of the
//                            service one project may consume at once
//                            (concurrent jobs, running shards, reserved
//                            memory). A policy refusal never touches the
//                            cluster.
//   Cluster Reservation    — scheduler-level ALLOCATION: the Phase 12
//                            registry's atomic ResourceVector lease on a
//                            concrete device. The orchestrator owns it; the
//                            quota engine never reserves cluster capacity
//                            and the registry never reads quotas. The two
//                            accounting worlds stay independent (no double
//                            accounting: the quota ledger charges a JOB
//                            once per its lifetime, the registry charges a
//                            SHARD for its lease duration).
//
// THE LEDGER (the consistency core): every accepted reservation is recorded
// under the job's id. Consequences, all pinned by tests:
//   - release(job_id) returns true EXACTLY once per job — a second release
//     is refused, so cancel/completion races can never drive usage negative
//     or double-credit capacity;
//   - reserve() for an already-reserved job with the SAME dimensions is a
//     replay (no double charge) — a retried submission cannot double-charge;
//   - reserve() for an already-reserved job with DIFFERENT dimensions is a
//     Conflict;
//   - every refusal names the quota field that would have been exceeded.
//
// Thread-safe: one internal mutex, no external lock dependencies (the
// facade's lock ordering rules never call INTO the engine while holding
// another engine's lock — see platform_service.hpp).

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform/identity.hpp"
#include "service/service_status.hpp"

namespace vortyx::service {

// The policy limits of one project. Values are POLICY, not measurements:
// nothing here claims a hardware or cluster property. Defaults are the
// service's documented starting policy (configurable per project and as the
// engine-wide default).
struct ProjectQuota {
    std::int64_t max_concurrent_jobs = 4;    // jobs in flight (queued+running)
    std::int64_t max_running_shards = 16;    // shard count of in-flight jobs
    std::int64_t max_memory_bytes = 1024LL * 1024 * 1024;  // reserved bytes (1 GiB)
};

// The live usage of one project (real bookkeeping — the ledger's sum).
struct QuotaUsage {
    std::int64_t active_jobs = 0;             // reserved (in-flight) jobs
    std::int64_t running_shards = 0;          // their shard sums
    std::int64_t reserved_memory_bytes = 0;   // their byte sums
};

class QuotaEngine {
public:
    QuotaEngine() = default;

    // -- policy -------------------------------------------------------------
    void set_default_quota(const ProjectQuota& quota);
    ProjectQuota default_quota() const;

    // Sets the project's quota (overriding the default). The engine accepts
    // the values as given; validation (positivity) is the caller's contract
    // — a non-positive limit simply refuses everything, which the caller
    // (the facade, after authz) validates before calling. Pure policy data.
    void set_quota(const std::string& project_id, const ProjectQuota& quota);
    ProjectQuota quota(const std::string& project_id) const;

    // -- reservation ----------------------------------------------------------
    struct Decision {
        ServiceStatus status = ServiceStatus::Ok;
        std::string error;      // the exceeded field, or the conflict reason
        QuotaUsage usage_after; // meaningful when status == Ok (a replay
                                // reports the unchanged usage)
    };

    // Atomically reserves one job's footprint for 'project_id'.
    //   dimensions: shards >= 1, memory_bytes >= 0 (else InvalidInput)
    //   replay:     same job_id + same dimensions        -> Ok (no charge)
    //   conflict:   same job_id + different dimensions   -> Conflict
    //   exceeded:   any field over its quota             -> QuotaExceeded
    Decision reserve(const std::string& project_id, const vortyx::platform::JobId& job_id,
                     std::int64_t shards, std::int64_t memory_bytes);

    // Releases the job's reservation. True EXACTLY once per job (a second
    // release returns false — never an error at the call site, the ledger
    // simply refuses to go negative or double-credit).
    bool release(const vortyx::platform::JobId& job_id);

    // True while the job holds a reservation (observability / tests).
    bool has_reservation(const vortyx::platform::JobId& job_id) const;

    // The project's current usage (0 for an unknown project).
    QuotaUsage usage(const std::string& project_id) const;

private:
    struct Reservation {
        std::string project_id;
        std::int64_t shards = 0;
        std::int64_t memory_bytes = 0;
    };

    ProjectQuota quota_for_locked(const std::string& project_id) const;

    mutable std::mutex mutex_;
    ProjectQuota default_quota_;
    std::unordered_map<std::string, ProjectQuota> quotas_;      // per project
    std::unordered_map<std::string, QuotaUsage> usage_;         // per project
    std::unordered_map<vortyx::platform::JobId, Reservation> ledger_;
};

}  // namespace vortyx::service
