#pragma once

// Project authorization model (Phase 14).
//
// The PROJECT is the service-layer unit of ownership and policy. One USER
// (the Phase 11 platform::UserId — reused verbatim, never a second identity
// scheme) owns projects; other users hold MEMBERSHIPS with a role. The role
// decides what the member may do to the project and its jobs.
//
// AUTHN vs AUTHZ (the Phase 11 rule, kept strict):
//   - Authentication ("who are you?") happens at the transport boundary and
//     produces the platform::AuthContext. The service never accepts a
//     client-claimed user id as an identity.
//   - Authorization ("what may you do?") is THIS module: a pure decision
//     table from (role, action) -> allowed. It is deliberately plain data —
//     the project store, the job service and the tests all consult the SAME
//     table, so the layers cannot drift (the same reason Phase 11 keeps one
//     ownership function for store + API + RLS).
//
// DEFENSE IN DEPTH: authorization is checked at the service boundary AND
// again at every mutating operation (the store methods take the AuthContext
// and re-check). A check that exists only at the API edge is one refactor
// away from missing.
//
// Role semantics (the minimal set the service actually enforces):
//   Owner  — the creator. Everything, plus archiving the project. There is
//            exactly one owner (the creator); the owner's role cannot be
//            changed or removed.
//   Admin  — manage members and project quota; cancel any project job.
//   Member — submit jobs, cancel own jobs, view project data, register
//            artifacts.
//   Viewer — read-only: view the project, its members and its jobs.

#include <cstdint>

#include "service/service_status.hpp"

namespace vortyx::service {

enum class ProjectRole : std::uint8_t {
    Owner = 0,
    Admin = 1,
    Member = 2,
    Viewer = 3,
};

const char* to_string(ProjectRole role);

// Parses a stable lowercase name ("owner", "admin", "member", "viewer").
// False for anything else.
bool project_role_from_string(const std::string& name, ProjectRole& out);

// True when 'role' grants at least the powers of 'required'
// (Owner > Admin > Member > Viewer). Pure.
bool project_role_at_least(ProjectRole role, ProjectRole required);

// The actions the service authorizes on a project. The action list is the
// ones the Phase 14 service actually gates — nothing speculative.
enum class ProjectAction : std::uint8_t {
    ViewProject,     // see the project record
    ViewMembers,     // list memberships
    ViewJobs,        // list/view jobs of the project
    ViewUsage,       // see the project's quota usage
    SubmitJob,       // submit a job into the project
    CancelOwnJob,    // cancel a job the caller submitted
    CancelAnyJob,    // cancel any job of the project (Admin+)
    RegisterArtifact, // attach artifact metadata to the project
    ManageMembers,   // add/remove members (Admin+)
    ChangeQuota,     // change the project's quota (Admin+)
    ArchiveProject,  // archive the project (Owner only)
};

const char* to_string(ProjectAction action);

// The pure decision table. ServiceStatus::Ok when the role may perform the
// action, ServiceStatus::Forbidden otherwise. THE one definition — the
// project store and the job service both call this; tests pin the table.
//
//   Viewer : ViewProject, ViewMembers, ViewJobs
//   Member : + ViewUsage, SubmitJob, CancelOwnJob, ViewUsage, RegisterArtifact
//   Admin  : + CancelAnyJob, ManageMembers, ChangeQuota
//   Owner  : + ArchiveProject
ServiceStatus authorize_project_action(ProjectRole role, ProjectAction action);

}  // namespace vortyx::service
