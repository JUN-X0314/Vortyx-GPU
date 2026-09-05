// Service layer types (Phase 15) — the TypeScript mirror of
// src/service/{service_status,project,quota,artifact,audit,platform_service}.hpp.
//
// The service control plane (projects, memberships, jobs, quota, artifacts,
// audit) is implemented twice on purpose, exactly like the Phase 11
// contract: once in C++ (the local in-process service) and once here (the
// Vercel/local HTTP control plane). The vocabularies below mirror the C++
// ones field for field; tests on both sides pin them so the layers cannot
// drift.
//
// Timestamps are epoch milliseconds; `null` means "not set" — never a
// fabricated 0 (the project-wide honesty rule).

/** Service outcome vocabulary (mirror of vortyx::service::ServiceStatus). */
export type ServiceStatus =
  | "ok"
  | "invalid_input"
  | "unauthenticated"
  | "forbidden"
  | "not_found"
  | "conflict"
  | "quota_exceeded"
  | "rate_limit_exceeded"
  | "unsupported_operation"
  | "unavailable"
  | "internal";

/**
 * The documented HTTP mapping (mirror of service_status_http) — the C++
 * service contract pins it; tests on both sides agree.
 */
export function serviceHttpStatus(status: ServiceStatus, invalidJson = false): number {
  switch (status) {
    case "ok":
      return 200;
    case "invalid_input":
      return invalidJson ? 400 : 422;
    case "unauthenticated":
      return 401;
    case "forbidden":
      return 403;
    case "not_found":
      return 404;
    case "conflict":
      return 409;
    case "quota_exceeded":
      return 429;
    case "rate_limit_exceeded":
      return 429;
    case "unsupported_operation":
      return 422;
    case "unavailable":
      return 503;
    case "internal":
      return 500;
  }
}

/** Project lifecycle (mirror of ProjectStatus). */
export type ProjectStatus = "active" | "archived";

/** Project roles (mirror of ProjectRole). The Owner role is NEVER grantable. */
export type ProjectRole = "owner" | "admin" | "member" | "viewer";

export interface ProjectRecord {
  project_id: string;
  owner_user_id: string;
  name: string;
  status: ProjectStatus;
  created_at_ms: number;
  updated_at_ms: number;
}

export interface ProjectMember {
  project_id: string;
  user_id: string;
  role: ProjectRole;
  created_at_ms: number;
}

/** Service job states — the service view of the Phase 12 vocabulary. */
export type ServiceJobStatus = "queued" | "running" | "completed" | "failed" | "cancelled";

export interface ServiceJobRecord {
  job_id: string;
  project_id: string;
  submitted_by: string;
  operation: string;
  element_count: number;
  requested_backend: string; // "" = no preference
  requested_shard_count: number;
  status: ServiceJobStatus;
  error: string;
  submitted_at_ms: number;
  terminal_at_ms: number | null;
  // Honest execution summary (null until the worker reports it).
  total_shards: number | null;
  succeeded_shards: number | null;
  failed_shards: number | null;
  result_element_count: number | null;
  result_backend: string | null;
  // Worker coordination (claim/lease — the durable queue state).
  attempt: number;
  claimed_by: string | null;
  claim_expires_at_ms: number | null;
  cancel_requested: boolean;
}

export interface ProjectQuota {
  max_concurrent_jobs: number;
  max_running_shards: number;
  max_memory_bytes: number;
}

/** The live usage of one project — derived from in-flight jobs. */
export interface QuotaUsage {
  active_jobs: number;
  running_shards: number;
  reserved_memory_bytes: number;
}

export interface ArtifactMetadata {
  artifact_id: string;
  project_id: string;
  name: string;
  created_by: string;
  declared_byte_size: number;
  created_at_ms: number;
}

export type AuditAction =
  | "auth_signup"
  | "project_create"
  | "project_archive"
  | "membership_change"
  | "job_submit"
  | "job_cancel"
  | "job_terminal"
  | "quota_change"
  | "artifact_register"
  | "artifact_delete";

export type AuditOutcome = "ok" | "denied" | "error";

export interface AuditEvent {
  event_id: string;
  timestamp_ms: number;
  actor_user_id: string;
  project_id: string;
  job_id: string;
  action: AuditAction;
  outcome: AuditOutcome;
  reason_code: string;
}

/** The claimed job the worker protocol returns (mirror of ClaimedJob). */
export interface WorkerClaimedJob {
  job_id: string;
  project_id: string;
  operation: string;
  element_count: number;
  requested_backend: string;
  requested_shard_count: number;
  attempt: number;
  lease_expires_at_ms: number;
}

/** The worker's terminal report (mirror of CompletionReport). */
export interface WorkerCompletionReport {
  status: "completed" | "failed" | "cancelled";
  error: string;
  backend: string;
  result_element_count: number | null;
  shards_total?: number;
  shards_succeeded?: number;
  shards_failed?: number;
}

// Shared policy constants (documented mirrors of the C++ service).
export const MAX_JOB_ELEMENT_COUNT = 2147483647;
export const MAX_REQUESTED_SHARD_COUNT = 64;
export const MAX_PAGE_LIMIT = 100;
export const DEFAULT_PAGE_LIMIT = 20;
export const MAX_ARTIFACTS_PER_PROJECT = 256;
export const DEFAULT_QUOTA: ProjectQuota = {
  max_concurrent_jobs: 4,
  max_running_shards: 16,
  max_memory_bytes: 1024 * 1024 * 1024,
};

/**
 * The whole-job memory estimate — the exact TS mirror of
 * vortyx::distributed::shard_memory_bytes (4 bytes per int32 element;
 * 3 resident buffers for two-input ops, 2 for VectorScale). The quota
 * policy consumes this; there is no other memory accounting.
 */
export function jobMemoryBytes(elementCount: number, operation: string): number | null {
  const bufferCount = operation === "vector_scale" ? 2 : operation === "vector_add" || operation === "vector_multiply" ? 3 : null;
  if (bufferCount === null) return null;
  const perElement = 4 * bufferCount;
  const max = Math.floor(Number.MAX_SAFE_INTEGER / perElement);
  if (elementCount > max) return null;
  return elementCount * perElement;
}
