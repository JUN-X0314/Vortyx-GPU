// Distributed / Multi-GPU record-keeping (Phase 12) — the TypeScript mirror
// of the distributed layer's CONTROL-PLANE surface.
//
// SCOPE (stated as loudly as in the C++ headers): this module records
// distributed job submissions, their shard tables and the cluster device
// views. It does NOT schedule, does NOT execute, and holds NO compute
// payloads — a distributed submission over the control plane is metadata
// (which operation, how big, how many shards, who wants it). The execution
// path lives in the C++ distributed layer (src/distributed) behind the
// provider-neutral store boundary; a future device agent would drive the
// status transitions through the same records.
//
// The state vocabularies mirror src/distributed/{job,shard}.hpp exactly:
//   job   : queued -> planning -> scheduled -> running -> completed |
//           failed | cancelled (terminal states are final)
//   shard : pending -> assigned -> running -> completed | failed |
//           retrying | cancelled
// The transition tables below are ports of the C++ pure functions, pinned
// by tests on BOTH sides so the layers cannot drift.

import type { AuthContext } from "./auth.ts";
import { isOwner, validateAuth } from "./auth.ts";
import type { PlatformStatus } from "./types.ts";
import type { JobEnvelope } from "./types.ts";

/** Distributed job lifecycle (mirror of vortyx::distributed::DistributedJobStatus). */
export type DistributedJobStatus =
  | "queued"
  | "planning"
  | "scheduled"
  | "running"
  | "completed"
  | "failed"
  | "cancelled";

/** Shard lifecycle (mirror of vortyx::distributed::ShardState). */
export type ShardState =
  | "pending"
  | "assigned"
  | "running"
  | "completed"
  | "failed"
  | "retrying"
  | "cancelled";

/** Device scheduling state (mirror of vortyx::distributed::DeviceState). */
export type DeviceSchedulingState =
  | "registering"
  | "ready"
  | "busy"
  | "draining"
  | "offline"
  | "failed";

/** Device health (mirror of vortyx::distributed::DeviceHealth). */
export type DeviceSchedulingHealth = "healthy" | "unhealthy" | "unknown";

/** One shard of a distributed job (mirror of the C++ wire schema). */
export interface DistributedShardRecord {
  shard_id: string;
  index: number;
  state: ShardState;
  element_begin: number;
  element_end: number;
  device_id: string; // "" = unplaced
  attempt: number;
  retry_count: number;
  failure_code: string; // "" = none (the stable FailureCode vocabulary)
}

/** One distributed job plus its control-plane state. */
export interface DistributedJobRecord {
  job_id: string;
  owner_user_id: string; // store-set, never client-claimed
  operation: string;
  element_count: number;
  requested_backend: string; // "" = unspecified
  requested_shard_count: number;
  status: DistributedJobStatus;
  error: string; // required when failed (failures are never hidden)
  shards: DistributedShardRecord[];
  created_at_ms: number;
  completed_at_ms: number | null;
}

/** One submission (metadata only — NO compute payload exists on the wire). */
export interface DistributedSubmission {
  envelope: JobEnvelope;
  requested_shard_count: number;
}

/** The cluster view entry (mirror of DeviceSnapshot's wire schema). */
export interface DeviceView {
  device_id: string;
  owner_user_id: string;
  state: DeviceSchedulingState;
  health: DeviceSchedulingHealth;
  capacity: { compute_units: number; memory_bytes: number; concurrent_jobs: number };
  allocated: { compute_units: number; memory_bytes: number; concurrent_jobs: number };
  backends: string[];
  running_shards: number;
  last_heartbeat_ms: number | null;
}

export interface ClusterView {
  revision: number;
  devices: DeviceView[];
}

// ---------------------------------------------------------------------------
// Transition tables (ports of the C++ pure functions — keep them identical)
// ---------------------------------------------------------------------------

const TERMINAL_JOB_STATUSES: ReadonlySet<DistributedJobStatus> = new Set([
  "completed",
  "failed",
  "cancelled",
]);

export function distributedJobTransitionValid(
  from: DistributedJobStatus,
  to: DistributedJobStatus,
): boolean {
  if (TERMINAL_JOB_STATUSES.has(from)) return false;
  switch (from) {
    case "queued":
      return to === "planning" || to === "cancelled";
    case "planning":
      return to === "scheduled" || to === "running" || to === "failed" || to === "cancelled";
    case "scheduled":
      return to === "running" || to === "planning" || to === "cancelled";
    case "running":
      return (
        to === "completed" || to === "failed" || to === "planning" || to === "cancelled"
      );
  }
  return false;
}

// ---------------------------------------------------------------------------
// The store interface (provider-neutral, like IPlatformStore)
// ---------------------------------------------------------------------------

export type StoreOk<T> = { status: "ok"; record: T; created?: boolean };
export type StoreFailure = { status: Exclude<PlatformStatus, "ok">; error: string };
export type StoreResult<T> = StoreOk<T> | StoreFailure;

export interface IDistributedStore {
  /**
   * Records a distributed job submission. IDEMPOTENT: the same job_id with
   * the same owner and payload replays (created: false); a different owner
   * or payload is a conflict. The error never reveals who owns the
   * existing record.
   */
  createDistributedJob(
    auth: AuthContext,
    submission: DistributedSubmission,
  ): Promise<StoreResult<DistributedJobRecord>>;

  /** One own distributed job. Missing OR foreign -> not_found. */
  distributedJob(auth: AuthContext, jobId: string): Promise<StoreResult<DistributedJobRecord>>;

  /** The caller's distributed jobs in submission order. */
  distributedJobs(auth: AuthContext): Promise<StoreResult<DistributedJobRecord[]>>;

  /** Owner cancellation of a non-terminal job (queued -> cancelled here). */
  cancelDistributedJob(
    auth: AuthContext,
    jobId: string,
  ): Promise<StoreResult<DistributedJobRecord>>;

  /** The caller's cluster view (device scheduling state; empty until a device agent reports). */
  clusterView(auth: AuthContext): Promise<StoreResult<ClusterView>>;

  /**
   * Control-plane-internal: a device agent's scheduling report (state,
   * health, capacity, allocation). NOT an application path — it exists so
   * the record layer is complete and testable; Phase 12 ships no route for
   * it (device agents speak for the C++ registry today).
   */
  reportDeviceView(view: DeviceView): Promise<StoreResult<DeviceView>>;
}

// ---------------------------------------------------------------------------
// In-memory implementation (local/mock — the reference of the rules)
// ---------------------------------------------------------------------------

export class InMemoryDistributedStore implements IDistributedStore {
  private readonly jobs: DistributedJobRecord[] = [];
  private readonly submissions = new Map<string, DistributedSubmission>();
  private readonly deviceViews = new Map<string, DeviceView>();
  private revision = 0;

  async createDistributedJob(
    auth: AuthContext,
    submission: DistributedSubmission,
  ): Promise<StoreResult<DistributedJobRecord>> {
    const verdict = validateAuth(auth);
    if (!verdict.ok) return { status: verdict.status, error: verdict.message };

    const existing = this.jobs.find((job) => job.job_id === submission.envelope.job_id);
    if (existing !== undefined) {
      const stored = this.submissions.get(existing.job_id);
      if (
        isOwner(auth, existing.owner_user_id) &&
        stored !== undefined &&
        sameSubmission(submission, stored)
      ) {
        return { status: "ok", record: structuredClone(existing), created: false };
      }
      return {
        status: "conflict",
        error: "job_id is already used with a different owner or payload",
      };
    }

    const now = Date.now();
    const record: DistributedJobRecord = {
      job_id: submission.envelope.job_id,
      owner_user_id: auth.user_id,
      operation: submission.envelope.operation,
      element_count: submission.envelope.element_count,
      requested_backend: submission.envelope.requested_backend,
      requested_shard_count: submission.requested_shard_count,
      status: "queued",
      error: "",
      shards: [],
      created_at_ms: now,
      completed_at_ms: null,
    };
    this.jobs.push(record);
    this.submissions.set(record.job_id, structuredClone(submission));
    return { status: "ok", record: structuredClone(record), created: true };
  }

  async distributedJob(
    auth: AuthContext,
    jobId: string,
  ): Promise<StoreResult<DistributedJobRecord>> {
    const verdict = validateAuth(auth);
    if (!verdict.ok) return { status: verdict.status, error: verdict.message };
    const record = this.jobs.find((job) => job.job_id === jobId);
    if (record === undefined || !isOwner(auth, record.owner_user_id)) {
      // Anti-enumeration: unknown and foreign are the same not_found.
      return { status: "not_found", error: "no such job" };
    }
    return { status: "ok", record: structuredClone(record) };
  }

  async distributedJobs(auth: AuthContext): Promise<StoreResult<DistributedJobRecord[]>> {
    const verdict = validateAuth(auth);
    if (!verdict.ok) return { status: verdict.status, error: verdict.message };
    const owned = this.jobs
      .filter((job) => isOwner(auth, job.owner_user_id))
      .map((job) => structuredClone(job));
    return { status: "ok", record: owned };
  }

  async cancelDistributedJob(
    auth: AuthContext,
    jobId: string,
  ): Promise<StoreResult<DistributedJobRecord>> {
    const verdict = validateAuth(auth);
    if (!verdict.ok) return { status: verdict.status, error: verdict.message };
    const record = this.jobs.find((job) => job.job_id === jobId);
    if (record === undefined || !isOwner(auth, record.owner_user_id)) {
      return { status: "not_found", error: "no such job" };
    }
    if (TERMINAL_JOB_STATUSES.has(record.status)) {
      return { status: "invalid_input", error: "job is already terminal" };
    }
    if (!distributedJobTransitionValid(record.status, "cancelled")) {
      return { status: "invalid_input", error: "illegal transition to cancelled" };
    }
    record.status = "cancelled";
    record.completed_at_ms = Date.now();
    // Non-executed shards are cancelled; real outcomes stay recorded.
    for (const shard of record.shards) {
      if (shard.state === "pending" || shard.state === "assigned" || shard.state === "retrying") {
        shard.state = "cancelled";
      }
    }
    return { status: "ok", record: structuredClone(record) };
  }

  async clusterView(auth: AuthContext): Promise<StoreResult<ClusterView>> {
    const verdict = validateAuth(auth);
    if (!verdict.ok) return { status: verdict.status, error: verdict.message };
    const devices: DeviceView[] = [];
    for (const view of this.deviceViews.values()) {
      if (isOwner(auth, view.owner_user_id)) devices.push(structuredClone(view));
    }
    return { status: "ok", record: { revision: this.revision, devices } };
  }

  async reportDeviceView(view: DeviceView): Promise<StoreResult<DeviceView>> {
    this.deviceViews.set(view.device_id, structuredClone(view));
    this.revision += 1;
    return { status: "ok", record: structuredClone(view) };
  }
}

function sameSubmission(a: DistributedSubmission, b: DistributedSubmission): boolean {
  return (
    a.envelope.job_id === b.envelope.job_id &&
    a.envelope.operation === b.envelope.operation &&
    a.envelope.element_count === b.envelope.element_count &&
    a.envelope.requested_backend === b.envelope.requested_backend &&
    a.envelope.priority === b.envelope.priority &&
    a.envelope.protocol_version === b.envelope.protocol_version &&
    a.requested_shard_count === b.requested_shard_count
  );
}
