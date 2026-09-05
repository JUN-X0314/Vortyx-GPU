// Project model and provider-neutral project store (Phase 14) —
// implementation.

#include "service/project.hpp"

#include <cstdio>
#include <random>

#include "platform/identity.hpp"

namespace vortyx::service {

const char* to_string(ProjectStatus status) {
    switch (status) {
        case ProjectStatus::Active: return "active";
        case ProjectStatus::Archived: return "archived";
    }
    return "unknown";
}

ProjectId generate_project_id() {
    // The SAME UUID-v4 construction the platform identity generators use
    // (same format, same entropy source): a uniqueness label, not a security
    // claim. Not imported because platform exposes only the named
    // generators (device/job); the format is pinned by the same tests.
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uint64_t hi = rng();
    std::uint64_t lo = rng();
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // RFC 4122 variant

    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>((hi >> 32) & 0xFFFFFFFFULL),
                  static_cast<unsigned>((hi >> 16) & 0xFFFFULL),
                  static_cast<unsigned>(hi & 0xFFFFULL),
                  static_cast<unsigned>((lo >> 48) & 0xFFFFULL),
                  static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
    return ProjectId(buf);
}

namespace {

std::int64_t now_ms(const std::shared_ptr<vortyx::distributed::IClock>& clock) {
    return clock ? clock->now_ms() : 0;
}

bool name_valid(const std::string& name, std::string& error) {
    if (name.empty() || name.size() > kMaxProjectNameLength) {
        error = "project name must be 1.." + std::to_string(kMaxProjectNameLength) + " bytes";
        return false;
    }
    for (const char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) {
            error = "project name must not contain control characters";
            return false;
        }
    }
    return true;
}

}  // namespace

ServiceStatus validate_project_record(const ProjectRecord& record, std::string& error) {
    if (const vortyx::platform::Status status =
            vortyx::platform::validate_id("project_id", record.project_id, error);
        status != vortyx::platform::Status::Ok) {
        return ServiceStatus::InvalidInput;
    }
    if (!name_valid(record.name, error)) return ServiceStatus::InvalidInput;
    return ServiceStatus::Ok;
}

// ---------------------------------------------------------------------------
// InMemoryProjectStore
// ---------------------------------------------------------------------------

ServiceStatus InMemoryProjectStore::resolve_visible(const vortyx::platform::AuthContext& auth,
                                                    const ProjectId& project_id,
                                                    ProjectRecord& out_project,
                                                    ProjectRole& out_role) {
    std::string error;
    if (const vortyx::platform::Status status =
            vortyx::platform::validate_auth(auth, error);
        status != vortyx::platform::Status::Ok) {
        return ServiceStatus::Unauthenticated;
    }
    for (const ProjectRecord& project : projects_) {
        if (project.project_id != project_id) continue;
        if (vortyx::platform::is_owner(auth, project.owner_user_id)) {
            out_project = project;
            out_role = ProjectRole::Owner;
            return ServiceStatus::Ok;
        }
        for (const auto& entry : members_) {
            if (entry.first != project_id) continue;
            for (const ProjectMember& member : entry.second) {
                if (member.user_id != auth.user_id) continue;
                out_project = project;
                out_role = member.role;
                return ServiceStatus::Ok;
            }
        }
        // Authenticated but not affiliated: invisible (anti-enumeration).
        return ServiceStatus::NotFound;
    }
    return ServiceStatus::NotFound;
}

ServiceStatus InMemoryProjectStore::create_project(const vortyx::platform::AuthContext& auth,
                                                   const ProjectRecord& record,
                                                   ProjectRecord& out) {
    std::string error;
    if (const vortyx::platform::Status status =
            vortyx::platform::validate_auth(auth, error);
        status != vortyx::platform::Status::Ok) {
        return ServiceStatus::Unauthenticated;
    }
    if (!name_valid(record.name, error)) return ServiceStatus::InvalidInput;

    ProjectRecord created = record;
    created.project_id = record.project_id.empty() ? generate_project_id() : record.project_id;
    if (const vortyx::platform::Status status =
            vortyx::platform::validate_id("project_id", created.project_id, error);
        status != vortyx::platform::Status::Ok) {
        return ServiceStatus::InvalidInput;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (const ProjectRecord& existing : projects_) {
        if (existing.project_id == created.project_id) {
            error = "project id already exists (never revealing whose it is)";
            return ServiceStatus::Conflict;
        }
    }
    const std::int64_t stamp = now_ms(clock_);
    created.owner_user_id = auth.user_id;
    created.status = ProjectStatus::Active;
    created.created_at_ms = stamp;
    created.updated_at_ms = stamp;
    projects_.push_back(created);

    ProjectMember owner;
    owner.project_id = created.project_id;
    owner.user_id = auth.user_id;
    owner.role = ProjectRole::Owner;
    owner.created_at_ms = stamp;
    members_.emplace_back(created.project_id, std::vector<ProjectMember>{owner});

    out = created;
    return ServiceStatus::Ok;
}

ServiceStatus InMemoryProjectStore::project(const vortyx::platform::AuthContext& auth,
                                            const ProjectId& project_id, ProjectRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    ProjectRole role = ProjectRole::Viewer;
    return resolve_visible(auth, project_id, out, role);
}

ServiceStatus InMemoryProjectStore::projects(const vortyx::platform::AuthContext& auth,
                                             std::vector<ProjectRecord>& out) {
    std::string error;
    if (const vortyx::platform::Status status =
            vortyx::platform::validate_auth(auth, error);
        status != vortyx::platform::Status::Ok) {
        return ServiceStatus::Unauthenticated;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    out.clear();
    for (const ProjectRecord& project : projects_) {
        if (vortyx::platform::is_owner(auth, project.owner_user_id)) {
            out.push_back(project);
            continue;
        }
        for (const auto& entry : members_) {
            if (entry.first != project.project_id) continue;
            for (const ProjectMember& member : entry.second) {
                if (member.user_id == auth.user_id) out.push_back(project);
            }
        }
    }
    return ServiceStatus::Ok;
}

ServiceStatus InMemoryProjectStore::archive_project(const vortyx::platform::AuthContext& auth,
                                                    const ProjectId& project_id,
                                                    ProjectRecord& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    ProjectRole role = ProjectRole::Viewer;
    ProjectRecord record;
    ServiceStatus status = resolve_visible(auth, project_id, record, role);
    if (status != ServiceStatus::Ok) return status;
    if (authorize_project_action(role, ProjectAction::ArchiveProject) != ServiceStatus::Ok) {
        return ServiceStatus::Forbidden;
    }
    if (record.status == ProjectStatus::Archived) {
        return ServiceStatus::InvalidInput;
    }
    for (ProjectRecord& project : projects_) {
        if (project.project_id == project_id) {
            project.status = ProjectStatus::Archived;
            project.updated_at_ms = now_ms(clock_);
            out = project;
            return ServiceStatus::Ok;
        }
    }
    return ServiceStatus::Internal;
}

ServiceStatus InMemoryProjectStore::add_member(const vortyx::platform::AuthContext& auth,
                                               const ProjectId& project_id,
                                               const vortyx::platform::UserId& user_id,
                                               ProjectRole role, ProjectMember& out) {
    std::string error;
    if (const vortyx::platform::Status status =
            vortyx::platform::validate_auth(auth, error);
        status != vortyx::platform::Status::Ok) {
        return ServiceStatus::Unauthenticated;
    }
    if (const vortyx::platform::Status status =
            vortyx::platform::validate_id("user_id", user_id, error);
        status != vortyx::platform::Status::Ok) {
        return ServiceStatus::InvalidInput;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ProjectRole caller_role = ProjectRole::Viewer;
    ProjectRecord record;
    ServiceStatus status = resolve_visible(auth, project_id, record, caller_role);
    if (status != ServiceStatus::Ok) return status;
    if (authorize_project_action(caller_role, ProjectAction::ManageMembers) !=
        ServiceStatus::Ok) {
        return ServiceStatus::Forbidden;
    }
    for (auto& entry : members_) {
        if (entry.first != project_id) continue;
        for (const ProjectMember& member : entry.second) {
            if (member.user_id == user_id) {
                error = member.role == ProjectRole::Owner
                            ? "the owner cannot be re-added"
                            : "user is already a member";
                return ServiceStatus::Conflict;
            }
        }
        if (entry.second.size() >= kMaxProjectMembers) {
            error = "project membership is at capacity (" +
                    std::to_string(kMaxProjectMembers) + ")";
            return ServiceStatus::InvalidInput;
        }
        ProjectMember added;
        added.project_id = project_id;
        added.user_id = user_id;
        added.role = role;
        added.created_at_ms = now_ms(clock_);
        entry.second.push_back(added);
        out = added;
        return ServiceStatus::Ok;
    }
    return ServiceStatus::Internal;
}

ServiceStatus InMemoryProjectStore::remove_member(const vortyx::platform::AuthContext& auth,
                                                  const ProjectId& project_id,
                                                  const vortyx::platform::UserId& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ProjectRole caller_role = ProjectRole::Viewer;
    ProjectRecord record;
    ServiceStatus status = resolve_visible(auth, project_id, record, caller_role);
    if (status != ServiceStatus::Ok) return status;
    if (authorize_project_action(caller_role, ProjectAction::ManageMembers) !=
        ServiceStatus::Ok) {
        return ServiceStatus::Forbidden;
    }
    for (auto& entry : members_) {
        if (entry.first != project_id) continue;
        auto& list = entry.second;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->user_id != user_id) continue;
            if (it->role == ProjectRole::Owner) {
                return ServiceStatus::InvalidInput;  // the owner cannot leave
            }
            list.erase(it);
            return ServiceStatus::Ok;
        }
        return ServiceStatus::NotFound;
    }
    return ServiceStatus::Internal;
}

ServiceStatus InMemoryProjectStore::members(const vortyx::platform::AuthContext& auth,
                                            const ProjectId& project_id,
                                            std::vector<ProjectMember>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    ProjectRole caller_role = ProjectRole::Viewer;
    ProjectRecord record;
    ServiceStatus status = resolve_visible(auth, project_id, record, caller_role);
    if (status != ServiceStatus::Ok) return status;
    if (authorize_project_action(caller_role, ProjectAction::ViewMembers) != ServiceStatus::Ok) {
        return ServiceStatus::Forbidden;
    }
    for (const auto& entry : members_) {
        if (entry.first == project_id) {
            out = entry.second;
            return ServiceStatus::Ok;
        }
    }
    return ServiceStatus::Internal;
}

ServiceStatus InMemoryProjectStore::role_of(const vortyx::platform::AuthContext& auth,
                                            const ProjectId& project_id, ProjectRole& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    ProjectRecord record;
    return resolve_visible(auth, project_id, record, out);
}

}  // namespace vortyx::service
