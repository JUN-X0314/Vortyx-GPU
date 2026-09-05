// Service project + authorization tests (Phase 14).
//
// Convention: plain main() + check(), like every other test in this project.
//
// Covered: project creation (id validation, generated ids, name rules),
// role resolution, the pure authz table, membership lifecycle (add/remove
// caps and conflicts), archival semantics, and the security rules —
// anti-enumeration (foreign project = NotFound), IDOR refusal (a known
// foreign project id yields nothing), Unauthenticated refusal.

#include <iostream>
#include <string>
#include <vector>

#include "service/project.hpp"
#include "service/service.hpp"

using namespace vortyx::service;
using vortyx::platform::AuthContext;
using vortyx::platform::make_authenticated;
using SS = ServiceStatus;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_status(SS actual, SS expected, const std::string& message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << " (expected " << to_string(expected) << ", got "
                  << to_string(actual) << ")\n";
        ++failures;
    }
}

}  // namespace

int main() {
    // =====================================================================
    // 1. The pure authorization table (the one definition all layers share)
    // =====================================================================
    {
        check(authorize_project_action(ProjectRole::Viewer, ProjectAction::ViewProject) == SS::Ok,
              "viewer views projects");
        check(authorize_project_action(ProjectRole::Viewer, ProjectAction::SubmitJob) == SS::Forbidden,
              "viewer cannot submit");
        check(authorize_project_action(ProjectRole::Member, ProjectAction::SubmitJob) == SS::Ok,
              "member submits");
        check(authorize_project_action(ProjectRole::Member, ProjectAction::CancelOwnJob) == SS::Ok,
              "member cancels own job");
        check(authorize_project_action(ProjectRole::Member, ProjectAction::CancelAnyJob) ==
                  SS::Forbidden,
              "member cannot cancel any job");
        check(authorize_project_action(ProjectRole::Admin, ProjectAction::CancelAnyJob) == SS::Ok,
              "admin cancels any job");
        check(authorize_project_action(ProjectRole::Admin, ProjectAction::ManageMembers) == SS::Ok,
              "admin manages members");
        check(authorize_project_action(ProjectRole::Admin, ProjectAction::ArchiveProject) ==
                  SS::Forbidden,
              "admin cannot archive (owner only)");
        check(authorize_project_action(ProjectRole::Owner, ProjectAction::ArchiveProject) == SS::Ok,
              "owner archives");
        check(project_role_at_least(ProjectRole::Owner, ProjectRole::Viewer) &&
                  project_role_at_least(ProjectRole::Admin, ProjectRole::Admin) &&
                  !project_role_at_least(ProjectRole::Viewer, ProjectRole::Member),
              "role strength ordering");
        // Stable codes for every action (the observability contract).
        bool codes_stable = true;
        for (const ProjectAction action : {
                 ProjectAction::ViewProject, ProjectAction::ViewMembers, ProjectAction::ViewJobs,
                 ProjectAction::ViewUsage, ProjectAction::SubmitJob, ProjectAction::CancelOwnJob,
                 ProjectAction::CancelAnyJob, ProjectAction::RegisterArtifact,
                 ProjectAction::ManageMembers, ProjectAction::ChangeQuota,
                 ProjectAction::ArchiveProject}) {
            codes_stable = codes_stable && std::string(to_string(action)) != "unknown";
        }
        check(codes_stable, "every action has a stable name");
    }

    // =====================================================================
    // 2. Project creation and validation
    // =====================================================================
    InMemoryProjectStore store;
    const AuthContext alice = make_authenticated("user-alice");
    const AuthContext bob = make_authenticated("user-bob");
    const AuthContext anonymous = vortyx::platform::anonymous();

    ProjectRecord created;
    {
        ProjectRecord request;
        request.name = "alpha";
        check_status(store.create_project(alice, request, created), SS::Ok, "create project");
        check(!created.project_id.empty() && created.owner_user_id == "user-alice",
              "the owner is the authenticated subject (server-managed)");
        check(created.status == ProjectStatus::Active, "new projects are active");

        ProjectRecord named;
        ProjectRecord named_request;
        named_request.project_id = "proj-custom-1";
        named_request.name = "beta";
        check_status(store.create_project(alice, named_request, named), SS::Ok,
                     "create with explicit id");
        check(named.project_id == "proj-custom-1", "explicit id respected");

        // Duplicate id (even by the same user) is a Conflict that never
        // reveals who owns the existing project.
        ProjectRecord dup_request;
        dup_request.project_id = "proj-custom-1";
        dup_request.name = "beta-again";
        ProjectRecord dup;
        check_status(store.create_project(bob, dup_request, dup), SS::Conflict,
                     "duplicate id refused");

        ProjectRecord bad;
        ProjectRecord bad_id;
        bad_id.name = "ok";
        bad_id.project_id = "bad id!";  // space is outside [A-Za-z0-9._-]
        check_status(store.create_project(alice, bad_id, bad), SS::InvalidInput,
                     "invalid id refused");
        ProjectRecord bad_name;
        bad_name.name = "";
        check_status(store.create_project(alice, bad_name, bad), SS::InvalidInput,
                     "empty name refused");
        ProjectRecord long_name;
        long_name.name.assign(kMaxProjectNameLength + 1, 'x');
        check_status(store.create_project(alice, long_name, bad), SS::InvalidInput,
                     "overlong name refused");
        ProjectRecord control_name;
        control_name.name = "line1\nline2";
        check_status(store.create_project(alice, control_name, bad), SS::InvalidInput,
                     "control characters refused");
        ProjectRecord anon;
        check_status(store.create_project(anonymous, request, anon), SS::Unauthenticated,
                     "anonymous creation refused");
    }

    // =====================================================================
    // 3. Visibility: owner sees; members see; the world does not
    //    (anti-enumeration: foreign and unknown are the same NotFound)
    // =====================================================================
    {
        ProjectRecord view;
        check_status(store.project(alice, created.project_id, view), SS::Ok, "owner sees");
        check_status(store.project(bob, created.project_id, view), SS::NotFound,
                     "foreign project is NotFound (before membership)");
        ProjectRecord unknown_view;
        check_status(store.project(bob, "proj-does-not-exist", unknown_view), SS::NotFound,
                     "unknown project is NotFound (same outcome)");
        ProjectRecord anon_view;
        check_status(store.project(anonymous, created.project_id, anon_view), SS::Unauthenticated,
                     "anonymous lookup refused");
    }

    // =====================================================================
    // 4. Membership lifecycle and its authorization
    // =====================================================================
    {
        ProjectMember member;
        check_status(store.add_member(alice, created.project_id, "user-bob", ProjectRole::Member,
                                      member),
                     SS::Ok, "owner adds bob as member");
        check(member.role == ProjectRole::Member && member.user_id == "user-bob",
              "membership recorded");

        // A member (bob) cannot manage members — only Admin+ can.
        ProjectMember self_promote;
        check_status(store.add_member(bob, created.project_id, "user-carol",
                                      ProjectRole::Viewer, self_promote),
                     SS::Forbidden, "member cannot add members");

        // Duplicate membership is a Conflict; the owner cannot be re-added.
        ProjectMember again;
        check_status(store.add_member(alice, created.project_id, "user-bob", ProjectRole::Admin,
                                      again),
                     SS::Conflict, "duplicate membership refused");
        check_status(store.add_member(alice, created.project_id, "user-alice",
                                      ProjectRole::Owner, again),
                     SS::Conflict, "owner re-add refused");

        // Promote bob to admin, then bob manages members.
        // (add_member of an existing member is a Conflict, so removal first.)
        ProjectMember removed_check;
        check_status(store.remove_member(alice, created.project_id, "user-bob"), SS::Ok,
                     "owner removes bob");
        ProjectMember admin_member;
        check_status(store.add_member(alice, created.project_id, "user-bob", ProjectRole::Admin,
                                      admin_member),
                     SS::Ok, "owner adds bob as admin");
        ProjectMember carol;
        check_status(store.add_member(bob, created.project_id, "user-carol", ProjectRole::Viewer,
                                      carol),
                     SS::Ok, "admin adds a viewer");

        // The owner cannot be removed (even by the owner).
        check_status(store.remove_member(alice, created.project_id, "user-alice"),
                     SS::InvalidInput, "owner removal refused");
        // Removing a non-member is NotFound.
        check_status(store.remove_member(alice, created.project_id, "user-nobody"), SS::NotFound,
                     "unknown member NotFound");

        std::vector<ProjectMember> list;
        check_status(store.members(alice, created.project_id, list), SS::Ok, "members listed");
        check(list.size() == 3, "owner + admin + viewer");
        check_status(store.members(bob, created.project_id, list), SS::Ok,
                     "members visible to members");
        check(list.size() == 3, "same membership list");

        // Role resolution: bob (admin), carol (viewer), dave (nothing).
        ProjectRole role;
        check_status(store.role_of(bob, created.project_id, role), SS::Ok, "role resolves");
        check(role == ProjectRole::Admin, "bob is admin");
        check_status(store.role_of(make_authenticated("user-dave"), created.project_id, role),
                     SS::NotFound, "non-member role is NotFound (invisible)");
    }

    // =====================================================================
    // 5. Archival: owner-only, refused twice, membership still readable
    // =====================================================================
    {
        ProjectRecord archived;
        check_status(store.archive_project(bob, created.project_id, archived), SS::Forbidden,
                     "non-owner cannot archive");
        check_status(store.archive_project(alice, created.project_id, archived), SS::Ok,
                     "owner archives");
        check(archived.status == ProjectStatus::Archived, "status archived");
        check_status(store.archive_project(alice, created.project_id, archived),
                     SS::InvalidInput, "double archive refused");
        ProjectRecord view;
        check_status(store.project(alice, created.project_id, view), SS::Ok,
                     "archived project still viewable");
    }

    // =====================================================================
    // 6. Listing: owned + member projects in creation order
    // =====================================================================
    {
        std::vector<ProjectRecord> alice_list;
        check_status(store.projects(alice, alice_list), SS::Ok, "alice lists");
        check(alice_list.size() == 2, "alice owns two projects");
        std::vector<ProjectRecord> bob_list;
        check_status(store.projects(bob, bob_list), SS::Ok, "bob lists");
        check(bob_list.size() == 1 && bob_list[0].project_id == created.project_id,
              "bob sees the project he is a member of");
        std::vector<ProjectRecord> anon_list;
        check_status(store.projects(anonymous, anon_list), SS::Unauthenticated,
                     "anonymous listing refused");
    }

    // =====================================================================
    // 7. IDOR simulation through the store: a known foreign id yields
    //    nothing (no read, no mutation, no existence signal)
    // =====================================================================
    {
        ProjectRecord foreign;
        check_status(store.project(bob, "proj-custom-1", foreign), SS::NotFound,
                     "IDOR read: foreign project invisible");
        ProjectMember foreign_member;
        check_status(store.add_member(bob, "proj-custom-1", "user-bob", ProjectRole::Admin,
                                      foreign_member),
                     SS::NotFound, "IDOR mutation: foreign membership invisible");
        check_status(store.remove_member(bob, "proj-custom-1", "user-alice"), SS::NotFound,
                     "IDOR removal: foreign project invisible");
    }

    if (failures == 0) {
        std::cout << "Service project tests passed.\n";
        return 0;
    }
    std::cerr << failures << " service project test(s) failed.\n";
    return 1;
}
