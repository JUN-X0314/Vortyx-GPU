// Provider-neutral service store (Phase 15) — the TypeScript mirror of the
// C++ service layer's provider-neutral seams (IProjectStore / IJobQueue /
// IArtifactStore / IAuditStore / QuotaEngine).
//
// THE SEAM between the service API surface and any persistence backend.
// Concrete providers:
//   * this file — InMemoryServiceStore: the local/mock reference (tests +
//     local development; fast, deterministic, never production).
//   * supabase-service-store.ts — the PostgreSQL adapter (production);
//     imports nothing here, is imported only by config.ts, and never by
//     the tests.
//
// AUTHORIZATION lives INSIDE the store (the IPlatformStore pattern): every
// method takes the caller's AuthContext and applies the role table
// (service-authz.ts) against the caller's project role. The router
// re-checks nothing on its own — the store is the enforcement point (and
// the database's RLS is the backstop in production).
//
// ANTI-ENUMERATION: unknown OR foreign projects/jobs/artifacts produce
// "not_found" — never "forbidden", never a leak of which ids exist.
//
// SOURCE OF TRUTH (mirrored by the migration's comments):
//   projects/project_members  — membership and roles
//   service_jobs              — the service job lifecycle + worker claim
//                               state (the durable queue + lease state)
//   quota_policies            — per-project quota policy; USAGE is DERIVED
//                               from in-flight service_jobs (a terminal job
//                               is out of the ledger by construction, so a
//                               reservation is released exactly once, with
//                               no second ledger to drift)
//   artifact_metadata         — artifact METADATA only (no payload bytes
//                               anywhere in the control plane)
//   audit_events              — the bounded audit trail

import { isOwner, type AuthContext } from "./auth.ts";
import {
  authorizeProjectAction,
  projectRoleGrantable,
  type ServiceAction,
} from "./service-authz.ts";
import {
  DEFAULT_QUOTA,
  MAX_ARTIFACTS_PER_PROJECT,
  MAX_PAGE_LIMIT,
  jobMemoryBytes,
  type ArtifactMetadata,
  type AuditAction,
  type AuditEvent,
  type AuditOutcome,
  type ProjectMember,
  type ProjectQuota,
  type ProjectRecord,
  type ProjectRole,
  type QuotaUsage,
  type ServiceJobRecord,
  type ServiceJobStatus,
  type ServiceStatus,
  type WorkerClaimedJob,
  type WorkerCompletionReport,
} from "./service-types.ts";

export interface ServiceOk<T> {
  status: "ok";
  record: T;
  /** submitJob only: false = idempotent replay of an existing submission. */
  created?: boolean;
}

export interface ServiceFailure {
  status: Exclude<ServiceStatus, "ok">;
  error: string;
}

export type ServiceResult<T> = ServiceOk<T> | ServiceFailure;

/** A project record plus the CALLER's role in it (for list views). */
export type ProjectWithRole = ProjectRecord & { role: ProjectRole };

export interface Paged<T> {
  items: T[];
  /** The offset to request next; null when the end was reached. */
  next_offset: number | null;
}

export interface MetricsSummary {
  total_jobs: number;
  queued: number;
  running: number;
  completed: number;
  failed: number;
  cancelled: number;
}

export interface SubmitJobInput {
  job_id: string;
  operation: string;
  element_count: number;
  requested_backend: string;
  requested_shard_count: number;
}

/** The worker-protocol coordination surface (service-role in production). */
export interface WorkerClaimOutcome {
  ok: boolean;
  error?: string;
  job?: WorkerClaimedJob | null;
}

export interface WorkerHeartbeatOutcome {
  ok: boolean;
  error?: string;
  accepted?: boolean;
  cancel_requested?: boolean;
  lease_expires_at_ms?: number;
}

export interface WorkerCompleteOutcome {
  ok: boolean;
  error?: string;
  /** false + ok: idempotent replay of an already-terminal job. */
  recorded?: boolean;
  status?: ServiceJobStatus;
}

export interface IWorkerCoordination {
  /** Atomically claims the oldest queued job (reconciles stale leases first). */
  workerClaim(workerId: string, leaseMs: number): Promise<WorkerClaimOutcome>;
  /** Renews the lease; reports whether cancellation was requested. */
  workerHeartbeat(workerId: string, jobId: string, leaseMs: number): Promise<WorkerHeartbeatOutcome>;
  /** Commits the terminal outcome (idempotent; duplicate-safe). */
  workerComplete(workerId: string, jobId: string, report: WorkerCompletionReport): Promise<WorkerCompleteOutcome>;
  /** Fails stale running jobs whose lease expired. Returns the count. */
  reconcile(): Promise<number>;
}

/**
 * The complete service store surface: user-facing control plane + worker
 * coordination. The memory store (below) and the Supabase adapter both
 * implement THIS interface — the router never knows the provider.
 */
export interface IServiceStore extends IWorkerCoordination {
  createProject(auth: AuthContext, name: string): Promise<ServiceResult<ProjectRecord>>;
  project(auth: AuthContext, projectId: string): Promise<ServiceResult<ProjectRecord>>;
  projects(auth: AuthContext): Promise<ServiceResult<ProjectWithRole[]>>;
  archiveProject(auth: AuthContext, projectId: string): Promise<ServiceResult<ProjectRecord>>;
  addMember(auth: AuthContext, projectId: string, userId: string, role: ProjectRole): Promise<ServiceResult<ProjectMember>>;
  removeMember(auth: AuthContext, projectId: string, userId: string): Promise<ServiceResult<null>>;
  members(auth: AuthContext, projectId: string): Promise<ServiceResult<ProjectMember[]>>;
  roleOf(auth: AuthContext, projectId: string): Promise<ServiceResult<ProjectRole>>;
  submitJob(auth: AuthContext, projectId: string, request: SubmitJobInput): Promise<ServiceResult<ServiceJobRecord>>;
  job(auth: AuthContext, jobId: string): Promise<ServiceResult<ServiceJobRecord>>;
  jobs(auth: AuthContext, projectId: string | null, limit: number, offset: number): Promise<ServiceResult<Paged<ServiceJobRecord>>>;
  cancelJob(auth: AuthContext, jobId: string): Promise<ServiceResult<ServiceJobRecord>>;
  quotaPolicy(auth: AuthContext, projectId: string): Promise<ServiceResult<ProjectQuota>>;
  setQuota(auth: AuthContext, projectId: string, quota: ProjectQuota): Promise<ServiceResult<ProjectQuota>>;
  usage(auth: AuthContext, projectId: string): Promise<ServiceResult<QuotaUsage>>;
  registerArtifact(auth: AuthContext, projectId: string, name: string, declaredByteSize: number): Promise<ServiceResult<ArtifactMetadata>>;
  artifacts(auth: AuthContext, projectId: string): Promise<ServiceResult<ArtifactMetadata[]>>;
  deleteArtifact(auth: AuthContext, artifactId: string): Promise<ServiceResult<null>>;
  auditTail(auth: AuthContext, limit: number): Promise<ServiceResult<AuditEvent[]>>;
  projectAudit(auth: AuthContext, projectId: string, limit: number): Promise<ServiceResult<AuditEvent[]>>;
  metrics(auth: AuthContext): Promise<ServiceResult<MetricsSummary>>;
}

// ---------------------------------------------------------------------------
// InMemoryServiceStore
// ---------------------------------------------------------------------------

interface MemoryJob extends ServiceJobRecord {}

const AUDIT_RING_SIZE = 10000;

export class InMemoryServiceStore implements IServiceStore {
  private projectRows: ProjectRecord[] = [];
  private memberRows: ProjectMember[] = [];
  private jobRows = new Map<string, MemoryJob>();
  private jobOrder: string[] = [];
  private quotas = new Map<string, ProjectQuota>();
  private artifactRows: ArtifactMetadata[] = [];
  private auditRows: AuditEvent[] = [];
  private auditDropped = 0;
  private eventCounter = 0;
  // Fixed-window rate limiting (mirror of the C++ RateLimiter semantics:
  // refused attempts count; window boundaries are exact multiples).
  private rateWindows = new Map<string, { windowStart: number; attempts: number }>();

  private readonly now: () => number;
  private readonly generateId: () => string;

  constructor(now: () => number = () => Date.now(), generateId: () => string = () => crypto.randomUUID()) {
    this.now = now;
    this.generateId = generateId;
  }

  // ---- helpers -----------------------------------------------------------

  private requireAuth(auth: AuthContext): ServiceFailure | null {
    if (!auth.authenticated || auth.user_id.length === 0) {
      return { status: "unauthenticated", error: "authentication required" };
    }
    return null;
  }

  private roleOfLocked(projectId: string, userId: string): ProjectRole | null {
    const project = this.projectRows.find((p) => p.project_id === projectId);
    if (project === undefined) return null;
    if (project.owner_user_id === userId) return "owner";
    const membership = this.memberRows.find(
      (m) => m.project_id === projectId && m.user_id === userId,
    );
    return membership === undefined ? null : membership.role;
  }

  private authorize(
    auth: AuthContext,
    projectId: string,
    action: ServiceAction,
  ): { role: ProjectRole } | ServiceFailure {
    const role = this.roleOfLocked(projectId, auth.user_id);
    if (role === null) {
      return { status: "not_found", error: "no such project" };
    }
    const verdict = authorizeProjectAction(role, action);
    if (verdict !== "ok") {
      return { status: "forbidden", error: `the role '${role}' may not ${action}` };
    }
    return { role };
  }

  private recordAudit(
    actor: string,
    projectId: string,
    jobId: string,
    action: AuditAction,
    outcome: AuditOutcome,
    reasonCode: string,
  ): void {
    this.eventCounter += 1;
    const event: AuditEvent = {
      event_id: `evt-${this.eventCounter.toString().padStart(8, "0")}`,
      timestamp_ms: this.now(),
      actor_user_id: actor,
      project_id: projectId,
      job_id: jobId,
      action,
      outcome,
      reason_code: reasonCode,
    };
    this.auditRows.push(event);
    if (this.auditRows.length > AUDIT_RING_SIZE) {
      this.auditRows.shift();
      this.auditDropped += 1;
    }
  }

  private quotaFor(projectId: string): ProjectQuota {
    return this.quotas.get(projectId) ?? { ...DEFAULT_QUOTA };
  }

  private usageFor(projectId: string): QuotaUsage {
    const usage: QuotaUsage = { active_jobs: 0, running_shards: 0, reserved_memory_bytes: 0 };
    for (const jobId of this.jobOrder) {
      const job = this.jobRows.get(jobId);
      if (job === undefined || job.project_id !== projectId) continue;
      if (job.status !== "queued" && job.status !== "running") continue;
      usage.active_jobs += 1;
      usage.running_shards += job.requested_shard_count;
      const memory = jobMemoryBytes(job.element_count, job.operation);
      if (memory !== null) usage.reserved_memory_bytes += memory;
    }
    return usage;
  }

  // ---- projects -----------------------------------------------------------

  async createProject(auth: AuthContext, name: string): Promise<ServiceResult<ProjectRecord>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const stamp = this.now();
    const project: ProjectRecord = {
      project_id: this.generateId(),
      owner_user_id: auth.user_id,
      name,
      status: "active",
      created_at_ms: stamp,
      updated_at_ms: stamp,
    };
    this.projectRows.push(project);
    this.memberRows.push({
      project_id: project.project_id,
      user_id: auth.user_id,
      role: "owner",
      created_at_ms: stamp,
    });
    this.recordAudit(auth.user_id, project.project_id, "", "project_create", "ok", "");
    return { status: "ok", record: project };
  }

  async project(auth: AuthContext, projectId: string): Promise<ServiceResult<ProjectRecord>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const role = this.roleOfLocked(projectId, auth.user_id);
    if (role === null) return { status: "not_found", error: "no such project" };
    const project = this.projectRows.find((p) => p.project_id === projectId);
    if (project === undefined) return { status: "not_found", error: "no such project" };
    return { status: "ok", record: { ...project } };
  }

  async projects(auth: AuthContext): Promise<ServiceResult<ProjectRecord[]>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const visible = this.projectRows.filter(
      (p) => this.roleOfLocked(p.project_id, auth.user_id) !== null,
    );
    return { status: "ok", record: visible.map((p) => ({ ...p })) };
  }

  async archiveProject(auth: AuthContext, projectId: string): Promise<ServiceResult<ProjectRecord>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const access = this.authorize(auth, projectId, "archive_project");
    if ("status" in access) return access;
    const project = this.projectRows.find((p) => p.project_id === projectId);
    if (project === undefined) return { status: "not_found", error: "no such project" };
    if (project.status === "archived") {
      return { status: "invalid_input", error: "the project is already archived" };
    }
    project.status = "archived";
    project.updated_at_ms = this.now();
    this.recordAudit(auth.user_id, projectId, "", "project_archive", "ok", "");
    return { status: "ok", record: { ...project } };
  }

  async addMember(
    auth: AuthContext,
    projectId: string,
    userId: string,
    role: ProjectRole,
  ): Promise<ServiceResult<ProjectMember>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const access = this.authorize(auth, projectId, "manage_members");
    if ("status" in access) return access;
    // The single-owner invariant (mirror of the C++ rule): the Owner role
    // is never grantable through a membership path.
    if (!projectRoleGrantable(role)) {
      this.recordAudit(auth.user_id, projectId, "", "membership_change", "denied",
        "invalid_input");
      return {
        status: "invalid_input",
        error: "the owner role cannot be granted; a project has exactly one owner (its creator)",
      };
    }
    const existing = this.memberRows.find(
      (m) => m.project_id === projectId && m.user_id === userId,
    );
    if (existing !== undefined) {
      return { status: "conflict", error: "user is already a member" };
    }
    const member: ProjectMember = {
      project_id: projectId,
      user_id: userId,
      role,
      created_at_ms: this.now(),
    };
    this.memberRows.push(member);
    this.recordAudit(auth.user_id, projectId, "", "membership_change", "ok", role);
    return { status: "ok", record: { ...member } };
  }

  async removeMember(auth: AuthContext, projectId: string, userId: string): Promise<ServiceResult<null>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const access = this.authorize(auth, projectId, "manage_members");
    if ("status" in access) return access;
    const index = this.memberRows.findIndex(
      (m) => m.project_id === projectId && m.user_id === userId,
    );
    if (index < 0) return { status: "not_found", error: "no such member" };
    if (this.memberRows[index].role === "owner") {
      return { status: "invalid_input", error: "the owner cannot be removed" };
    }
    this.memberRows.splice(index, 1);
    this.recordAudit(auth.user_id, projectId, "", "membership_change", "ok", "remove");
    return { status: "ok", record: null };
  }

  async members(auth: AuthContext, projectId: string): Promise<ServiceResult<ProjectMember[]>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const access = this.authorize(auth, projectId, "view_members");
    if ("status" in access) return access;
    const list = this.memberRows.filter((m) => m.project_id === projectId);
    return { status: "ok", record: list.map((m) => ({ ...m })) };
  }

  async roleOf(auth: AuthContext, projectId: string): Promise<ServiceResult<ProjectRole>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const role = this.roleOfLocked(projectId, auth.user_id);
    if (role === null) return { status: "not_found", error: "no such project" };
    return { status: "ok", record: role };
  }

  // ---- jobs ---------------------------------------------------------------

  async submitJob(
    auth: AuthContext,
    projectId: string,
    request: SubmitJobInput,
  ): Promise<ServiceResult<ServiceJobRecord>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;

    // Idempotency FIRST (a replay is never rate-limited, never re-charged —
    // the C++ service rule): same id + same owner + same project + same
    // payload -> replay; same id + different anything -> conflict.
    const existing = this.jobRows.get(request.job_id);
    if (existing !== undefined) {
      const samePayload =
        existing.submitted_by === auth.user_id &&
        existing.project_id === projectId &&
        existing.operation === request.operation &&
        existing.element_count === request.element_count &&
        existing.requested_backend === request.requested_backend &&
        existing.requested_shard_count === request.requested_shard_count;
      if (samePayload) {
        this.recordAudit(auth.user_id, projectId, request.job_id, "job_submit", "ok", "replay");
        return { status: "ok", record: { ...existing }, created: false };
      }
      this.recordAudit(auth.user_id, projectId, request.job_id, "job_submit", "denied",
        "conflict");
      return { status: "conflict", error: "job id already used with a different submission" };
    }

    const access = this.authorize(auth, projectId, "submit_job");
    if ("status" in access) return access;
    const project = this.projectRows.find((p) => p.project_id === projectId);
    if (project === undefined) return { status: "not_found", error: "no such project" };
    if (project.status === "archived") {
      this.recordAudit(auth.user_id, projectId, request.job_id, "job_submit", "denied",
        "project_archived");
      return { status: "unsupported_operation", error: "the project is archived; submissions are refused" };
    }

    // Rate limiting (per user; refused attempts count).
    if (!this.rateLimitTake(`submit:${auth.user_id}`)) {
      this.recordAudit(auth.user_id, projectId, request.job_id, "job_submit", "denied",
        "rate_limit_exceeded");
      return { status: "rate_limit_exceeded", error: "submission rate limit exceeded for this user" };
    }

    // Quota policy (derived usage; per-field honest refusals).
    const quota = this.quotaFor(projectId);
    const usage = this.usageFor(projectId);
    const memory = jobMemoryBytes(request.element_count, request.operation);
    if (memory === null) {
      return { status: "invalid_input", error: "element_count exceeds the addressable memory range" };
    }
    if (usage.active_jobs + 1 > quota.max_concurrent_jobs) {
      this.recordAudit(auth.user_id, projectId, request.job_id, "job_submit", "denied",
        "quota_exceeded:max_concurrent_jobs");
      return { status: "quota_exceeded", error: "max_concurrent_jobs would be exceeded" };
    }
    if (usage.running_shards + request.requested_shard_count > quota.max_running_shards) {
      this.recordAudit(auth.user_id, projectId, request.job_id, "job_submit", "denied",
        "quota_exceeded:max_running_shards");
      return { status: "quota_exceeded", error: "max_running_shards would be exceeded" };
    }
    if (usage.reserved_memory_bytes + memory > quota.max_memory_bytes) {
      this.recordAudit(auth.user_id, projectId, request.job_id, "job_submit", "denied",
        "quota_exceeded:max_memory_bytes");
      return { status: "quota_exceeded", error: "max_memory_bytes would be exceeded" };
    }

    // Insert (the idempotency check + insert are one synchronous section in
    // this store — the same atomicity the C++ service keeps under its lock
    // and the database enforces with the primary key).
    const record: ServiceJobRecord = {
      job_id: request.job_id,
      project_id: projectId,
      submitted_by: auth.user_id,
      operation: request.operation,
      element_count: request.element_count,
      requested_backend: request.requested_backend,
      requested_shard_count: request.requested_shard_count,
      status: "queued",
      error: "",
      submitted_at_ms: this.now(),
      terminal_at_ms: null,
      total_shards: null,
      succeeded_shards: null,
      failed_shards: null,
      result_element_count: null,
      result_backend: null,
      attempt: 0,
      claimed_by: null,
      claim_expires_at_ms: null,
      cancel_requested: false,
    };
    this.jobRows.set(record.job_id, record);
    this.jobOrder.push(record.job_id);
    this.recordAudit(auth.user_id, projectId, record.job_id, "job_submit", "ok", "");
    return { status: "ok", record: { ...record }, created: true };
  }

  async job(auth: AuthContext, jobId: string): Promise<ServiceResult<ServiceJobRecord>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const job = this.jobRows.get(jobId);
    if (job === undefined) return { status: "not_found", error: "no such job" };
    // Visibility: the submitter, or any project member (view_jobs).
    const role = this.roleOfLocked(job.project_id, auth.user_id);
    if (role === null) return { status: "not_found", error: "no such job" };
    if (job.submitted_by !== auth.user_id) {
      const verdict = authorizeProjectAction(role, "view_jobs");
      if (verdict !== "ok") return { status: "forbidden", error: "the role may not view jobs" };
    }
    return { status: "ok", record: { ...job } };
  }

  async jobs(
    auth: AuthContext,
    projectId: string | null,
    limit: number,
    offset: number,
  ): Promise<ServiceResult<Paged<ServiceJobRecord>>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    if (projectId !== null) {
      // A project filter requires visibility of that project (a foreign
      // project id is not_found — never an empty list).
      const access = this.authorize(auth, projectId, "view_jobs");
      if ("status" in access) return access;
    }
    const visible: ServiceJobRecord[] = [];
    for (const jobId of this.jobOrder) {
      const job = this.jobRows.get(jobId);
      if (job === undefined) continue;
      if (projectId !== null && job.project_id !== projectId) continue;
      if (job.submitted_by === auth.user_id) {
        visible.push({ ...job });
        continue;
      }
      const role = this.roleOfLocked(job.project_id, auth.user_id);
      if (role !== null && authorizeProjectAction(role, "view_jobs") === "ok") {
        visible.push({ ...job });
      }
    }
    const page = visible.slice(offset, offset + limit);
    const nextOffset = offset + limit < visible.length ? offset + limit : null;
    return { status: "ok", record: { items: page, next_offset: nextOffset } };
  }

  async cancelJob(auth: AuthContext, jobId: string): Promise<ServiceResult<ServiceJobRecord>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const job = this.jobRows.get(jobId);
    if (job === undefined) return { status: "not_found", error: "no such job" };
    const role = this.roleOfLocked(job.project_id, auth.user_id);
    if (role === null) return { status: "not_found", error: "no such job" };
    // Authorization: the submitter (cancel_own_job) or an admin+ (the
    // audited privileged cancellation).
    if (job.submitted_by !== auth.user_id) {
      const verdict = authorizeProjectAction(role, "cancel_any_job");
      if (verdict !== "ok") {
        this.recordAudit(auth.user_id, job.project_id, jobId, "job_cancel", "denied",
          "forbidden");
        return { status: "forbidden", error: "the role may not cancel this job" };
      }
    }
    if (job.status === "queued") {
      job.status = "cancelled";
      job.error = "cancelled";
      job.terminal_at_ms = this.now();
      this.recordAudit(auth.user_id, job.project_id, jobId, "job_cancel", "ok",
        job.submitted_by === auth.user_id ? "cancelled_in_queue" : "privileged:cancel_any_job");
      // The terminal transition is audited too (the C++ finalize_terminal
      // contract: every terminal outcome leaves one job_terminal event).
      this.recordAudit(job.submitted_by, job.project_id, jobId, "job_terminal", "error",
        "cancelled");
      return { status: "ok", record: { ...job } };
    }
    if (job.status === "running") {
      job.cancel_requested = true;
      this.recordAudit(auth.user_id, job.project_id, jobId, "job_cancel", "ok",
        job.submitted_by === auth.user_id ? "requested" : "privileged:cancel_any_job");
      return { status: "ok", record: { ...job } };
    }
    // Terminal: the honest race-loser outcome (mirror of the C++ 422).
    return { status: "invalid_input", error: "the job is already terminal" };
  }

  // ---- quota ---------------------------------------------------------------

  async quotaPolicy(auth: AuthContext, projectId: string): Promise<ServiceResult<ProjectQuota>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const access = this.authorize(auth, projectId, "view_usage");
    if ("status" in access) return access;
    return { status: "ok", record: { ...this.quotaFor(projectId) } };
  }

  async setQuota(
    auth: AuthContext,
    projectId: string,
    quota: ProjectQuota,
  ): Promise<ServiceResult<ProjectQuota>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const access = this.authorize(auth, projectId, "change_quota");
    if ("status" in access) return access;
    if (!Number.isInteger(quota.max_concurrent_jobs) || quota.max_concurrent_jobs < 0 ||
        !Number.isInteger(quota.max_running_shards) || quota.max_running_shards < 0 ||
        !Number.isInteger(quota.max_memory_bytes) || quota.max_memory_bytes < 0) {
      return { status: "invalid_input", error: "quota values must be non-negative integers" };
    }
    this.quotas.set(projectId, { ...quota });
    this.recordAudit(auth.user_id, projectId, "", "quota_change", "ok", "");
    return { status: "ok", record: { ...quota } };
  }

  async usage(auth: AuthContext, projectId: string): Promise<ServiceResult<QuotaUsage>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const access = this.authorize(auth, projectId, "view_usage");
    if ("status" in access) return access;
    return { status: "ok", record: this.usageFor(projectId) };
  }

  // ---- artifacts -----------------------------------------------------------

  async registerArtifact(
    auth: AuthContext,
    projectId: string,
    name: string,
    declaredByteSize: number,
  ): Promise<ServiceResult<ArtifactMetadata>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const access = this.authorize(auth, projectId, "register_artifact");
    if ("status" in access) return access;
    const count = this.artifactRows.filter((a) => a.project_id === projectId).length;
    if (count >= MAX_ARTIFACTS_PER_PROJECT) {
      return {
        status: "unavailable",
        error: `the project has reached the artifact metadata capacity (${MAX_ARTIFACTS_PER_PROJECT})`,
      };
    }
    const artifact: ArtifactMetadata = {
      artifact_id: this.generateId(),
      project_id: projectId,
      name,
      created_by: auth.user_id,
      declared_byte_size: declaredByteSize,
      created_at_ms: this.now(),
    };
    this.artifactRows.push(artifact);
    this.recordAudit(auth.user_id, projectId, "", "artifact_register", "ok",
      artifact.artifact_id);
    return { status: "ok", record: { ...artifact } };
  }

  async artifacts(auth: AuthContext, projectId: string): Promise<ServiceResult<ArtifactMetadata[]>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const access = this.authorize(auth, projectId, "view_jobs");
    if ("status" in access) return access;
    const list = this.artifactRows.filter((a) => a.project_id === projectId);
    return { status: "ok", record: list.map((a) => ({ ...a })) };
  }

  async deleteArtifact(auth: AuthContext, artifactId: string): Promise<ServiceResult<null>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const index = this.artifactRows.findIndex((a) => a.artifact_id === artifactId);
    // A foreign user's probe learns nothing: unknown AND invisible are the
    // same not_found (anti-enumeration).
    if (index < 0) return { status: "not_found", error: "no such artifact" };
    const artifact = this.artifactRows[index];
    // The project scope is DERIVED from the artifact (never client-supplied)
    // and the caller must hold a role in it.
    const access = this.authorize(auth, artifact.project_id, "view_jobs");
    if ("status" in access) return access;
    // Creator OR Admin+ (the DeleteArtifact rule).
    const isCreator = artifact.created_by === auth.user_id;
    if (!isCreator && authorizeProjectAction(access.role, "delete_artifact") !== "ok") {
      this.recordAudit(auth.user_id, artifact.project_id, "", "artifact_delete", "denied",
        "forbidden");
      return { status: "forbidden", error: "only the creator or an admin may delete an artifact" };
    }
    this.artifactRows.splice(index, 1);
    this.recordAudit(auth.user_id, artifact.project_id, "", "artifact_delete", "ok",
      isCreator ? "creator" : "admin");
    return { status: "ok", record: null };
  }

  // ---- audit / metrics -------------------------------------------------------

  async auditTail(auth: AuthContext, limit: number): Promise<ServiceResult<AuditEvent[]>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    // The caller's own events (the safe default scope, mirror of the C++
    // audit_tail_for_actor).
    const own = this.auditRows.filter((e) => e.actor_user_id === auth.user_id);
    return { status: "ok", record: own.slice(-limit).reverse() };
  }

  async projectAudit(auth: AuthContext, projectId: string, limit: number): Promise<ServiceResult<AuditEvent[]>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const access = this.authorize(auth, projectId, "view_audit");
    if ("status" in access) return access;
    const scoped = this.auditRows.filter((e) => e.project_id === projectId);
    return { status: "ok", record: scoped.slice(-limit).reverse() };
  }

  async metrics(auth: AuthContext): Promise<ServiceResult<MetricsSummary>> {
    const authFailure = this.requireAuth(auth);
    if (authFailure !== null) return authFailure;
    const summary: MetricsSummary = {
      total_jobs: 0, queued: 0, running: 0, completed: 0, failed: 0, cancelled: 0,
    };
    for (const jobId of this.jobOrder) {
      const job = this.jobRows.get(jobId);
      if (job === undefined) continue;
      const visible =
        job.submitted_by === auth.user_id ||
        (this.roleOfLocked(job.project_id, auth.user_id) !== null);
      if (!visible) continue;
      summary.total_jobs += 1;
      summary[job.status as ServiceJobStatus] += 1;
    }
    return { status: "ok", record: summary };
  }

  // ---- worker coordination -----------------------------------------------------

  async reconcile(): Promise<number> {
    // Stale running jobs (lease expired) are honestly FAILED — never left
    // running forever, never silently retried here. The worker's lease
    // renewal is the liveness contract.
    const now = this.now();
    let recovered = 0;
    for (const job of this.jobRows.values()) {
      if (
        job.status === "running" &&
        job.claim_expires_at_ms !== null &&
        job.claim_expires_at_ms < now
      ) {
        job.status = "failed";
        job.error = "worker_lease_expired";
        job.terminal_at_ms = now;
        this.recordAudit("", job.project_id, job.job_id, "job_terminal", "error",
          "worker_lease_expired");
        recovered += 1;
      }
    }
    return recovered;
  }

  async workerClaim(workerId: string, leaseMs: number): Promise<WorkerClaimOutcome> {
    if (workerId.length === 0) return { ok: false, error: "worker_id is required" };
    if (!Number.isInteger(leaseMs) || leaseMs < 1000 || leaseMs > 600000) {
      return { ok: false, error: "lease_ms must be 1000..600000" };
    }
    await this.reconcile();
    const now = this.now();
    for (const jobId of this.jobOrder) {
      const job = this.jobRows.get(jobId);
      if (job === undefined || job.status !== "queued") continue;
      job.status = "running";
      job.claimed_by = workerId;
      job.claim_expires_at_ms = now + leaseMs;
      job.attempt += 1;
      return {
        ok: true,
        job: {
          job_id: job.job_id,
          project_id: job.project_id,
          operation: job.operation,
          element_count: job.element_count,
          requested_backend: job.requested_backend,
          requested_shard_count: job.requested_shard_count,
          attempt: job.attempt,
          lease_expires_at_ms: job.claim_expires_at_ms,
        },
      };
    }
    return { ok: true, job: null };
  }

  async workerHeartbeat(workerId: string, jobId: string, leaseMs: number): Promise<WorkerHeartbeatOutcome> {
    const job = this.jobRows.get(jobId);
    if (job === undefined || job.status !== "running" || job.claimed_by !== workerId) {
      return { ok: true, accepted: false };
    }
    job.claim_expires_at_ms = this.now() + leaseMs;
    return {
      ok: true,
      accepted: true,
      cancel_requested: job.cancel_requested,
      lease_expires_at_ms: job.claim_expires_at_ms,
    };
  }

  async workerComplete(
    workerId: string,
    jobId: string,
    report: WorkerCompletionReport,
  ): Promise<WorkerCompleteOutcome> {
    const job = this.jobRows.get(jobId);
    if (job === undefined) return { ok: false, error: "no such job" };
    if (job.status !== "running" || job.claimed_by !== workerId) {
      // A terminal replay is the idempotent outcome; a foreign/queued job
      // is a conflict.
      if (isTerminal(job.status)) {
        return { ok: true, recorded: false, status: job.status };
      }
      return { ok: false, error: "the job is not claimed by this worker" };
    }
    if (report.status === "failed" && report.error.length === 0) {
      return { ok: false, error: "a failed report requires its reason" };
    }
    const now = this.now();
    job.status = report.status;
    job.error = report.error;
    job.terminal_at_ms = now;
    if (report.shards_total !== undefined) {
      job.total_shards = report.shards_total;
      job.succeeded_shards = report.shards_succeeded ?? null;
      job.failed_shards = report.shards_failed ?? null;
    }
    if (report.result_element_count !== null) {
      job.result_element_count = report.result_element_count;
    }
    job.result_backend = report.backend.length > 0 ? report.backend : null;
    this.recordAudit(job.submitted_by, job.project_id, jobId, "job_terminal",
      report.status === "completed" ? "ok" : "error", report.status);
    return { ok: true, recorded: true, status: job.status };
  }

  // ---- test observability ---------------------------------------------------
  get auditDroppedTotal(): number {
    return this.auditDropped;
  }

  // The rate limiter (private): fixed window per key, refused attempts
  // count — the documented C++ RateLimiter semantics.
  private rateLimitTake(key: string, maxPerWindow = 60, windowMs = 60000): boolean {
    const now = this.now();
    const windowStart = Math.floor(now / windowMs) * windowMs;
    const counter = this.rateWindows.get(key);
    if (counter === undefined || counter.windowStart !== windowStart) {
      this.rateWindows.set(key, { windowStart, attempts: 1 });
      return maxPerWindow >= 1;
    }
    counter.attempts += 1;
    return counter.attempts <= maxPerWindow;
  }
}

function isTerminal(status: ServiceJobStatus): boolean {
  return status === "completed" || status === "failed" || status === "cancelled";
}

// Re-export for the router's ownership checks.
export { isOwner };
