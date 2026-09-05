// Platform job model implementation (Phase 11).

#include "platform/job.hpp"

#include <chrono>

#include "platform/identity.hpp"
#include "platform/metadata.hpp"

namespace vortyx::platform {

std::int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const char* to_string(JobStatus status) {
    switch (status) {
        case JobStatus::Queued: return "queued";
        case JobStatus::Running: return "running";
        case JobStatus::Completed: return "completed";
        case JobStatus::Failed: return "failed";
        case JobStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

bool job_status_is_terminal(JobStatus status) {
    switch (status) {
        case JobStatus::Completed:
        case JobStatus::Failed:
        case JobStatus::Cancelled: return true;
        case JobStatus::Queued:
        case JobStatus::Running: return false;
    }
    return false;
}

bool job_status_transition_valid(JobStatus from, JobStatus to) {
    if (job_status_is_terminal(from)) return false;
    switch (from) {
        case JobStatus::Queued:
            return to == JobStatus::Running || to == JobStatus::Cancelled;
        case JobStatus::Running:
            return to == JobStatus::Completed || to == JobStatus::Failed ||
                   to == JobStatus::Cancelled;
        default:
            return false;  // terminal handled above; nothing else exists
    }
}

Status validate_job_envelope(const JobEnvelope& envelope, std::string& error) {
    Status id_status = validate_id("job_id", envelope.job_id, error);
    if (id_status != Status::Ok) return id_status;

    if (envelope.element_count == 0) {
        error = "element_count must be greater than 0 (a zero-element job has nothing to compute)";
        return Status::InvalidInput;
    }
    if (envelope.element_count > kMaxJobElementCount) {
        error = "element_count exceeds the control-plane contract cap (" +
                std::to_string(kMaxJobElementCount) + ")";
        return Status::InvalidInput;
    }
    if (!envelope.requested_backend.empty() && !is_known_backend(envelope.requested_backend)) {
        error = "unknown requested_backend '" + envelope.requested_backend +
                "' (known: cpu, vulkan)";
        return Status::InvalidInput;
    }
    if (envelope.protocol_version != kProtocolVersion) {
        error = "unsupported protocol version '" + envelope.protocol_version +
                "' (this control plane speaks '" + kProtocolVersion + "')";
        return Status::InvalidInput;
    }
    error.clear();
    return Status::Ok;
}

Status validate_result_envelope(const ResultEnvelope& envelope, std::string& error) {
    Status id_status = validate_id("job_id", envelope.job_id, error);
    if (id_status != Status::Ok) return id_status;

    if (envelope.status != JobStatus::Completed && envelope.status != JobStatus::Failed) {
        error = std::string("a result envelope records an OUTCOME (completed or failed); '") +
                to_string(envelope.status) + "' is not an outcome";
        return Status::InvalidInput;
    }
    if (envelope.status == JobStatus::Failed && envelope.error.empty()) {
        error = "a failed result requires an error reason (failures are never hidden)";
        return Status::InvalidInput;
    }
    if (envelope.status == JobStatus::Completed && !envelope.error.empty()) {
        error = "a completed result must not carry an error string";
        return Status::InvalidInput;
    }
    if (!envelope.backend.empty() && !is_known_backend(envelope.backend)) {
        error = "unknown backend '" + envelope.backend + "' in result (known: cpu, vulkan)";
        return Status::InvalidInput;
    }
    error.clear();
    return Status::Ok;
}

}  // namespace vortyx::platform
