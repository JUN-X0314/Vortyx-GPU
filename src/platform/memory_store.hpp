#pragma once

// InMemoryPlatformStore (Phase 11) — the LOCAL/MOCK platform store.
//
// Purpose, stated loudly: this is a LOCAL DEVELOPMENT AND TEST
// implementation of IPlatformStore. It holds everything in process memory,
// persists NOTHING, and must never back a real deployment (a process restart
// loses every record). It exists so that:
//   - the Phase 11 test suite can exercise the full contract — registration,
//     ownership, idempotency, lifecycle transitions, results — with zero
//     infrastructure, zero accounts, zero secrets, and
//   - the API layer has something honest to run against before the real
//     Supabase project exists (actual configuration is deliberately deferred
//     until after Phase 11).
//
// Semantics: it implements the IPlatformStore contract EXACTLY — including
// the ownership rule (auth.hpp) and the job transition table (job.hpp) — so
// it doubles as the executable specification of what the Supabase adapter
// plus RLS must reproduce. Where the real backend would rely on the
// database, this store relies on nothing but its own code; that is the
// point: the rules live in the platform layer, not in any provider.
//
// Determinism: lists return records in insertion order (devices:
// registration order; jobs: submission order) — stable and testable, unlike
// hash-map iteration. Lookups are linear scans, which is fine for local/mock
// scale and honest about it.
//
// Threading: one mutex guards all state, so concurrent API calls against one
// store object are safe (the same guarantee the real backend gets from
// PostgreSQL transactions). No lock is held across any callback — there are
// no callbacks.

#include <mutex>
#include <string>
#include <vector>

#include "platform/store.hpp"

namespace vortyx::platform {

class InMemoryPlatformStore final : public IPlatformStore {
public:
    InMemoryPlatformStore() = default;

    // Copying/moving deleted: like every other Vortyx owner object, the
    // store's identity is meaningful (records live in it).
    InMemoryPlatformStore(const InMemoryPlatformStore&) = delete;
    InMemoryPlatformStore& operator=(const InMemoryPlatformStore&) = delete;
    InMemoryPlatformStore(InMemoryPlatformStore&&) = delete;
    InMemoryPlatformStore& operator=(InMemoryPlatformStore&&) = delete;

    // IPlatformStore (see store.hpp for the full contract of each method).
    Status register_device(const AuthContext& auth, const DeviceId& device_id,
                           const DeviceMetadata& metadata, DeviceRecord& out) override;
    Status device(const AuthContext& auth, const DeviceId& device_id,
                  DeviceRecord& out) override;
    Status devices(const AuthContext& auth, std::vector<DeviceRecord>& out) override;
    Status heartbeat_device(const AuthContext& auth, const DeviceId& device_id,
                            DeviceRecord& out) override;

    Status create_job(const AuthContext& auth, const JobEnvelope& envelope,
                      const std::optional<DeviceId>& submitted_by,
                      JobRecord& out, bool& created) override;
    Status job(const AuthContext& auth, const JobId& job_id, JobRecord& out) override;
    Status jobs(const AuthContext& auth, std::vector<JobRecord>& out) override;
    Status update_job(const AuthContext& auth, const JobId& job_id, JobStatus to,
                      const std::string& error_reason, JobRecord& out) override;
    Status cancel_job(const AuthContext& auth, const JobId& job_id, JobRecord& out) override;

    Status put_result(const AuthContext& auth, const ResultEnvelope& result,
                      ResultEnvelope& out) override;
    Status result(const AuthContext& auth, const JobId& job_id, ResultEnvelope& out) override;

private:
    // Records. Insertion order == deterministic list order (see above).
    std::vector<DeviceRecord> devices_;
    std::vector<JobRecord> jobs_;
    std::vector<ResultEnvelope> results_;  // one per terminal job, at most

    mutable std::mutex mutex_;
};

}  // namespace vortyx::platform
