#pragma once

// Project model and provider-neutral project store (Phase 14).
//
// One user owns projects; memberships grant other users roles. The project
// is the unit the service's quota / rate-limit / job bookkeeping attach to.
//
// IDENTITY (no second scheme): users are platform::UserId, jobs are
// platform::JobId, devices are platform::DeviceId (all reused verbatim). A
// project id is a NEW kind of id (there is no Phase 11 ProjectId to reuse)
// and follows the SAME syntax rule — 1..kMaxIdLength chars of
// [A-Za-z0-9._-], validated with the platform validator. Generation uses the
// same UUID-v4 shape as the platform generators (uniqueness labels, not
// security claims).
//
// AUTHORIZATION INSIDE THE STORE (the IPlatformStore pattern): every method
// takes the caller's AuthContext and applies the role table (authz.hpp)
// against the caller's project role. The service facade re-checks before
// calling; the store re-checks inside (defense in depth — the same rule the
// Phase 11 store established).
//
// ANTI-ENUMERATION (the Phase 11 security rule): a project that is unknown
// OR the caller has no membership in produces NotFound — never Forbidden,
// never a leak of which project ids exist. Forbidden is reserved for
// members whose role lacks the requested action.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "distributed/clock.hpp"  // IClock — the injected service clock
#include "platform/auth.hpp"
#include "platform/identity.hpp"
#include "platform/status.hpp"
#include "service/authz.hpp"
#include "service/service_status.hpp"

namespace vortyx::service {

using ProjectId = std::string;

// Project lifecycle: Active (normal operation) and Archived (read-only:
// viewing stays possible, submitting does not). There is no hard delete —
// archival is the terminal story (job records keep referential integrity).
enum class ProjectStatus : std::uint8_t {
    Active = 0,
    Archived = 1,
};

const char* to_string(ProjectStatus status);

// Upper bound for project names (bytes; control characters refused — names
// land in JSON and logs and must stay one-line safe).
inline constexpr std::size_t kMaxProjectNameLength = 128;
// Membership cap per project (a service-level resource limit; refusal is
// ResourceLimit-shaped: ServiceStatus::QuotaExceeded? No — a plain
// Conflict-free capacity refusal: ServiceStatus::Unavailable is wrong too;
// use ServiceStatus::InvalidInput? The honest code is a capacity refusal:
// ServiceStatus::Conflict is about state. The service uses
// ServiceStatus::QuotaExceeded ONLY for quota policy; membership capacity is
// a request-shape limit -> ServiceStatus::InvalidInput with a named reason).
inline constexpr std::size_t kMaxProjectMembers = 64;

struct ProjectRecord {
    ProjectId project_id;
    vortyx::platform::UserId owner_user_id;  // store-managed (the creator)
    std::string name;                        // validated (see kMaxProjectNameLength)

    ProjectStatus status = ProjectStatus::Active;
    std::int64_t created_at_ms = 0;  // service clock
    std::int64_t updated_at_ms = 0;  // service clock
};

struct ProjectMember {
    ProjectId project_id;
    vortyx::platform::UserId user_id;
    ProjectRole role = ProjectRole::Viewer;
    std::int64_t created_at_ms = 0;
};

// Generates a project id (UUID-v4 shape, the platform generators' format).
ProjectId generate_project_id();

// Validates a project record's caller-supplied fields (id syntax, name).
// Ok or ServiceStatus::InvalidInput with 'error'.
ServiceStatus validate_project_record(const ProjectRecord& record, std::string& error);

// ---------------------------------------------------------------------------
// IProjectStore — the provider-neutral seam (the IPlatformStore pattern).
// The in-memory implementation is the local/mock reference; a future
// Supabase-backed adapter would implement the same interface OUTSIDE the
// C++ core boundary.
// ---------------------------------------------------------------------------

class IProjectStore {
public:
    virtual ~IProjectStore() = default;

    // Creates a project owned by the authenticated user (the owner member
    // row is created with the project). 'record.project_id' may be empty —
    // the store then generates one (returned in 'out'). Errors:
    // Unauthenticated | InvalidInput (name / id) | Conflict (duplicate id) |
    // Internal.
    virtual ServiceStatus create_project(const vortyx::platform::AuthContext& auth,
                                         const ProjectRecord& record, ProjectRecord& out) = 0;

    // Fetches one project the caller can see (owner or member). Unknown or
    // foreign -> NotFound (anti-enumeration). Errors: Unauthenticated |
    // NotFound | Internal.
    virtual ServiceStatus project(const vortyx::platform::AuthContext& auth,
                                  const ProjectId& project_id, ProjectRecord& out) = 0;

    // Lists projects the caller owns or is a member of, in creation order.
    virtual ServiceStatus projects(const vortyx::platform::AuthContext& auth,
                                   std::vector<ProjectRecord>& out) = 0;

    // Owner-only archival (the authz table gates ArchiveProject). Sets
    // status Archived and stamps updated_at. Errors: Unauthenticated |
    // NotFound | Forbidden | InvalidInput (already archived) | Internal.
    virtual ServiceStatus archive_project(const vortyx::platform::AuthContext& auth,
                                          const ProjectId& project_id, ProjectRecord& out) = 0;

    // Adds a member (Admin+ per the authz table). Adding an existing member
    // is a Conflict. The owner's role cannot be granted again (they are
    // already Owner — Conflict). Errors: Unauthenticated | NotFound |
    // Forbidden | InvalidInput (bad user id / role) | Conflict | Internal.
    virtual ServiceStatus add_member(const vortyx::platform::AuthContext& auth,
                                     const ProjectId& project_id,
                                     const vortyx::platform::UserId& user_id, ProjectRole role,
                                     ProjectMember& out) = 0;

    // Removes a member (Admin+). The owner cannot be removed. Removing a
    // non-member is NotFound. Errors: Unauthenticated | NotFound | Forbidden
    // | InvalidInput (owner removal attempt) | Internal.
    virtual ServiceStatus remove_member(const vortyx::platform::AuthContext& auth,
                                        const ProjectId& project_id,
                                        const vortyx::platform::UserId& user_id) = 0;

    // Lists members (Viewer+). Errors: Unauthenticated | NotFound | Forbidden
    // | Internal.
    virtual ServiceStatus members(const vortyx::platform::AuthContext& auth,
                                  const ProjectId& project_id,
                                  std::vector<ProjectMember>& out) = 0;

    // The caller's role in a project — the service facade's authorization
    // query. NotFound when the project is unknown or the caller is neither
    // owner nor member (the SAME anti-enumeration rule). Errors:
    // Unauthenticated | NotFound | Internal.
    virtual ServiceStatus role_of(const vortyx::platform::AuthContext& auth,
                                  const ProjectId& project_id, ProjectRole& out) = 0;
};

// The local/mock reference implementation (clearly labeled, like
// InMemoryPlatformStore). Thread-safe (the service may call it from several
// dispatchers); creation order is stable; no map-iteration-order dependence
// anywhere in the observable behavior.
class InMemoryProjectStore final : public IProjectStore {
public:
    ServiceStatus create_project(const vortyx::platform::AuthContext& auth,
                                 const ProjectRecord& record, ProjectRecord& out) override;
    ServiceStatus project(const vortyx::platform::AuthContext& auth, const ProjectId& project_id,
                          ProjectRecord& out) override;
    ServiceStatus projects(const vortyx::platform::AuthContext& auth,
                           std::vector<ProjectRecord>& out) override;
    ServiceStatus archive_project(const vortyx::platform::AuthContext& auth,
                                  const ProjectId& project_id, ProjectRecord& out) override;
    ServiceStatus add_member(const vortyx::platform::AuthContext& auth,
                             const ProjectId& project_id, const vortyx::platform::UserId& user_id,
                             ProjectRole role, ProjectMember& out) override;
    ServiceStatus remove_member(const vortyx::platform::AuthContext& auth,
                                const ProjectId& project_id,
                                const vortyx::platform::UserId& user_id) override;
    ServiceStatus members(const vortyx::platform::AuthContext& auth, const ProjectId& project_id,
                          std::vector<ProjectMember>& out) override;
    ServiceStatus role_of(const vortyx::platform::AuthContext& auth, const ProjectId& project_id,
                          ProjectRole& out) override;

    // The service clock (epoch ms) used for created/updated stamps. The
    // store does not invent one: the facade injects the shared IClock (the
    // same instance the rest of the service reads — one time source).
    void set_clock(std::shared_ptr<vortyx::distributed::IClock> clock) { clock_ = std::move(clock); }

private:
    // Resolved role or the NotFound/Unauthenticated outcome (shared precheck).
    ServiceStatus resolve_visible(const vortyx::platform::AuthContext& auth,
                                  const ProjectId& project_id, ProjectRecord& out_project,
                                  ProjectRole& out_role);

    std::shared_ptr<vortyx::distributed::IClock> clock_;
    std::vector<ProjectRecord> projects_;  // creation order
    // (project_id -> members in add order).
    std::vector<std::pair<ProjectId, std::vector<ProjectMember>>> members_;
    mutable std::mutex mutex_;
};

}  // namespace vortyx::service
