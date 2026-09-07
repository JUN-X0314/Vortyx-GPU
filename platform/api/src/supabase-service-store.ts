// Supabase service-store adapter (Phase 15) — the PostgreSQL-backed
// implementation of IServiceStore.
//
// This file is the ONLY service module that knows Supabase. It follows the
// supabase-store.ts rules exactly:
//
//   * USER-SCOPED operations run WITH THE CALLER'S ACCESS TOKEN — every
//     query executes under that user's Row Level Security policies; the
//     database enforces the authorization even if this adapter had a bug.
//   * The service-role client is used ONLY for the worker-protocol paths
//     (claim/heartbeat/complete run outside any user's identity) and
//     reconciliation. It is never used for user-facing reads/writes and it
//     never reaches a browser.
//   * Authorization (role table, anti-enumeration NotFound) is applied in
//     code FIRST — identical rules to the memory store — and RLS is the
//     backstop (the two MUST agree; the migration comments pin it).
//   * The quota policy is enforced by a database trigger
//     (vortyx_enforce_service_quota) so concurrent submissions cannot race
//     past the policy; the adapter maps the trigger's error to the honest
//     quota_exceeded outcome.
//   * Not-yet-applied migrations are reported as failures, never faked: a
//     missing table surfaces as an internal error with the database's
//     message (the operator applies migration 0003).

import { createClient, type SupabaseClient } from "@supabase/supabase-js";
import type { AuthContext } from "./auth.ts";
import {
  DEFAULT_QUOTA,
  MAX_ARTIFACTS_PER_PROJECT,
  jobMemoryBytes,
  type ArtifactMetadata,
  type AuditEvent,
  type ProjectMember,
  type ProjectQuota,
  type ProjectRecord,
  type ProjectRole,
  type QuotaUsage,
  type ServiceJobRecord,
  type ServiceJobStatus,
  type WorkerClaimedJob,
  type WorkerCompletionReport,
} from "./service-types.ts";
import type {
  IServiceStore,
  MetricsSummary,
  Paged,
  ProjectWithRole,
  ServiceResult,
  SubmitJobInput,
  WorkerClaimOutcome,
  WorkerCompleteOutcome,
  WorkerHeartbeatOutcome,
} from "./service-store.ts";
import {
  authorizeProjectAction,
  projectRoleGrantable,
} from "./service-authz.ts";

// ---------------------------------------------------------------------------
// Row shapes (the migration's tables)
// ---------------------------------------------------------------------------

interface ProjectRow {
  id: string;
  owner_user_id: string;
  name: string;
  status: string;
  created_at: string;
  updated_at: string;
}

interface MemberRow {
  project_id: string;
  user_id: string;
  role: string;
  created_at: string;
}

interface ServiceJobRow {
  job_id: string;
  project_id: string;
  submitted_by: string;
  operation: string;
  element_count: number;
  requested_backend: string;
  requested_shard_count: number;
  status: string;
  error: string;
  submitted_at_ms: number;
  terminal_at_ms: number | null;
  total_shards: number | null;
  succeeded_shards: number | null;
  failed_shards: number | null;
  result_element_count: number | null;
  result_backend: string | null;
  attempt: number;
  claimed_by: string | null;
  claim_expires_at_ms: number | null;
  cancel_requested: boolean;
}

interface ArtifactRow {
  artifact_id: string;
  project_id: string;
  name: string;
  created_by: string;
  declared_byte_size: number;
  created_at_ms: number;
}

interface AuditRow {
  event_id: string;
  timestamp_ms: number;
  actor_user_id: string;
  project_id: string;
  job_id: string;
  action: string;
  outcome: string;
  reason_code: string;
}

// ---------------------------------------------------------------------------
// Mapping helpers
// ---------------------------------------------------------------------------

function mapProject(row: ProjectRow): ProjectRecord {
  return {
    project_id: row.id,
    owner_user_id: row.owner_user_id,
    name: row.name,
    status: row.status === "archived" ? "archived" : "active",
    created_at_ms: Date.parse(row.created_at),
    updated_at_ms: Date.parse(row.updated_at),
  };
}

function mapMember(row: MemberRow): ProjectMember {
  return {
    project_id: row.project_id,
    user_id: row.user_id,
    role: row.role as ProjectRole,
    created_at_ms: Date.parse(row.created_at),
  };
}

function mapJob(row: ServiceJobRow): ServiceJobRecord {
  return {
    job_id: row.job_id,
    project_id: row.project_id,
    submitted_by: row.submitted_by,
    operation: row.operation,
    element_count: Number(row.element_count),
    requested_backend: row.requested_backend,
    requested_shard_count: row.requested_shard_count,
    status: row.status as ServiceJobStatus,
    error: row.error,
    submitted_at_ms: Number(row.submitted_at_ms),
    terminal_at_ms: row.terminal_at_ms,
    total_shards: row.total_shards,
    succeeded_shards: row.succeeded_shards,
    failed_shards: row.failed_shards,
    result_element_count: row.result_element_count === null ? null : Number(row.result_element_count),
    result_backend: row.result_backend,
    attempt: row.attempt,
    claimed_by: row.claimed_by,
    claim_expires_at_ms: row.claim_expires_at_ms,
    cancel_requested: row.cancel_requested,
  };
}

function mapArtifact(row: ArtifactRow): ArtifactMetadata {
  return {
    artifact_id: row.artifact_id,
    project_id: row.project_id,
    name: row.name,
    created_by: row.created_by,
    declared_byte_size: Number(row.declared_byte_size),
    created_at_ms: Number(row.created_at_ms),
  };
}

function mapAudit(row: AuditRow): AuditEvent {
  return {
    event_id: row.event_id,
    timestamp_ms: Number(row.timestamp_ms),
    actor_user_id: row.actor_user_id,
    project_id: row.project_id,
    job_id: row.job_id,
    action: row.action as AuditEvent["action"],
    outcome: row.outcome as AuditEvent["outcome"],
    reason_code: row.reason_code,
  };
}

function mapDbError(message: string, code: string | null): { status: "conflict" | "forbidden" | "not_found" | "quota_exceeded" | "unavailable" | "internal"; error: string } {
  if (code === "23505") return { status: "conflict", error: message };
  if (code === "42544" || code === "42501") return { status: "forbidden", error: message };
  // The hardening trigger's shape (0004): the locked project row vanished —
  // the same not_found the API reports for any foreign/unknown project.
  if (message.startsWith("project_missing")) {
    return { status: "not_found", error: "no such project" };
  }
  // The quota trigger's documented message shape: "quota_exceeded:<field>".
  if (message.startsWith("quota_exceeded:")) {
    return { status: "quota_exceeded", error: `${message.slice("quota_exceeded:".length)} would be exceeded` };
  }
  // The artifact-capacity trigger's documented message.
  if (message.startsWith("artifact_capacity:")) {
    return { status: "unavailable", error: message.slice("artifact_capacity:".length) };
  }
  return { status: "internal", error: message };
}

/**
 * True when a PostgREST error is just ".single()/.maybeSingle() seeing zero
 * rows" (the conditional-update race loser), not a real failure. Detected
 * by code when the driver exposes it, by message/details otherwise (the
 * shape varies slightly across PostgREST/supabase-js versions).
 */
function isZeroRowsResult(error: { code?: string | null; message: string; details?: string | null }): boolean {
  if (error.code === "PGRST116") return true;
  const text = `${error.message} ${error.details ?? ""}`;
  return text.includes("0 rows");
}

// ---------------------------------------------------------------------------
// The adapter
// ---------------------------------------------------------------------------

export function createSupabaseServiceStore(
  url: string,
  anonKey: string,
  serviceRoleKey: string | null,
): IServiceStore {
  /** Per-request user client: every query runs AS THE USER (RLS applies). */
  function userClient(auth: AuthContext): SupabaseClient | null {
    if (!auth.authenticated || auth.user_id.length === 0) return null;
    if (auth.access_token === undefined || auth.access_token.length === 0) return null;
    return createClient(url, anonKey, {
      global: { headers: { Authorization: `Bearer ${auth.access_token}` } },
    });
  }

  /** The service-role client (worker paths + reconciliation ONLY). */
  function adminClient(): SupabaseClient {
    if (serviceRoleKey === null) {
      throw new Error(
        "SUPABASE_SERVICE_ROLE_KEY is required for the worker protocol in supabase mode",
      );
    }
    return createClient(url, serviceRoleKey);
  }

  function requireAuth(auth: AuthContext): { status: "unauthenticated"; error: string } | null {
    if (!auth.authenticated || auth.user_id.length === 0) {
      return { status: "unauthenticated", error: "authentication required" };
    }
    return null;
  }

  /** Resolves the caller's role in a project (NotFound = foreign/unknown). */
  async function roleOf(
    client: SupabaseClient,
    auth: AuthContext,
    projectId: string,
  ): Promise<ProjectRole | null> {
    const { data } = await client
      .from("project_members")
      .select("role")
      .eq("project_id", projectId)
      .eq("user_id", auth.user_id)
      .maybeSingle();
    if (data !== null && data !== undefined) return (data as { role: string }).role as ProjectRole;
    // The owner row is also a member row (created together), but a direct
    // owner check keeps the rule self-evident.
    const { data: project } = await client
      .from("projects")
      .select("owner_user_id")
      .eq("id", projectId)
      .maybeSingle();
    if (project !== null && project !== undefined &&
        (project as { owner_user_id: string }).owner_user_id === auth.user_id) {
      return "owner";
    }
    return null;
  }

  async function audit(
    client: SupabaseClient,
    actor: string,
    projectId: string,
    jobId: string,
    action: string,
    outcome: string,
    reasonCode: string,
  ): Promise<void> {
    await client.from("audit_events").insert({
      actor_user_id: actor.length > 0 ? actor : null,
      project_id: projectId.length > 0 ? projectId : null,
      job_id: jobId.length > 0 ? jobId : null,
      action,
      outcome,
      reason_code: reasonCode,
      timestamp_ms: Date.now(),
    });
  }

  return {
    // ---- projects ----------------------------------------------------------

    async createProject(auth, name): Promise<ServiceResult<ProjectRecord>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const { data, error } = await client
        .from("projects")
        .insert({ name })
        .select("*")
        .single();
      if (error !== null) return mapDbError(error.message, error.code);
      const project = mapProject(data as ProjectRow);
      // The owner member row is created by the same insert's trigger-side
      // default? No — the migration inserts it in the projects trigger
      // (vortyx_handle_new_project). Re-read not needed: the row exists.
      await audit(client, auth.user_id, project.project_id, "", "project_create", "ok", "");
      return { status: "ok", record: project };
    },

    async project(auth, projectId): Promise<ServiceResult<ProjectRecord>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const role = await roleOf(client, auth, projectId);
      if (role === null) return { status: "not_found", error: "no such project" };
      const { data, error } = await client.from("projects").select("*").eq("id", projectId).single();
      if (error !== null) return mapDbError(error.message, error.code);
      return { status: "ok", record: mapProject(data as ProjectRow) };
    },

    async projects(auth): Promise<ServiceResult<ProjectWithRole[]>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const { data, error } = await client
        .from("project_members")
        .select("role, projects(*)")
        .eq("user_id", auth.user_id)
        .order("created_at", { ascending: true });
      if (error !== null) return mapDbError(error.message, error.code);
      // The untyped client infers the embedded resource as any[]; the actual
      // PostgREST payload is one embedded project row per member row (the FK
      // guarantees it is never null). Asserted once, then filtered
      // defensively — no `any` enters the mapping below.
      const rows = (data ?? []) as unknown as Array<{ role: string; projects: ProjectRow }>;
      const records = rows
        .filter((row) => row.projects !== null && row.projects !== undefined)
        .map((row) => ({
          ...mapProject(row.projects),
          role: row.role as ProjectRole,
        }));
      return { status: "ok", record: records };
    },

    async archiveProject(auth, projectId): Promise<ServiceResult<ProjectRecord>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const role = await roleOf(client, auth, projectId);
      if (role === null) return { status: "not_found", error: "no such project" };
      if (authorizeProjectAction(role, "archive_project") !== "ok") {
        await audit(client, auth.user_id, projectId, "", "project_archive", "denied", "forbidden");
        return { status: "forbidden", error: "only the owner may archive the project" };
      }
      const { data, error } = await client
        .from("projects")
        .update({ status: "archived" })
        .eq("id", projectId)
        .neq("status", "archived")
        .select("*")
        .single();
      if (error !== null) {
        // zero rows updated surfaces as an error-shaped result in some
        // driver versions; distinguish honestly.
        const failure = mapDbError(error.message, error.code);
        return failure.status === "internal" && error.message.includes("0 rows")
          ? { status: "invalid_input", error: "the project is already archived" }
          : failure;
      }
      await audit(client, auth.user_id, projectId, "", "project_archive", "ok", "");
      return { status: "ok", record: mapProject(data as ProjectRow) };
    },

    async addMember(auth, projectId, userId, role): Promise<ServiceResult<ProjectMember>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const callerRole = await roleOf(client, auth, projectId);
      if (callerRole === null) return { status: "not_found", error: "no such project" };
      if (authorizeProjectAction(callerRole, "manage_members") !== "ok") {
        return { status: "forbidden", error: `the role '${callerRole}' may not manage members` };
      }
      if (!projectRoleGrantable(role)) {
        await audit(client, auth.user_id, projectId, "", "membership_change", "denied", "invalid_input");
        return {
          status: "invalid_input",
          error: "the owner role cannot be granted; a project has exactly one owner (its creator)",
        };
      }
      const { data, error } = await client
        .from("project_members")
        .insert({ project_id: projectId, user_id: userId, role })
        .select("*")
        .single();
      if (error !== null) return mapDbError(error.message, error.code);
      await audit(client, auth.user_id, projectId, "", "membership_change", "ok", role);
      return { status: "ok", record: mapMember(data as MemberRow) };
    },

    async removeMember(auth, projectId, userId): Promise<ServiceResult<null>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const callerRole = await roleOf(client, auth, projectId);
      if (callerRole === null) return { status: "not_found", error: "no such project" };
      if (authorizeProjectAction(callerRole, "manage_members") !== "ok") {
        return { status: "forbidden", error: `the role '${callerRole}' may not manage members` };
      }
      const { data, error } = await client
        .from("project_members")
        .delete()
        .eq("project_id", projectId)
        .eq("user_id", userId)
        .neq("role", "owner")
        .select("user_id");
      if (error !== null) return mapDbError(error.message, error.code);
      if (data === null || data.length === 0) {
        return { status: "not_found", error: "no such member" };
      }
      await audit(client, auth.user_id, projectId, "", "membership_change", "ok", "remove");
      return { status: "ok", record: null };
    },

    async members(auth, projectId): Promise<ServiceResult<ProjectMember[]>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const role = await roleOf(client, auth, projectId);
      if (role === null) return { status: "not_found", error: "no such project" };
      if (authorizeProjectAction(role, "view_members") !== "ok") {
        return { status: "forbidden", error: `the role '${role}' may not view members` };
      }
      const { data, error } = await client
        .from("project_members")
        .select("*")
        .eq("project_id", projectId)
        .order("created_at", { ascending: true });
      if (error !== null) return mapDbError(error.message, error.code);
      return { status: "ok", record: (data as MemberRow[]).map(mapMember) };
    },

    async roleOf(auth, projectId): Promise<ServiceResult<ProjectRole>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const role = await roleOf(client, auth, projectId);
      if (role === null) return { status: "not_found", error: "no such project" };
      return { status: "ok", record: role };
    },

    // ---- jobs ---------------------------------------------------------------

    async submitJob(auth, projectId, request: SubmitJobInput): Promise<ServiceResult<ServiceJobRecord>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };

      // Idempotency first (a replay is never rate-limited, never re-charged).
      const { data: existing } = await client
        .from("service_jobs")
        .select("*")
        .eq("job_id", request.job_id)
        .maybeSingle();
      if (existing !== null && existing !== undefined) {
        const job = mapJob(existing as ServiceJobRow);
        const samePayload =
          job.submitted_by === auth.user_id &&
          job.project_id === projectId &&
          job.operation === request.operation &&
          job.element_count === request.element_count &&
          job.requested_backend === request.requested_backend &&
          job.requested_shard_count === request.requested_shard_count;
        if (samePayload) {
          await audit(client, auth.user_id, projectId, request.job_id, "job_submit", "ok", "replay");
          return { status: "ok", record: job, created: false };
        }
        await audit(client, auth.user_id, projectId, request.job_id, "job_submit", "denied", "conflict");
        return { status: "conflict", error: "job id already used with a different submission" };
      }

      const role = await roleOf(client, auth, projectId);
      if (role === null) return { status: "not_found", error: "no such project" };
      if (authorizeProjectAction(role, "submit_job") !== "ok") {
        await audit(client, auth.user_id, projectId, request.job_id, "job_submit", "denied", "forbidden");
        return { status: "forbidden", error: `the role '${role}' may not submit jobs` };
      }
      const { data: project } = await client
        .from("projects")
        .select("status")
        .eq("id", projectId)
        .single();
      if (project !== null && project !== undefined &&
          (project as { status: string }).status === "archived") {
        await audit(client, auth.user_id, projectId, request.job_id, "job_submit", "denied", "project_archived");
        return { status: "unsupported_operation", error: "the project is archived; submissions are refused" };
      }

      // Rate limiting (the centralized fixed-window function; the same
      // semantics as the memory store's limiter — refused attempts count).
      const limited = await client.rpc("vortyx_rate_limit_take", {
        p_key: `submit:${auth.user_id}`,
        p_window_ms: 60000,
        p_max: 60,
      });
      if (limited.error !== null && limited.error !== undefined) {
        return mapDbError(limited.error.message, limited.error.code);
      }
      if (limited.data === false) {
        await audit(client, auth.user_id, projectId, request.job_id, "job_submit", "denied", "rate_limit_exceeded");
        return { status: "rate_limit_exceeded", error: "submission rate limit exceeded for this user" };
      }

      if (jobMemoryBytes(request.element_count, request.operation) === null) {
        return { status: "invalid_input", error: "element_count exceeds the addressable memory range" };
      }

      // The INSERT. The quota trigger (vortyx_enforce_service_quota) checks
      // the policy atomically against in-flight usage — concurrent
      // submissions cannot race past it (0004: the trigger serializes on
      // the project row before reading usage).
      const { data, error } = await client
        .from("service_jobs")
        .insert({
          job_id: request.job_id,
          project_id: projectId,
          submitted_by: auth.user_id,
          operation: request.operation,
          element_count: request.element_count,
          requested_backend: request.requested_backend,
          requested_shard_count: request.requested_shard_count,
        })
        .select("*")
        .single();
      if (error !== null) {
        // A 23505 here is the CONCURRENT duplicate case: another request
        // with the same job_id committed between the pre-check above and
        // this insert (the primary key is the arbiter — exactly one
        // logical reservation exists). Re-read the winner and apply the
        // SAME replay/conflict rule as the pre-check, so a retried
        // submission keeps its idempotency semantics no matter when the
        // duplicate lands.
        if (error.code === "23505") {
          const { data: winner } = await client
            .from("service_jobs")
            .select("*")
            .eq("job_id", request.job_id)
            .maybeSingle();
          if (winner !== null && winner !== undefined) {
            const job = mapJob(winner as ServiceJobRow);
            const samePayload =
              job.submitted_by === auth.user_id &&
              job.project_id === projectId &&
              job.operation === request.operation &&
              job.element_count === request.element_count &&
              job.requested_backend === request.requested_backend &&
              job.requested_shard_count === request.requested_shard_count;
            if (samePayload) {
              await audit(client, auth.user_id, projectId, request.job_id, "job_submit", "ok", "replay");
              return { status: "ok", record: job, created: false };
            }
          }
          await audit(client, auth.user_id, projectId, request.job_id, "job_submit", "denied", "conflict");
          return { status: "conflict", error: "job id already used with a different submission" };
        }
        return mapDbError(error.message, error.code);
      }
      await audit(client, auth.user_id, projectId, request.job_id, "job_submit", "ok", "");
      return { status: "ok", record: mapJob(data as ServiceJobRow), created: true };
    },

    async job(auth, jobId): Promise<ServiceResult<ServiceJobRecord>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const { data, error } = await client
        .from("service_jobs")
        .select("*")
        .eq("job_id", jobId)
        .maybeSingle();
      if (error !== null) return mapDbError(error.message, error.code);
      // RLS already hides foreign rows (not_found-shaped); the explicit
      // role check keeps the rule visible in code.
      if (data === null || data === undefined) return { status: "not_found", error: "no such job" };
      const job = mapJob(data as ServiceJobRow);
      const role = await roleOf(client, auth, job.project_id);
      if (role === null) return { status: "not_found", error: "no such job" };
      if (job.submitted_by !== auth.user_id && authorizeProjectAction(role, "view_jobs") !== "ok") {
        return { status: "forbidden", error: `the role '${role}' may not view jobs` };
      }
      return { status: "ok", record: job };
    },

    async jobs(auth, projectId, limit, offset): Promise<ServiceResult<Paged<ServiceJobRecord>>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      if (projectId !== null) {
        const role = await roleOf(client, auth, projectId);
        if (role === null) return { status: "not_found", error: "no such project" };
        if (authorizeProjectAction(role, "view_jobs") !== "ok") {
          return { status: "forbidden", error: `the role '${role}' may not view jobs` };
        }
      }
      // RLS makes foreign rows invisible; membership visibility is enforced
      // by the policies, the page is ordered by submission time.
      let query = client
        .from("service_jobs")
        .select("*")
        .order("submitted_at_ms", { ascending: true })
        .range(offset, offset + limit - 1);
      if (projectId !== null) query = query.eq("project_id", projectId);
      const { data, error } = await query;
      if (error !== null) return mapDbError(error.message, error.code);
      const items = (data as ServiceJobRow[]).map(mapJob);
      const nextOffset = items.length === limit ? offset + limit : null;
      return { status: "ok", record: { items, next_offset: nextOffset } };
    },

    async cancelJob(auth, jobId): Promise<ServiceResult<ServiceJobRecord>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const { data: row } = await client.from("service_jobs").select("*").eq("job_id", jobId).maybeSingle();
      if (row === null || row === undefined) return { status: "not_found", error: "no such job" };
      const job = mapJob(row as ServiceJobRow);
      const role = await roleOf(client, auth, job.project_id);
      if (role === null) return { status: "not_found", error: "no such job" };
      const isOwn = job.submitted_by === auth.user_id;
      if (!isOwn && authorizeProjectAction(role, "cancel_any_job") !== "ok") {
        await audit(client, auth.user_id, job.project_id, jobId, "job_cancel", "denied", "forbidden");
        return { status: "forbidden", error: `the role '${role}' may not cancel this job` };
      }

      if (job.status === "queued") {
        // Conditional update: the worker claim is racing us — whoever wins
        // the state transition decides (the documented cancel/claim race).
        // The LOSER of that race observes zero updated rows (supabase-js
        // surfaces .single()'s empty result as a PGRST116-shaped error) —
        // mapped honestly to the same "already terminal" outcome the
        // memory store returns, never to a fabricated internal error.
        const { data: updated, error } = await client
          .from("service_jobs")
          .update({ status: "cancelled", error: "cancelled", terminal_at_ms: Date.now() })
          .eq("job_id", jobId)
          .eq("status", "queued")
          .select("*")
          .single();
        if (error !== null) {
          if (isZeroRowsResult(error)) {
            return { status: "invalid_input", error: "the job is already terminal" };
          }
          return mapDbError(error.message, error.code);
        }
        if (updated !== null && updated !== undefined) {
          await audit(client, auth.user_id, job.project_id, jobId, "job_cancel", "ok",
            isOwn ? "cancelled_in_queue" : "privileged:cancel_any_job");
          return { status: "ok", record: mapJob(updated as ServiceJobRow) };
        }
        return { status: "invalid_input", error: "the job is already terminal" };
      }
      if (job.status === "running") {
        const { data: updated, error } = await client
          .from("service_jobs")
          .update({ cancel_requested: true })
          .eq("job_id", jobId)
          .eq("status", "running")
          .select("*")
          .single();
        if (error !== null) {
          // The reconcile path failed the job between the read above and
          // this update: the job went terminal under us — the honest
          // race-loser outcome (mirror of the C++ 422), not a 500.
          if (isZeroRowsResult(error)) {
            return { status: "invalid_input", error: "the job is already terminal" };
          }
          return mapDbError(error.message, error.code);
        }
        if (updated === null || updated === undefined) {
          return { status: "invalid_input", error: "the job is already terminal" };
        }
        await audit(client, auth.user_id, job.project_id, jobId, "job_cancel", "ok",
          isOwn ? "requested" : "privileged:cancel_any_job");
        return { status: "ok", record: mapJob(updated as ServiceJobRow) };
      }
      return { status: "invalid_input", error: "the job is already terminal" };
    },

    // ---- quota ---------------------------------------------------------------

    async quotaPolicy(auth, projectId): Promise<ServiceResult<ProjectQuota>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const role = await roleOf(client, auth, projectId);
      if (role === null) return { status: "not_found", error: "no such project" };
      if (authorizeProjectAction(role, "view_usage") !== "ok") {
        return { status: "forbidden", error: `the role '${role}' may not view usage` };
      }
      const { data } = await client
        .from("quota_policies")
        .select("*")
        .eq("project_id", projectId)
        .maybeSingle();
      if (data === null || data === undefined) {
        return { status: "ok", record: { ...DEFAULT_QUOTA } };
      }
      const row = data as { max_concurrent_jobs: number; max_running_shards: number; max_memory_bytes: number };
      return {
        status: "ok",
        record: {
          max_concurrent_jobs: Number(row.max_concurrent_jobs),
          max_running_shards: Number(row.max_running_shards),
          max_memory_bytes: Number(row.max_memory_bytes),
        },
      };
    },

    async setQuota(auth, projectId, quota): Promise<ServiceResult<ProjectQuota>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const role = await roleOf(client, auth, projectId);
      if (role === null) return { status: "not_found", error: "no such project" };
      if (authorizeProjectAction(role, "change_quota") !== "ok") {
        return { status: "forbidden", error: `the role '${role}' may not change quota` };
      }
      const { error } = await client
        .from("quota_policies")
        .upsert({ project_id: projectId, ...quota, updated_by: auth.user_id, updated_at_ms: Date.now() });
      if (error !== null) return mapDbError(error.message, error.code);
      await audit(client, auth.user_id, projectId, "", "quota_change", "ok", "");
      return { status: "ok", record: { ...quota } };
    },

    async usage(auth, projectId): Promise<ServiceResult<QuotaUsage>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const role = await roleOf(client, auth, projectId);
      if (role === null) return { status: "not_found", error: "no such project" };
      if (authorizeProjectAction(role, "view_usage") !== "ok") {
        return { status: "forbidden", error: `the role '${role}' may not view usage` };
      }
      // The usage ledger IS the in-flight jobs (terminal = released,
      // exactly-once by construction — the documented source of truth).
      const { data, error } = await client
        .from("service_jobs")
        .select("requested_shard_count, operation, element_count")
        .eq("project_id", projectId)
        .in("status", ["queued", "running"]);
      if (error !== null) return mapDbError(error.message, error.code);
      const usage: QuotaUsage = { active_jobs: 0, running_shards: 0, reserved_memory_bytes: 0 };
      for (const row of data as Array<{ requested_shard_count: number; operation: string; element_count: number }>) {
        usage.active_jobs += 1;
        usage.running_shards += Number(row.requested_shard_count);
        const memory = jobMemoryBytes(Number(row.element_count), row.operation);
        if (memory !== null) usage.reserved_memory_bytes += memory;
      }
      return { status: "ok", record: usage };
    },

    // ---- artifacts -----------------------------------------------------------

    async registerArtifact(auth, projectId, name, declaredByteSize): Promise<ServiceResult<ArtifactMetadata>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const role = await roleOf(client, auth, projectId);
      if (role === null) return { status: "not_found", error: "no such project" };
      if (authorizeProjectAction(role, "register_artifact") !== "ok") {
        return { status: "forbidden", error: `the role '${role}' may not register artifacts` };
      }
      // The per-project capacity is enforced atomically by the
      // vortyx_enforce_artifact_capacity trigger (mapped below).
      const { data, error } = await client
        .from("artifact_metadata")
        .insert({
          project_id: projectId,
          name,
          created_by: auth.user_id,
          declared_byte_size: declaredByteSize,
          created_at_ms: Date.now(),
        })
        .select("*")
        .single();
      if (error !== null) return mapDbError(error.message, error.code);
      const artifact = mapArtifact(data as ArtifactRow);
      await audit(client, auth.user_id, projectId, "", "artifact_register", "ok", artifact.artifact_id);
      return { status: "ok", record: artifact };
    },

    async artifacts(auth, projectId): Promise<ServiceResult<ArtifactMetadata[]>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const role = await roleOf(client, auth, projectId);
      if (role === null) return { status: "not_found", error: "no such project" };
      if (authorizeProjectAction(role, "view_jobs") !== "ok") {
        return { status: "forbidden", error: `the role '${role}' may not view artifacts` };
      }
      const { data, error } = await client
        .from("artifact_metadata")
        .select("*")
        .eq("project_id", projectId)
        .order("created_at_ms", { ascending: true });
      if (error !== null) return mapDbError(error.message, error.code);
      return { status: "ok", record: (data as ArtifactRow[]).map(mapArtifact) };
    },

    async deleteArtifact(auth, artifactId): Promise<ServiceResult<null>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const { data: row } = await client
        .from("artifact_metadata")
        .select("*")
        .eq("artifact_id", artifactId)
        .maybeSingle();
      // RLS already hides foreign-project artifacts; the double-check keeps
      // the rule in code (unknown and invisible are the same not_found).
      if (row === null || row === undefined) return { status: "not_found", error: "no such artifact" };
      const artifact = mapArtifact(row as ArtifactRow);
      const role = await roleOf(client, auth, artifact.project_id);
      if (role === null) return { status: "not_found", error: "no such artifact" };
      const isCreator = artifact.created_by === auth.user_id;
      if (!isCreator && authorizeProjectAction(role, "delete_artifact") !== "ok") {
        await audit(client, auth.user_id, artifact.project_id, "", "artifact_delete", "denied", "forbidden");
        return { status: "forbidden", error: "only the creator or an admin may delete an artifact" };
      }
      const { error } = await client.from("artifact_metadata").delete().eq("artifact_id", artifactId);
      if (error !== null) return mapDbError(error.message, error.code);
      await audit(client, auth.user_id, artifact.project_id, "", "artifact_delete", "ok",
        isCreator ? "creator" : "admin");
      return { status: "ok", record: null };
    },

    // ---- audit / metrics -------------------------------------------------------

    async auditTail(auth, limit): Promise<ServiceResult<AuditEvent[]>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const { data, error } = await client
        .from("audit_events")
        .select("*")
        .eq("actor_user_id", auth.user_id)
        .order("timestamp_ms", { ascending: false })
        .limit(limit);
      if (error !== null) return mapDbError(error.message, error.code);
      return { status: "ok", record: (data as AuditRow[]).map(mapAudit) };
    },

    async projectAudit(auth, projectId, limit): Promise<ServiceResult<AuditEvent[]>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const role = await roleOf(client, auth, projectId);
      if (role === null) return { status: "not_found", error: "no such project" };
      if (authorizeProjectAction(role, "view_audit") !== "ok") {
        return { status: "forbidden", error: `the role '${role}' may not view audit` };
      }
      const { data, error } = await client
        .from("audit_events")
        .select("*")
        .eq("project_id", projectId)
        .order("timestamp_ms", { ascending: false })
        .limit(limit);
      if (error !== null) return mapDbError(error.message, error.code);
      return { status: "ok", record: (data as AuditRow[]).map(mapAudit) };
    },

    async metrics(auth): Promise<ServiceResult<MetricsSummary>> {
      const authFailure = requireAuth(auth);
      if (authFailure !== null) return authFailure;
      const client = userClient(auth);
      if (client === null) return { status: "unauthenticated", error: "authentication required" };
      const summary: MetricsSummary = { total_jobs: 0, queued: 0, running: 0, completed: 0, failed: 0, cancelled: 0 };
      // RLS-scoped: the caller counts exactly the jobs they can see.
      const { data, error } = await client.from("service_jobs").select("status");
      if (error !== null) return mapDbError(error.message, error.code);
      for (const row of data as Array<{ status: string }>) {
        summary.total_jobs += 1;
        if (row.status in summary) {
          summary[row.status as keyof MetricsSummary & string] += 1;
        }
      }
      return { status: "ok", record: summary };
    },

    // ---- worker coordination (service-role; outside any user identity) ----------

    async workerClaim(workerId, leaseMs): Promise<WorkerClaimOutcome> {
      if (serviceRoleKey === null) {
        return { ok: false, error: "SUPABASE_SERVICE_ROLE_KEY is required for the worker protocol" };
      }
      const admin = adminClient();
      // The atomic claim: reconciliation of stale leases + the oldest queued
      // job, FOR UPDATE SKIP LOCKED — one RPC, no check-then-act gap.
      const { data, error } = await admin.rpc("vortyx_worker_claim", {
        p_worker_id: workerId,
        p_lease_ms: leaseMs,
      });
      if (error !== null && error !== undefined) {
        return { ok: false, error: error.message };
      }
      const payload = data as { claimed: boolean; job: WorkerClaimedJob | null } | null;
      if (payload === null || payload === undefined) return { ok: true, job: null };
      return { ok: true, job: payload.claimed ? payload.job : null };
    },

    async workerHeartbeat(workerId, jobId, leaseMs): Promise<WorkerHeartbeatOutcome> {
      if (serviceRoleKey === null) {
        return { ok: false, error: "SUPABASE_SERVICE_ROLE_KEY is required for the worker protocol" };
      }
      const admin = adminClient();
      const { data, error } = await admin.rpc("vortyx_worker_heartbeat", {
        p_worker_id: workerId,
        p_job_id: jobId,
        p_lease_ms: leaseMs,
      });
      if (error !== null && error !== undefined) return { ok: false, error: error.message };
      const payload = data as { accepted: boolean; cancel_requested: boolean; lease_expires_at_ms: number } | null;
      if (payload === null || payload === undefined) return { ok: true, accepted: false };
      return { ok: true, ...payload };
    },

    async workerComplete(workerId, jobId, report: WorkerCompletionReport): Promise<WorkerCompleteOutcome> {
      if (serviceRoleKey === null) {
        return { ok: false, error: "SUPABASE_SERVICE_ROLE_KEY is required for the worker protocol" };
      }
      const admin = adminClient();
      const { data, error } = await admin.rpc("vortyx_worker_complete", {
        p_worker_id: workerId,
        p_job_id: jobId,
        p_status: report.status,
        p_error: report.error,
        p_backend: report.backend,
        p_result_element_count: report.result_element_count,
        p_shards_total: report.shards_total ?? null,
        p_shards_succeeded: report.shards_succeeded ?? null,
        p_shards_failed: report.shards_failed ?? null,
      });
      if (error !== null && error !== undefined) return { ok: false, error: error.message };
      const payload = data as { recorded: boolean; status: ServiceJobStatus } | null;
      if (payload === null || payload === undefined) return { ok: false, error: "the job is not claimed by this worker" };
      return { ok: true, recorded: payload.recorded, status: payload.status };
    },

    async reconcile(): Promise<number> {
      if (serviceRoleKey === null) {
        throw new Error("SUPABASE_SERVICE_ROLE_KEY is required for reconciliation");
      }
      const admin = adminClient();
      const { data, error } = await admin.rpc("vortyx_worker_reconcile");
      if (error !== null && error !== undefined) throw new Error(error.message);
      return Number(data ?? 0);
    },
  };
}
