// Audit logging (Phase 14) — implementation.

#include "service/audit.hpp"

#include <cstdio>

namespace vortyx::service {

const char* to_string(AuditAction action) {
    switch (action) {
        case AuditAction::AuthContext: return "auth_context";
        case AuditAction::ProjectCreate: return "project_create";
        case AuditAction::ProjectArchive: return "project_archive";
        case AuditAction::MembershipChange: return "membership_change";
        case AuditAction::JobSubmit: return "job_submit";
        case AuditAction::JobCancel: return "job_cancel";
        case AuditAction::JobTerminal: return "job_terminal";
        case AuditAction::QuotaChange: return "quota_change";
        case AuditAction::DeviceRegister: return "device_register";
        case AuditAction::DeviceStateChange: return "device_state_change";
        case AuditAction::ResultAccess: return "result_access";
        case AuditAction::ArtifactRegister: return "artifact_register";
        case AuditAction::AdminAction: return "admin_action";
    }
    return "unknown";
}

const char* to_string(AuditOutcome outcome) {
    switch (outcome) {
        case AuditOutcome::Ok: return "ok";
        case AuditOutcome::Denied: return "denied";
        case AuditOutcome::Error: return "error";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// InMemoryAuditStore
// ---------------------------------------------------------------------------

InMemoryAuditStore::InMemoryAuditStore(std::size_t max_entries)
    : max_entries_(max_entries < 1 ? 1 : max_entries) {}

void InMemoryAuditStore::append(const AuditEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.size() >= max_entries_) {
        events_.erase(events_.begin());  // drop the OLDEST (bounded ring)
        ++dropped_;
    }
    events_.push_back(event);
}

std::vector<AuditEvent> InMemoryAuditStore::tail(std::size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count > events_.size()) count = events_.size();
    return std::vector<AuditEvent>(events_.end() - static_cast<std::ptrdiff_t>(count),
                                   events_.end());
}

std::size_t InMemoryAuditStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
}

std::size_t InMemoryAuditStore::dropped_total() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

// ---------------------------------------------------------------------------
// AuditTrail
// ---------------------------------------------------------------------------

std::atomic<std::uint64_t> AuditTrail::next_event_id_{1};

AuditTrail::AuditTrail(std::shared_ptr<IAuditStore> store,
                       std::shared_ptr<vortyx::distributed::IClock> clock)
    : store_(std::move(store)), clock_(std::move(clock)) {}

void AuditTrail::record(const AuditEvent& event) {
    if (!store_) return;
    AuditEvent stamped = event;
    stamped.timestamp_ms = clock_ ? clock_->now_ms() : 0;
    const std::uint64_t id = next_event_id_.fetch_add(1, std::memory_order_relaxed);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "evt-%010llu", static_cast<unsigned long long>(id));
    stamped.event_id = buf;
    store_->append(stamped);
}

void AuditTrail::record(vortyx::platform::UserId actor, const std::string& project_id,
                        const vortyx::platform::JobId& job_id, AuditAction action,
                        AuditOutcome outcome, const std::string& reason_code) {
    AuditEvent event;
    event.actor_user_id = std::move(actor);
    event.project_id = project_id;
    event.job_id = std::move(job_id);
    event.action = action;
    event.outcome = outcome;
    event.reason_code = reason_code;
    record(event);
}

}  // namespace vortyx::service
