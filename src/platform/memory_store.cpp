// InMemoryPlatformStore implementation (Phase 11).
//
// The authorization decisions below go through auth.hpp's shared rules, the
// envelope/result validation through job.hpp's validators, and the status
// transitions through job_status_transition_valid — the in-memory store
// defines none of them locally, so it cannot drift from the platform
// contract it is the reference implementation of.

#include "platform/memory_store.hpp"

namespace vortyx::platform {

const char* to_string(DeviceStatus status) {
    switch (status) {
        case DeviceStatus::Online: return "online";
        case DeviceStatus::Offline: return "offline";
    }
    return "unknown";
}

namespace {

// Common failure text for single-record access to missing OR foreign
// records: identical to what RLS produces (a foreign row is invisible).
// Deliberately NEVER a Forbidden — that would leak which ids exist for
// other users.
constexpr const char* kNoSuchDevice = "no such device";
constexpr const char* kNoSuchJob = "no such job";

}  // namespace

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

Status InMemoryPlatformStore::register_device(const AuthContext& auth, const DeviceId& device_id,
                                              const DeviceMetadata& metadata,
                                              DeviceRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    Status status = validate_auth(auth, error);
    if (status != Status::Ok) return status;
    status = validate_id("device_id", device_id, error);
    if (status != Status::Ok) return status;
    status = validate_device_metadata(metadata, error);
    if (status != Status::Ok) return status;

    // Duplicate registration with ANY owner is a conflict; the error must
    // not reveal who owns the existing registration.
    for (const DeviceRecord& existing : devices_) {
        if (existing.device_id == device_id) {
            error = "device_id is already registered";
            return Status::Conflict;
        }
    }

    DeviceRecord record;
    record.device_id = device_id;
    record.owner_user_id = auth.user_id;  // server-side subject, never client-claimed
    record.metadata = metadata;
    record.status = DeviceStatus::Online;  // the control plane just heard from it
    const std::int64_t now = now_epoch_ms();
    record.last_seen_ms = now;
    record.created_at_ms = now;

    devices_.push_back(record);
    out = record;
    error.clear();
    return Status::Ok;
}

Status InMemoryPlatformStore::device(const AuthContext& auth, const DeviceId& device_id,
                                     DeviceRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    Status status = validate_auth(auth, error);
    if (status != Status::Ok) return status;

    for (const DeviceRecord& existing : devices_) {
        if (existing.device_id == device_id) {
            if (!is_owner(auth, existing.owner_user_id)) {
                error = kNoSuchDevice;  // foreign -> invisible (RLS rule)
                return Status::NotFound;
            }
            out = existing;
            error.clear();
            return Status::Ok;
        }
    }
    error = "no such device";
    return Status::NotFound;
}

Status InMemoryPlatformStore::devices(const AuthContext& auth, std::vector<DeviceRecord>& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    Status status = validate_auth(auth, error);
    if (status != Status::Ok) return status;

    out.clear();
    for (const DeviceRecord& existing : devices_) {
        if (is_owner(auth, existing.owner_user_id)) {
            out.push_back(existing);
        }
    }
    error.clear();
    return Status::Ok;
}

Status InMemoryPlatformStore::heartbeat_device(const AuthContext& auth, const DeviceId& device_id,
                                               DeviceRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    Status status = validate_auth(auth, error);
    if (status != Status::Ok) return status;

    for (DeviceRecord& existing : devices_) {
        if (existing.device_id == device_id) {
            if (!is_owner(auth, existing.owner_user_id)) {
                error = kNoSuchDevice;  // foreign -> invisible (RLS rule)
                return Status::NotFound;
            }
            existing.status = DeviceStatus::Online;
            existing.last_seen_ms = now_epoch_ms();
            out = existing;
            error.clear();
            return Status::Ok;
        }
    }
    error = "no such device";
    return Status::NotFound;
}

// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------

Status InMemoryPlatformStore::create_job(const AuthContext& auth, const JobEnvelope& envelope,
                                         const std::optional<DeviceId>& submitted_by,
                                         JobRecord& out, bool& created) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    Status status = validate_auth(auth, error);
    if (status != Status::Ok) return status;
    status = validate_job_envelope(envelope, error);
    if (status != Status::Ok) return status;

    // Idempotency check FIRST: a replay of the same submission must return
    // the existing record, never create a second one.
    for (const JobRecord& existing : jobs_) {
        if (existing.job.job_id != envelope.job_id) continue;
        const bool same_owner = existing.owner_user_id == auth.user_id;
        const bool same_payload =
            existing.job.operation == envelope.operation &&
            existing.job.element_count == envelope.element_count &&
            existing.job.requested_backend == envelope.requested_backend &&
            existing.job.priority == envelope.priority &&
            existing.job.protocol_version == envelope.protocol_version &&
            existing.job.created_at_ms == envelope.created_at_ms &&
            existing.submitted_by_device_id == submitted_by;
        if (same_owner && same_payload) {
            out = existing;
            created = false;
            error.clear();
            return Status::Ok;
        }
        error = "job_id is already used by a different submission";
        return Status::Conflict;  // same id, different payload (or owner)
    }

    // Cross-record authorization: the submitting device (when given) must
    // exist AND belong to the caller. Forbidden either way — the error must
    // not disclose which foreign/unknown ids exist.
    if (submitted_by.has_value()) {
        bool found_owned = false;
        for (const DeviceRecord& device : devices_) {
            if (device.device_id == *submitted_by) {
                if (is_owner(auth, device.owner_user_id)) {
                    found_owned = true;
                }
                break;
            }
        }
        if (!found_owned) {
            error = "submitted_by_device_id must reference a device owned by the authenticated user";
            return Status::Forbidden;
        }
    }

    JobRecord record;
    record.job = envelope;
    record.owner_user_id = auth.user_id;
    record.submitted_by_device_id = submitted_by;
    record.status = JobStatus::Queued;
    record.created_at_ms = now_epoch_ms();

    jobs_.push_back(record);
    out = record;
    created = true;
    error.clear();
    return Status::Ok;
}

Status InMemoryPlatformStore::job(const AuthContext& auth, const JobId& job_id, JobRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    Status status = validate_auth(auth, error);
    if (status != Status::Ok) return status;

    for (const JobRecord& existing : jobs_) {
        if (existing.job.job_id == job_id) {
            if (!is_owner(auth, existing.owner_user_id)) {
                error = kNoSuchJob;  // foreign -> invisible (RLS rule)
                return Status::NotFound;
            }
            out = existing;
            error.clear();
            return Status::Ok;
        }
    }
    error = "no such job";
    return Status::NotFound;
}

Status InMemoryPlatformStore::jobs(const AuthContext& auth, std::vector<JobRecord>& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    Status status = validate_auth(auth, error);
    if (status != Status::Ok) return status;

    out.clear();
    for (const JobRecord& existing : jobs_) {
        if (is_owner(auth, existing.owner_user_id)) {
            out.push_back(existing);
        }
    }
    error.clear();
    return Status::Ok;
}

Status InMemoryPlatformStore::update_job(const AuthContext& auth, const JobId& job_id,
                                         JobStatus to, const std::string& error_reason,
                                         JobRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    Status status = validate_auth(auth, error);
    if (status != Status::Ok) return status;

    for (JobRecord& existing : jobs_) {
        if (existing.job.job_id == job_id) {
            if (!is_owner(auth, existing.owner_user_id)) {
                error = kNoSuchJob;  // foreign -> invisible (RLS rule)
                return Status::NotFound;
            }
            if (!job_status_transition_valid(existing.status, to)) {
                error = std::string("invalid status transition '") + to_string(existing.status) +
                        "' -> '" + to_string(to) + "'";
                return Status::InvalidInput;
            }
            if (to == JobStatus::Failed && error_reason.empty()) {
                error = "a failed transition requires an error reason (failures are never hidden)";
                return Status::InvalidInput;
            }
            const std::int64_t now = now_epoch_ms();
            if (to == JobStatus::Running) {
                existing.started_at_ms = now;
            }
            if (job_status_is_terminal(to)) {
                existing.completed_at_ms = now;
                existing.error = error_reason;
            }
            existing.status = to;
            out = existing;
            error.clear();
            return Status::Ok;
        }
    }
    error = "no such job";
    return Status::NotFound;
}

Status InMemoryPlatformStore::cancel_job(const AuthContext& auth, const JobId& job_id,
                                         JobRecord& out) {
    return update_job(auth, job_id, JobStatus::Cancelled, "cancelled", out);
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

Status InMemoryPlatformStore::put_result(const AuthContext& auth, const ResultEnvelope& result,
                                         ResultEnvelope& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    Status status = validate_auth(auth, error);
    if (status != Status::Ok) return status;
    status = validate_result_envelope(result, error);
    if (status != Status::Ok) return status;

    for (JobRecord& existing : jobs_) {
        if (existing.job.job_id != result.job_id) continue;
        if (!is_owner(auth, existing.owner_user_id)) {
            error = kNoSuchJob;  // foreign -> invisible (RLS rule)
            return Status::NotFound;
        }
        if (existing.status == JobStatus::Queued) {
            error = "job has not started; a result can only be recorded for a running job";
            return Status::InvalidInput;
        }
        if (job_status_is_terminal(existing.status)) {
            error = "job already reached a terminal state";
            return Status::Conflict;
        }
        // Transition Running -> (Completed | Failed) exactly like update_job
        // would; the envelope's error text becomes the job's error.
        existing.completed_at_ms = now_epoch_ms();
        existing.status = result.status;
        existing.error = result.error;

        ResultEnvelope stored = result;
        results_.push_back(stored);
        out = std::move(stored);
        error.clear();
        return Status::Ok;
    }
    error = "no such job";
    return Status::NotFound;
}

Status InMemoryPlatformStore::result(const AuthContext& auth, const JobId& job_id,
                                     ResultEnvelope& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string error;
    Status status = validate_auth(auth, error);
    if (status != Status::Ok) return status;

    // The job must exist and be owned by the caller. Missing AND foreign
    // jobs are the same NotFound (existence of foreign ids is never
    // disclosed; see the RLS-equivalence rule above).
    bool job_owned = false;
    for (const JobRecord& existing : jobs_) {
        if (existing.job.job_id == job_id) {
            if (!is_owner(auth, existing.owner_user_id)) {
                error = kNoSuchJob;
                return Status::NotFound;
            }
            job_owned = true;
            break;
        }
    }
    if (!job_owned) {
        error = "no such job";
        return Status::NotFound;
    }
    for (const ResultEnvelope& existing : results_) {
        if (existing.job_id == job_id) {
            out = existing;
            error.clear();
            return Status::Ok;
        }
    }
    error = "no result has been recorded for this job";
    return Status::NotFound;
}

}  // namespace vortyx::platform
