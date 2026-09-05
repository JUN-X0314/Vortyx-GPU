#pragma once

// Audit logging (Phase 14).
//
// Every IMPORTANT control-plane operation leaves an audit event: project
// lifecycle, membership changes, job submission / cancellation / terminal
// outcomes, quota changes, device registration. The audit trail is the
// "who did what, when, with what outcome" record the platform foundation
// needs before anything user-facing ships.
//
// WHAT IS NEVER STORED (the security rule): passwords, tokens, secrets, raw
// request payloads, tensor data, hardware fingerprints — the event structure
// below has no field that could carry them, and no caller passes them.
//
// BOUNDED (the "must not grow unboundedly" rule): the in-memory store keeps
// at most max_entries events and DROPS THE OLDEST beyond that (a bounded
// ring, documented — not a silent unbounded list). The dropped count is
// reported honestly by dropped_total().
//
// Thread-safe; event ids are process-unique monotonic labels ("evt-...").

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "distributed/clock.hpp"
#include "platform/identity.hpp"

namespace vortyx::service {

enum class AuditAction : std::uint8_t {
    AuthContext,        // an authentication context was created (local mode)
    ProjectCreate,
    ProjectArchive,
    MembershipChange,
    JobSubmit,
    JobCancel,
    JobTerminal,        // a job reached a terminal state
    QuotaChange,
    DeviceRegister,
    DeviceStateChange,
    ResultAccess,
    ArtifactRegister,   // artifact METADATA registered (no payload exists)
    AdminAction,
};

const char* to_string(AuditAction action);

enum class AuditOutcome : std::uint8_t {
    Ok,      // the operation succeeded
    Denied,  // refused by policy (quota, rate limit, authorization)
    Error,   // failed for another reason
};

const char* to_string(AuditOutcome outcome);

struct AuditEvent {
    std::int64_t timestamp_ms = 0;          // the injected service clock
    std::string event_id;                   // process-unique "evt-..." label
    vortyx::platform::UserId actor_user_id; // who (empty for system events)
    std::string project_id;                 // scope ("" when not applicable)
    vortyx::platform::JobId job_id;         // scope ("" when not applicable)
    AuditAction action = AuditAction::AdminAction;
    AuditOutcome outcome = AuditOutcome::Ok;
    std::string reason_code;                // the refusal/failure code ("" on plain ok)
};

class IAuditStore {
public:
    virtual ~IAuditStore() = default;
    virtual void append(const AuditEvent& event) = 0;
    // The last 'count' events in chronological order.
    virtual std::vector<AuditEvent> tail(std::size_t count) const = 0;
    virtual std::size_t size() const = 0;
    virtual std::size_t dropped_total() const = 0;
};

// The bounded in-memory reference store.
class InMemoryAuditStore final : public IAuditStore {
public:
    explicit InMemoryAuditStore(std::size_t max_entries);

    void append(const AuditEvent& event) override;
    std::vector<AuditEvent> tail(std::size_t count) const override;
    std::size_t size() const override;
    std::size_t dropped_total() const override;

private:
    std::size_t max_entries_;
    std::vector<AuditEvent> events_;  // chronological; front dropped when full
    std::size_t dropped_ = 0;
    mutable std::mutex mutex_;
};

// The audit writer: stamps time and a unique event id, then appends. The
// facade owns one; every component records through it.
class AuditTrail final {
public:
    AuditTrail(std::shared_ptr<IAuditStore> store,
               std::shared_ptr<vortyx::distributed::IClock> clock);

    // Fills the event's timestamp/id and appends. Never throws, never blocks
    // on I/O (the in-memory store appends in O(1) amortized).
    void record(const AuditEvent& event);

    // Convenience for the common shape.
    void record(vortyx::platform::UserId actor, const std::string& project_id,
                const vortyx::platform::JobId& job_id, AuditAction action,
                AuditOutcome outcome, const std::string& reason_code);

    std::shared_ptr<IAuditStore> store() const { return store_; }

private:
    std::shared_ptr<IAuditStore> store_;
    std::shared_ptr<vortyx::distributed::IClock> clock_;
    static std::atomic<std::uint64_t> next_event_id_;  // process-unique labels
};

}  // namespace vortyx::service
