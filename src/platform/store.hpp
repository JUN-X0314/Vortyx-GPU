#pragma once

// Provider-neutral platform store (Phase 11).
//
// THE seam between the platform contract and any control-plane backend.
// The API layer speaks ONLY to this interface; concrete providers live
// behind it:
//
//   InMemoryPlatformStore  — local/mock implementation (tests + local
//                            development; clearly labeled, never production)
//   Supabase adapter       — future/optional, configured through environment
//                            variables, NEVER compiled into or linked from
//                            the C++ core (the adapter boundary is the whole
//                            point of Phase 11)
//
// Every method takes the caller's AuthContext and applies the SAME
// authorization rule (auth.hpp): a user may touch exactly the records they
// own. The in-memory store is the executable specification of what the
// Supabase Row Level Security policies enforce for real.
//
// Result vocabulary: platform::Status only (see status.hpp for the HTTP
// mapping). No method throws; no method blocks on I/O in the in-memory
// implementation; no method ever fabricates a value it does not know.

#include <optional>
#include <string>
#include <vector>

#include "platform/auth.hpp"
#include "platform/job.hpp"
#include "platform/metadata.hpp"
#include "platform/status.hpp"

namespace vortyx::platform {

// Lifecycle of a device's PRESENCE in the control plane (self-reported via
// registration/heartbeat — the control plane observes, it does not measure).
enum class DeviceStatus {
    Online,   // the control plane heard from this node (registered/heartbeat)
    Offline,  // known node that has not reported (or reported leaving)
};

const char* to_string(DeviceStatus status);

// A registered node. Server-managed fields (owner, timestamps, status) are
// filled by the STORE — a client-claimed owner or last_seen would be a
// broken authorization model, so no write path accepts them.
struct DeviceRecord {
    DeviceId device_id;
    UserId owner_user_id;     // server-side: the authenticated subject at registration
    DeviceMetadata metadata;  // self-reported description (validated)

    DeviceStatus status = DeviceStatus::Offline;
    std::optional<std::int64_t> last_seen_ms;  // server clock at last report
    std::optional<std::int64_t> created_at_ms;
};

// A submitted job plus its control-plane state. owner_user_id and the
// server timestamps are store-managed, exactly like DeviceRecord.
struct JobRecord {
    JobEnvelope job;                      // submission contract (validated)
    UserId owner_user_id;
    std::optional<DeviceId> submitted_by_device_id;  // must exist AND be owned by the same user

    JobStatus status = JobStatus::Queued;
    std::string error;                    // failure/cancel reason (Failed requires one)

    std::optional<std::int64_t> created_at_ms;    // server clock at submission
    std::optional<std::int64_t> started_at_ms;    // set on Queued -> Running
    std::optional<std::int64_t> completed_at_ms;  // set on any -> terminal transition
};

class IPlatformStore {
public:
    virtual ~IPlatformStore() = default;

    // ---- devices ---------------------------------------------------------

    // Registers a NEW device owned by the authenticated user. The record is
    // returned with server fields filled (status Online, timestamps set).
    // Errors: Unauthenticated | InvalidInput (bad id / metadata) | Conflict
    // (the device id is already registered — with ANY owner; the error never
    // reveals who owns it) | Internal.
    virtual Status register_device(const AuthContext& auth, const DeviceId& device_id,
                                   const DeviceMetadata& metadata, DeviceRecord& out) = 0;

    // Fetches one own device. A device that is missing OR belongs to
    // another user is NotFound (anti-enumeration; identical to what RLS
    // does — foreign rows are invisible). Errors: Unauthenticated |
    // NotFound | Internal.
    virtual Status device(const AuthContext& auth, const DeviceId& device_id,
                          DeviceRecord& out) = 0;

    // Lists the authenticated user's devices in registration order.
    virtual Status devices(const AuthContext& auth, std::vector<DeviceRecord>& out) = 0;

    // Marks an own device as heard-from: status Online, last_seen/updated
    // stamped with the server clock. Missing or foreign -> NotFound.
    // Errors: Unauthenticated | NotFound | Internal.
    virtual Status heartbeat_device(const AuthContext& auth, const DeviceId& device_id,
                                    DeviceRecord& out) = 0;

    // ---- jobs ------------------------------------------------------------

    // Submits a job. IDEMPOTENCY: submitting the same job_id again
    //   - with the same owner and the same envelope payload -> Status::Ok
    //     with created == false and the EXISTING record in 'out' (a replay,
    //     not a duplicate row),
    //   - with a different owner or a different payload     -> Status::Conflict.
    // 'submitted_by' (optional) must reference a device that exists AND is
    // owned by the caller; anything else is Forbidden (the same outcome the
    // RLS INSERT ... WITH CHECK policy produces; unknown and foreign device
    // ids are indistinguishable on purpose). Errors otherwise:
    // Unauthenticated | InvalidInput (envelope) | Forbidden | Conflict |
    // Internal.
    virtual Status create_job(const AuthContext& auth, const JobEnvelope& envelope,
                              const std::optional<DeviceId>& submitted_by,
                              JobRecord& out, bool& created) = 0;

    // Fetches one own job (any state). Missing or foreign -> NotFound (RLS
    // equivalence). Errors: Unauthenticated | NotFound | Internal.
    virtual Status job(const AuthContext& auth, const JobId& job_id, JobRecord& out) = 0;

    // Lists the authenticated user's jobs in submission order.
    virtual Status jobs(const AuthContext& auth, std::vector<JobRecord>& out) = 0;

    // Applies a status transition (the documented table in job.hpp). Sets
    // started_at (-> Running) or completed_at (-> any terminal state) with
    // the server clock. For -> Failed / -> Cancelled, 'error_reason' becomes
    // the record's error text; a Failed transition with an empty reason is
    // refused (failures are never hidden). Missing or foreign jobs are
    // NotFound. Errors: Unauthenticated | InvalidInput (illegal transition /
    // missing failure reason) | NotFound | Internal.
    virtual Status update_job(const AuthContext& auth, const JobId& job_id, JobStatus to,
                              const std::string& error_reason, JobRecord& out) = 0;

    // Owner-initiated cancellation: update_job(..., Cancelled, "cancelled").
    // Cancelling an already-terminal job is an ILLEGAL TRANSITION and is
    // refused with InvalidInput (same rule as update_job). Errors: same as
    // update_job.
    virtual Status cancel_job(const AuthContext& auth, const JobId& job_id, JobRecord& out) = 0;

    // ---- results ----------------------------------------------------------

    // Records the OUTCOME of an own job that is currently Running: the job
    // is transitioned to the envelope's terminal status (completed_at
    // stamped) and the result metadata is stored. The envelope's status must
    // be Completed or Failed (cancellation is an owner action, not a
    // result). Missing or foreign jobs are NotFound. Errors: Unauthenticated
    // | InvalidInput (envelope / job not running) | NotFound | Conflict (a
    // result already exists) | Internal.
    virtual Status put_result(const AuthContext& auth, const ResultEnvelope& result,
                              ResultEnvelope& out) = 0;

    // Fetches the stored result of an own job. Errors: Unauthenticated |
    // NotFound (unknown job, foreign job, OR no result recorded yet — the
    // error text says which) | Internal.
    virtual Status result(const AuthContext& auth, const JobId& job_id,
                          ResultEnvelope& out) = 0;
};

}  // namespace vortyx::platform
