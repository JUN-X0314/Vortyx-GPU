// Project authorization model (Phase 14) — implementation.

#include "service/authz.hpp"

namespace vortyx::service {

const char* to_string(ProjectRole role) {
    switch (role) {
        case ProjectRole::Owner: return "owner";
        case ProjectRole::Admin: return "admin";
        case ProjectRole::Member: return "member";
        case ProjectRole::Viewer: return "viewer";
    }
    return "unknown";
}

bool project_role_from_string(const std::string& name, ProjectRole& out) {
    if (name == "owner") { out = ProjectRole::Owner; return true; }
    if (name == "admin") { out = ProjectRole::Admin; return true; }
    if (name == "member") { out = ProjectRole::Member; return true; }
    if (name == "viewer") { out = ProjectRole::Viewer; return true; }
    return false;
}

bool project_role_at_least(ProjectRole role, ProjectRole required) {
    // Lower enum value = stronger role (see the enum declaration).
    return static_cast<std::uint8_t>(role) <= static_cast<std::uint8_t>(required);
}

const char* to_string(ProjectAction action) {
    switch (action) {
        case ProjectAction::ViewProject: return "view_project";
        case ProjectAction::ViewMembers: return "view_members";
        case ProjectAction::ViewJobs: return "view_jobs";
        case ProjectAction::ViewUsage: return "view_usage";
        case ProjectAction::SubmitJob: return "submit_job";
        case ProjectAction::CancelOwnJob: return "cancel_own_job";
        case ProjectAction::CancelAnyJob: return "cancel_any_job";
        case ProjectAction::RegisterArtifact: return "register_artifact";
        case ProjectAction::DeleteArtifact: return "delete_artifact";
        case ProjectAction::ManageMembers: return "manage_members";
        case ProjectAction::ChangeQuota: return "change_quota";
        case ProjectAction::ArchiveProject: return "archive_project";
    }
    return "unknown";
}

ServiceStatus authorize_project_action(ProjectRole role, ProjectAction action) {
    // The table, one row per role (see the module header). Member is the
    // pivot: everything a Member may do, a stronger role may do too — the
    // strength comparison keeps the table additive and impossible to get
    // half-right.
    switch (action) {
        case ProjectAction::ViewProject:
        case ProjectAction::ViewMembers:
        case ProjectAction::ViewJobs:
            return project_role_at_least(role, ProjectRole::Viewer)
                       ? ServiceStatus::Ok
                       : ServiceStatus::Forbidden;

        case ProjectAction::ViewUsage:
        case ProjectAction::SubmitJob:
        case ProjectAction::CancelOwnJob:
        case ProjectAction::RegisterArtifact:
            return project_role_at_least(role, ProjectRole::Member)
                       ? ServiceStatus::Ok
                       : ServiceStatus::Forbidden;

        case ProjectAction::CancelAnyJob:
        case ProjectAction::DeleteArtifact:
        case ProjectAction::ManageMembers:
        case ProjectAction::ChangeQuota:
            return project_role_at_least(role, ProjectRole::Admin)
                       ? ServiceStatus::Ok
                       : ServiceStatus::Forbidden;

        case ProjectAction::ArchiveProject:
            return role == ProjectRole::Owner ? ServiceStatus::Ok : ServiceStatus::Forbidden;
    }
    return ServiceStatus::Forbidden;
}

bool project_role_grantable(ProjectRole role) {
    // The single-owner invariant: ownership is minted exactly once, by
    // project creation. No add-member path may grant it again.
    return role != ProjectRole::Owner;
}

}  // namespace vortyx::service
