// Service API contract (Phase 15) — request parsing / validation and the
// unified error body, in the EXACT style of contract.ts (the Phase 11
// contract module).
//
// Validation is SERVER-side and complete: the web console validates too,
// but browser validation is a convenience — never the enforcement point.
// Every refusal names a stable error code; the shapes mirror the C++
// service contract's vocabulary field for field.

import { isValidId } from "./ids.ts";
import {
  MAX_ARTIFACTS_PER_PROJECT,
  MAX_JOB_ELEMENT_COUNT,
  MAX_PAGE_LIMIT,
  MAX_REQUESTED_SHARD_COUNT,
  type ArtifactMetadata,
  type AuditEvent,
  type MetricsSummary,
  type ProjectMember,
  type ProjectQuota,
  type ProjectRecord,
  type QuotaUsage,
  type ServiceJobRecord,
  type WorkerClaimedJob,
} from "./service-types.ts";

// Reuse the Phase 11 error-code vocabulary (one error model everywhere).
export {
  ERR_CONFLICT,
  ERR_FORBIDDEN,
  ERR_INTERNAL,
  ERR_INVALID_ENUM,
  ERR_INVALID_ID,
  ERR_INVALID_JSON,
  ERR_INVALID_TYPE,
  ERR_INVALID_VALUE,
  ERR_METHOD_NOT_ALLOWED,
  ERR_MISSING_FIELD,
  ERR_NOT_FOUND,
  ERR_UNAUTHENTICATED,
} from "./contract.ts";

import { errorBody } from "./contract.ts";

export interface ParseOk<T> {
  ok: true;
  value: T;
}

export interface ParseFailure {
  ok: false;
  code: string;
  message: string;
}

export type ParseResult<T> = ParseOk<T> | ParseFailure;

function fail(code: string, message: string): ParseFailure {
  return { ok: false, code, message };
}

function asObject(value: unknown): ParseResult<Record<string, unknown>> {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    return fail("invalid_type", "request body must be a JSON object");
  }
  return { ok: true, value: value as Record<string, unknown> };
}

function stringField(object: Record<string, unknown>, key: string, max: number, required = true): ParseResult<string> {
  const raw = object[key];
  if (raw === undefined) {
    return required ? fail("missing_field", `${key} is required`) : { ok: true, value: "" };
  }
  if (typeof raw !== "string") return fail("invalid_type", `${key} must be a string`);
  if (raw.length === 0 && required) return fail("invalid_value", `${key} must not be empty`);
  if (raw.length > max) return fail("invalid_value", `${key} must be at most ${max} bytes`);
  // Control characters never enter the control plane (JSON/log safety).
  for (const c of raw) {
    const codePoint = c.codePointAt(0) ?? 0;
    if (codePoint < 0x20 || codePoint === 0x7f) {
      return fail("invalid_value", `${key} must not contain control characters`);
    }
  }
  return { ok: true, value: raw };
}

function integerField(object: Record<string, unknown>, key: string, min: number, max: number): ParseResult<number> {
  const raw = object[key];
  if (typeof raw !== "number" || !Number.isInteger(raw)) {
    return fail("invalid_type", `${key} must be an integer`);
  }
  if (raw < min || raw > max) {
    return fail("invalid_value", `${key} must be ${min}..${max}`);
  }
  return { ok: true, value: raw };
}

// ---------------------------------------------------------------------------
// Request parsers
// ---------------------------------------------------------------------------

const KNOWN_OPERATIONS = ["vector_add", "vector_multiply", "vector_scale"];
const KNOWN_BACKENDS = ["", "cpu", "vulkan"];
const KNOWN_ROLES = ["admin", "member", "viewer"]; // owner is NEVER grantable

export interface ParsedProjectCreate {
  name: string;
}

export function parseProjectCreate(body: unknown): ParseResult<ParsedProjectCreate> {
  const object = asObject(body);
  if (!object.ok) return object;
  const name = stringField(object.value, "name", 128);
  if (!name.ok) return name;
  return { ok: true, value: { name: name.value } };
}

export interface ParsedMemberAdd {
  user_id: string;
  role: string;
}

export function parseMemberAdd(body: unknown): ParseResult<ParsedMemberAdd> {
  const object = asObject(body);
  if (!object.ok) return object;
  const userId = stringField(object.value, "user_id", 128);
  if (!userId.ok) return userId;
  const role = stringField(object.value, "role", 16);
  if (!role.ok) return role;
  if (!KNOWN_ROLES.includes(role.value)) {
    return fail("invalid_enum", "role must be one of admin | member | viewer (owner is never grantable)");
  }
  return { ok: true, value: { user_id: userId.value, role: role.value } };
}

export interface ParsedSubmitJob {
  job_id: string;
  operation: string;
  element_count: number;
  requested_backend: string;
  requested_shard_count: number;
}

export function parseSubmitJob(body: unknown): ParseResult<ParsedSubmitJob> {
  const object = asObject(body);
  if (!object.ok) return object;
  const jobId = stringField(object.value, "job_id", 128);
  if (!jobId.ok) return jobId;
  if (!isValidId(jobId.value)) {
    return fail("invalid_id", "job_id must match ^[A-Za-z0-9._-]+$ (1..128 chars)");
  }
  const operation = stringField(object.value, "operation", 32);
  if (!operation.ok) return operation;
  if (!KNOWN_OPERATIONS.includes(operation.value)) {
    return fail("invalid_enum", `operation must be one of ${KNOWN_OPERATIONS.join(" | ")}`);
  }
  const elementCount = integerField(object.value, "element_count", 1, MAX_JOB_ELEMENT_COUNT);
  if (!elementCount.ok) return elementCount;
  const backend = stringField(object.value, "requested_backend", 16, false);
  if (!backend.ok) return backend;
  if (!KNOWN_BACKENDS.includes(backend.value)) {
    return fail("invalid_enum", "requested_backend must be one of '' | cpu | vulkan");
  }
  let shardCount = 1;
  const rawShards = object.value["requested_shard_count"];
  if (rawShards !== undefined) {
    const parsed = integerField(object.value, "requested_shard_count", 1, MAX_REQUESTED_SHARD_COUNT);
    if (!parsed.ok) return parsed;
    shardCount = parsed.value;
  }
  return {
    ok: true,
    value: {
      job_id: jobId.value,
      operation: operation.value,
      element_count: elementCount.value,
      requested_backend: backend.value,
      requested_shard_count: shardCount,
    },
  };
}

export interface ParsedArtifactRegister {
  name: string;
  declared_byte_size: number;
}

export function parseArtifactRegister(body: unknown): ParseResult<ParsedArtifactRegister> {
  const object = asObject(body);
  if (!object.ok) return object;
  const name = stringField(object.value, "name", 128);
  if (!name.ok) return name;
  const declaredSize = integerField(object.value, "declared_byte_size", 0, Number.MAX_SAFE_INTEGER);
  if (!declaredSize.ok) return declaredSize;
  return { ok: true, value: { name: name.value, declared_byte_size: declaredSize.value } };
}

export function parseQuota(body: unknown): ParseResult<ProjectQuota> {
  const object = asObject(body);
  if (!object.ok) return object;
  const jobs = integerField(object.value, "max_concurrent_jobs", 0, 100000);
  if (!jobs.ok) return jobs;
  const shards = integerField(object.value, "max_running_shards", 0, 100000);
  if (!shards.ok) return shards;
  const memory = integerField(object.value, "max_memory_bytes", 0, Number.MAX_SAFE_INTEGER);
  if (!memory.ok) return memory;
  return {
    ok: true,
    value: { max_concurrent_jobs: jobs.value, max_running_shards: shards.value, max_memory_bytes: memory.value },
  };
}

export interface ParsedPage {
  limit: number;
  offset: number;
}

/** Query-string pagination with hard bounds (resource-exhaustion guard). */
export function parsePage(query: Record<string, string>): ParsedPage {
  const rawLimit = query["limit"] ?? "";
  const rawOffset = query["offset"] ?? "";
  let limit = 20;
  let offset = 0;
  if (rawLimit.length > 0 && /^\d+$/.test(rawLimit)) {
    limit = Math.min(Math.max(parseInt(rawLimit, 10), 1), MAX_PAGE_LIMIT);
  }
  if (rawOffset.length > 0 && /^\d+$/.test(rawOffset)) {
    offset = Math.min(parseInt(rawOffset, 10), 1000000);
  }
  return { limit, offset };
}

// ---------------------------------------------------------------------------
// Worker protocol parsers (the C++ worker is the client; the same strictness)
// ---------------------------------------------------------------------------

export interface ParsedWorkerClaim {
  worker_id: string;
  lease_ms: number;
}

export function parseWorkerClaim(body: unknown): ParseResult<ParsedWorkerClaim> {
  const object = asObject(body);
  if (!object.ok) return object;
  const workerId = stringField(object.value, "worker_id", 128);
  if (!workerId.ok) return workerId;
  const leaseMs = integerField(object.value, "lease_ms", 1000, 600000);
  if (!leaseMs.ok) return leaseMs;
  return { ok: true, value: { worker_id: workerId.value, lease_ms: leaseMs.value } };
}

export interface ParsedWorkerHeartbeat {
  worker_id: string;
}

export function parseWorkerHeartbeat(body: unknown): ParseResult<ParsedWorkerHeartbeat> {
  const object = asObject(body);
  if (!object.ok) return object;
  const workerId = stringField(object.value, "worker_id", 128);
  if (!workerId.ok) return workerId;
  return { ok: true, value: { worker_id: workerId.value } };
}

export interface ParsedWorkerComplete {
  worker_id: string;
  status: "completed" | "failed" | "cancelled";
  error: string;
  backend: string;
  result_element_count: number | null;
  shards_total: number | null;
  shards_succeeded: number | null;
  shards_failed: number | null;
}

export function parseWorkerComplete(body: unknown): ParseResult<ParsedWorkerComplete> {
  const object = asObject(body);
  if (!object.ok) return object;
  const workerId = stringField(object.value, "worker_id", 128);
  if (!workerId.ok) return workerId;
  const status = stringField(object.value, "status", 16);
  if (!status.ok) return status;
  if (status.value !== "completed" && status.value !== "failed" && status.value !== "cancelled") {
    return fail("invalid_enum", "status must be completed | failed | cancelled");
  }
  const error = stringField(object.value, "error", 1024, false);
  if (!error.ok) return error;
  if ((status.value === "failed" || status.value === "cancelled") && error.value.length === 0) {
    return fail("invalid_value", "a failed/cancelled report requires its reason");
  }
  const backend = stringField(object.value, "backend", 32, false);
  if (!backend.ok) return backend;
  let resultElementCount: number | null = null;
  const rawResult = object.value["result_element_count"];
  if (rawResult !== undefined && rawResult !== null) {
    const parsed = integerField(object.value, "result_element_count", 0, Number.MAX_SAFE_INTEGER);
    if (!parsed.ok) return parsed;
    resultElementCount = parsed.value;
  }
  const shards: { total: number | null; succeeded: number | null; failed: number | null } = {
    total: null, succeeded: null, failed: null,
  };
  const rawTotal = object.value["shards_total"];
  if (rawTotal !== undefined && rawTotal !== null) {
    const total = integerField(object.value, "shards_total", 0, 100000);
    if (!total.ok) return total;
    const succeeded = integerField(object.value, "shards_succeeded", 0, 100000);
    if (!succeeded.ok) return succeeded;
    const failedShards = integerField(object.value, "shards_failed", 0, 100000);
    if (!failedShards.ok) return failedShards;
    shards.total = total.value;
    shards.succeeded = succeeded.value;
    shards.failed = failedShards.value;
  }
  return {
    ok: true,
    value: {
      worker_id: workerId.value,
      status: status.value,
      error: error.value,
      backend: backend.value,
      result_element_count: resultElementCount,
      shards_total: shards.total,
      shards_succeeded: shards.succeeded,
      shards_failed: shards.failed,
    },
  };
}

// ---------------------------------------------------------------------------
// Serializers (snake_case, stable field order — the project contract style)
// ---------------------------------------------------------------------------

export function serializeProject(project: ProjectRecord): Record<string, unknown> {
  return {
    project_id: project.project_id,
    owner_user_id: project.owner_user_id,
    name: project.name,
    status: project.status,
    created_at_ms: project.created_at_ms,
    updated_at_ms: project.updated_at_ms,
  };
}

export function serializeMember(member: ProjectMember): Record<string, unknown> {
  return {
    project_id: member.project_id,
    user_id: member.user_id,
    role: member.role,
    created_at_ms: member.created_at_ms,
  };
}

export function serializeJob(job: ServiceJobRecord): Record<string, unknown> {
  return {
    job_id: job.job_id,
    project_id: job.project_id,
    submitted_by: job.submitted_by,
    operation: job.operation,
    element_count: job.element_count,
    requested_backend: job.requested_backend,
    requested_shard_count: job.requested_shard_count,
    status: job.status,
    error: job.error,
    submitted_at_ms: job.submitted_at_ms,
    terminal_at_ms: job.terminal_at_ms,
    total_shards: job.total_shards,
    succeeded_shards: job.succeeded_shards,
    failed_shards: job.failed_shards,
    result_element_count: job.result_element_count,
    result_backend: job.result_backend,
    attempt: job.attempt,
    cancel_requested: job.cancel_requested,
  };
}

export function serializeArtifact(artifact: ArtifactMetadata): Record<string, unknown> {
  return {
    artifact_id: artifact.artifact_id,
    project_id: artifact.project_id,
    name: artifact.name,
    created_by: artifact.created_by,
    declared_byte_size: artifact.declared_byte_size,
    created_at_ms: artifact.created_at_ms,
  };
}

export function serializeAuditEvent(event: AuditEvent): Record<string, unknown> {
  return {
    event_id: event.event_id,
    timestamp_ms: event.timestamp_ms,
    actor_user_id: event.actor_user_id,
    project_id: event.project_id,
    job_id: event.job_id,
    action: event.action,
    outcome: event.outcome,
    reason_code: event.reason_code,
  };
}

export function serializeQuota(quota: ProjectQuota): Record<string, unknown> {
  return {
    max_concurrent_jobs: quota.max_concurrent_jobs,
    max_running_shards: quota.max_running_shards,
    max_memory_bytes: quota.max_memory_bytes,
  };
}

export function serializeUsage(usage: QuotaUsage): Record<string, unknown> {
  return {
    active_jobs: usage.active_jobs,
    running_shards: usage.running_shards,
    reserved_memory_bytes: usage.reserved_memory_bytes,
  };
}

export function serializeMetrics(metrics: MetricsSummary): Record<string, unknown> {
  return {
    total_jobs: metrics.total_jobs,
    queued: metrics.queued,
    running: metrics.running,
    completed: metrics.completed,
    failed: metrics.failed,
    cancelled: metrics.cancelled,
  };
}

export function serializeClaimedJob(job: WorkerClaimedJob): Record<string, unknown> {
  return {
    job_id: job.job_id,
    project_id: job.project_id,
    operation: job.operation,
    element_count: job.element_count,
    requested_backend: job.requested_backend,
    requested_shard_count: job.requested_shard_count,
    attempt: job.attempt,
    lease_expires_at_ms: job.lease_expires_at_ms,
  };
}

// The artifact metadata cap is part of the API contract responses via
// /api/platform/info (visible to clients, honest about the bound).
export function serviceLimits(): Record<string, unknown> {
  return {
    max_page_limit: MAX_PAGE_LIMIT,
    max_artifacts_per_project: MAX_ARTIFACTS_PER_PROJECT,
    max_requested_shard_count: MAX_REQUESTED_SHARD_COUNT,
    max_job_element_count: MAX_JOB_ELEMENT_COUNT,
  };
}
