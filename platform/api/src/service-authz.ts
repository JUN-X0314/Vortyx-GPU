// Project authorization table (Phase 15) — the TypeScript mirror of
// src/service/authz.hpp.
//
// THE ONE authorization table for the service control plane: the API
// router, the service stores (memory + Supabase) and the tests all consult
// THE SAME functions, so the layers cannot drift (the same rule the Phase
// 11 ownership functions established).
//
// Role semantics (mirror of the C++ header):
//   Owner  — the creator; everything, plus archive. There is EXACTLY ONE
//            owner per project (the creator). The Owner role is never
//            grantable through any membership path (single-owner
//            invariant); no ownership transfer exists yet.
//   Admin  — manage members/quota, cancel any project job (through the
//            audited privileged cancellation path).
//   Member — submit jobs, cancel own jobs, register/delete own artifacts.
//   Viewer — read-only.

import type { ProjectRole, ServiceStatus } from "./service-types.ts";

export type ServiceAction =
  | "view_project"
  | "view_members"
  | "view_jobs"
  | "view_usage"
  | "view_audit"
  | "submit_job"
  | "cancel_own_job"
  | "cancel_any_job"
  | "register_artifact"
  | "delete_artifact"
  | "manage_members"
  | "change_quota"
  | "archive_project";

const ROLE_STRENGTH: Record<ProjectRole, number> = {
  owner: 0,
  admin: 1,
  member: 2,
  viewer: 3,
};

/** True when 'role' carries at least the powers of 'required'. Pure. */
export function roleAtLeast(role: ProjectRole, required: ProjectRole): boolean {
  return ROLE_STRENGTH[role] <= ROLE_STRENGTH[required];
}

/** The single-owner invariant as a pure rule: Owner is never grantable. */
export function projectRoleGrantable(role: ProjectRole): boolean {
  return role !== "owner";
}

/**
 * The pure decision table (mirror of authorize_project_action). "ok" when
 * the role may perform the action; the refusing ServiceStatus otherwise.
 */
export function authorizeProjectAction(role: ProjectRole, action: ServiceAction): ServiceStatus {
  switch (action) {
    case "view_project":
    case "view_members":
    case "view_jobs":
      return roleAtLeast(role, "viewer") ? "ok" : "forbidden";
    case "view_usage":
    case "submit_job":
    case "cancel_own_job":
    case "register_artifact":
      return roleAtLeast(role, "member") ? "ok" : "forbidden";
    case "view_audit":
    case "cancel_any_job":
    case "delete_artifact":
    case "manage_members":
    case "change_quota":
      return roleAtLeast(role, "admin") ? "ok" : "forbidden";
    case "archive_project":
      return role === "owner" ? "ok" : "forbidden";
  }
}
