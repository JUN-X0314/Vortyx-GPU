// InMemoryPlatformStore (Phase 11) — the LOCAL/MOCK store, TypeScript side.
//
// Purpose, stated loudly: a LOCAL DEVELOPMENT AND TEST implementation. It
// persists NOTHING and must never back a deployment. It implements the
// IPlatformStore contract exactly (ownership via auth.ts, transitions via
// job-lifecycle.ts) so it is the executable specification of what the
// Supabase adapter + RLS must reproduce — the same role the C++
// InMemoryPlatformStore plays on the device-agent side.
//
// Determinism: lists return records in insertion order.

import { isOwner, validateAuth, type AuthContext } from "./auth.ts";
import { isTerminal, transitionValid } from "./job-lifecycle.ts";
import { isValidId } from "./ids.ts";
import type { IPlatformStore, StoreFailure, StoreResult } from "./store.ts";
import type {
  DeviceMetadata,
  DeviceRecord,
  JobEnvelope,
  JobRecord,
  JobStatus,
  ResultEnvelope,
} from "./types.ts";
import { MAX_JOB_ELEMENT_COUNT, PROTOCOL_VERSION } from "./types.ts";

// Single-record lookups treat a foreign record exactly like a missing one
// ("no such ...") — the RLS equivalence: a foreign row is INVISIBLE, and a
// Forbidden would leak which ids exist for other users.

function validateMetadata(metadata: DeviceMetadata): string | null {
  if (metadata.protocol_version !== PROTOCOL_VERSION) {
    return `unsupported protocol version '${metadata.protocol_version}' (this control plane speaks '${PROTOCOL_VERSION}')`;
  }
  if (metadata.software_version.length === 0) {
    return "software_version is required (a node must honestly report what it runs)";
  }
  for (const backend of metadata.backends) {
    if (backend !== "cpu" && backend !== "vulkan") {
      return `unknown backend '${backend}' in backends (known: cpu, vulkan)`;
    }
  }
  if (new Set(metadata.backends).size !== metadata.backends.length) {
    return "duplicate backend entry";
  }
  const knownOperations = ["vector_add", "vector_multiply", "vector_scale"];
  for (const operation of metadata.operations) {
    if (!knownOperations.includes(operation)) {
      return `unknown operation '${operation}' in operations`;
    }
  }
  if (new Set(metadata.operations).size !== metadata.operations.length) {
    return "duplicate operation entry";
  }
  return null;
}

function validateEnvelope(envelope: JobEnvelope): string | null {
  if (!isValidId(envelope.job_id)) {
    return "job_id is invalid (allowed: A-Z a-z 0-9 . _ -, length 1..128)";
  }
  if (!Number.isInteger(envelope.element_count) || envelope.element_count <= 0) {
    return "element_count must be a positive integer";
  }
  if (envelope.element_count > MAX_JOB_ELEMENT_COUNT) {
    return `element_count exceeds the control-plane contract cap (${MAX_JOB_ELEMENT_COUNT})`;
  }
  if (
    envelope.requested_backend !== "" &&
    envelope.requested_backend !== "cpu" &&
    envelope.requested_backend !== "vulkan"
  ) {
    return `unknown requested_backend '${envelope.requested_backend}' (known: cpu, vulkan)`;
  }
  if (envelope.protocol_version !== PROTOCOL_VERSION) {
    return `unsupported protocol version '${envelope.protocol_version}' (this control plane speaks '${PROTOCOL_VERSION}')`;
  }
  return null;
}

function validateResult(result: ResultEnvelope): string | null {
  if (!isValidId(result.job_id)) {
    return "job_id is invalid";
  }
  if (result.status !== "completed" && result.status !== "failed") {
    return "a result envelope records an OUTCOME (completed or failed)";
  }
  if (result.status === "failed" && result.error.length === 0) {
    return "a failed result requires an error reason (failures are never hidden)";
  }
  if (result.status === "completed" && result.error.length !== 0) {
    return "a completed result must not carry an error string";
  }
  if (result.backend !== "" && result.backend !== "cpu" && result.backend !== "vulkan") {
    return `unknown backend '${result.backend}' in result (known: cpu, vulkan)`;
  }
  return null;
}

function nowMs(): number {
  return Date.now();
}

function fail<T>(status: StoreFailure["status"], error: string): StoreResult<T> {
  return { status, error } as unknown as StoreResult<T>;
}

export class InMemoryPlatformStore implements IPlatformStore {
  private deviceRows: DeviceRecord[] = [];
  private jobRows: JobRecord[] = [];
  private resultRows: ResultEnvelope[] = [];

  async registerDevice(
    auth: AuthContext,
    deviceId: string,
    metadata: DeviceMetadata,
  ): Promise<StoreResult<DeviceRecord>> {
    const authFailure = validateAuth(auth);
    if (!authFailure.ok) return fail(authFailure.status, authFailure.message);
    if (!isValidId(deviceId)) {
      return fail("invalid_input", "device_id is invalid (allowed: A-Z a-z 0-9 . _ -, length 1..128)");
    }
    const metadataError = validateMetadata(metadata);
    if (metadataError !== null) return fail("invalid_input", metadataError);

    if (this.deviceRows.some((existing) => existing.device_id === deviceId)) {
      return fail("conflict", "device_id is already registered");
    }
    const now = nowMs();
    const record: DeviceRecord = {
      device_id: deviceId,
      owner_user_id: auth.user_id,
      metadata: structuredClone(metadata),
      status: "online",
      last_seen_ms: now,
      created_at_ms: now,
    };
    this.deviceRows.push(record);
    return { status: "ok", record: structuredClone(record) };
  }

  async device(auth: AuthContext, deviceId: string): Promise<StoreResult<DeviceRecord>> {
    const authFailure = validateAuth(auth);
    if (!authFailure.ok) return fail(authFailure.status, authFailure.message);
    for (const existing of this.deviceRows) {
      if (existing.device_id === deviceId) {
        if (!isOwner(auth, existing.owner_user_id)) {
          return fail("not_found", "no such device");
        }
        return { status: "ok", record: structuredClone(existing) };
      }
    }
    return fail("not_found", "no such device");
  }

  async devices(auth: AuthContext): Promise<StoreResult<DeviceRecord[]>> {
    const authFailure = validateAuth(auth);
    if (!authFailure.ok) return fail(authFailure.status, authFailure.message);
    const own = this.deviceRows.filter(
      (existing) => auth.authenticated && auth.user_id === existing.owner_user_id,
    );
    return { status: "ok", record: structuredClone(own) };
  }

  async heartbeatDevice(auth: AuthContext, deviceId: string): Promise<StoreResult<DeviceRecord>> {
    const authFailure = validateAuth(auth);
    if (!authFailure.ok) return fail(authFailure.status, authFailure.message);
    for (const existing of this.deviceRows) {
      if (existing.device_id === deviceId) {
        if (!isOwner(auth, existing.owner_user_id)) {
          return fail("not_found", "no such device");
        }
        existing.status = "online";
        existing.last_seen_ms = nowMs();
        return { status: "ok", record: structuredClone(existing) };
      }
    }
    return fail("not_found", "no such device");
  }

  async createJob(
    auth: AuthContext,
    envelope: JobEnvelope,
    submittedBy: string | null,
  ): Promise<StoreResult<JobRecord>> {
    const authFailure = validateAuth(auth);
    if (!authFailure.ok) return fail(authFailure.status, authFailure.message);
    const envelopeError = validateEnvelope(envelope);
    if (envelopeError !== null) return fail("invalid_input", envelopeError);

    // Idempotency: identical resubmission returns the existing record.
    for (const existing of this.jobRows) {
      if (existing.job.job_id !== envelope.job_id) continue;
      const sameOwner = existing.owner_user_id === auth.user_id;
      const samePayload =
        existing.job.operation === envelope.operation &&
        existing.job.element_count === envelope.element_count &&
        existing.job.requested_backend === envelope.requested_backend &&
        existing.job.priority === envelope.priority &&
        existing.job.protocol_version === envelope.protocol_version &&
        existing.job.created_at_ms === envelope.created_at_ms &&
        existing.submitted_by_device_id === submittedBy;
      if (sameOwner && samePayload) {
        return { status: "ok", record: structuredClone(existing), created: false };
      }
      return fail("conflict", "job_id is already used by a different submission");
    }

    // Cross-record authorization: the submitting device (when given) must
    // exist AND belong to the caller; neither unknown nor foreign ids leak.
    if (submittedBy !== null) {
      const found = this.deviceRows.find((device) => device.device_id === submittedBy);
      const owned = found !== undefined && found.owner_user_id === auth.user_id;
      if (!owned) {
        return fail(
          "forbidden",
          "submitted_by_device_id must reference a device owned by the authenticated user",
        );
      }
    }

    const now = nowMs();
    const record: JobRecord = {
      job: { ...envelope },
      owner_user_id: auth.user_id,
      submitted_by_device_id: submittedBy,
      status: "queued",
      error: "",
      created_at_ms: now,
      started_at_ms: null,
      completed_at_ms: null,
    };
    this.jobRows.push(record);
    return { status: "ok", record: structuredClone(record), created: true };
  }

  async job(auth: AuthContext, jobId: string): Promise<StoreResult<JobRecord>> {
    const authFailure = validateAuth(auth);
    if (!authFailure.ok) return fail(authFailure.status, authFailure.message);
    for (const existing of this.jobRows) {
      if (existing.job.job_id === jobId) {
        if (!isOwner(auth, existing.owner_user_id)) {
          return fail("not_found", "no such job");
        }
        return { status: "ok", record: structuredClone(existing) };
      }
    }
    return fail("not_found", "no such job");
  }

  async jobs(auth: AuthContext): Promise<StoreResult<JobRecord[]>> {
    const authFailure = validateAuth(auth);
    if (!authFailure.ok) return fail(authFailure.status, authFailure.message);
    const own = this.jobRows.filter(
      (existing) => auth.authenticated && auth.user_id === existing.owner_user_id,
    );
    return { status: "ok", record: structuredClone(own) };
  }

  async updateJob(
    auth: AuthContext,
    jobId: string,
    to: JobStatus,
    errorReason: string,
  ): Promise<StoreResult<JobRecord>> {
    const authFailure = validateAuth(auth);
    if (!authFailure.ok) return fail(authFailure.status, authFailure.message);
    for (const existing of this.jobRows) {
      if (existing.job.job_id === jobId) {
        if (!isOwner(auth, existing.owner_user_id)) {
          return fail("not_found", "no such job");
        }
        if (!transitionValid(existing.status, to)) {
          return fail(
            "invalid_input",
            `invalid status transition '${existing.status}' -> '${to}'`,
          );
        }
        if (to === "failed" && errorReason.length === 0) {
          return fail(
            "invalid_input",
            "a failed transition requires an error reason (failures are never hidden)",
          );
        }
        const now = nowMs();
        if (to === "running") existing.started_at_ms = now;
        if (isTerminal(to)) {
          existing.completed_at_ms = now;
          existing.error = errorReason;
        }
        existing.status = to;
        return { status: "ok", record: structuredClone(existing) };
      }
    }
    return fail("not_found", "no such job");
  }

  async cancelJob(auth: AuthContext, jobId: string): Promise<StoreResult<JobRecord>> {
    return this.updateJob(auth, jobId, "cancelled", "cancelled");
  }

  async putResult(
    auth: AuthContext,
    result: ResultEnvelope,
  ): Promise<StoreResult<ResultEnvelope>> {
    const authFailure = validateAuth(auth);
    if (!authFailure.ok) return fail(authFailure.status, authFailure.message);
    const resultError = validateResult(result);
    if (resultError !== null) return fail("invalid_input", resultError);

    for (const existing of this.jobRows) {
      if (existing.job.job_id !== result.job_id) continue;
      if (!isOwner(auth, existing.owner_user_id)) {
        return fail("not_found", "no such job");
      }
      if (existing.status === "queued") {
        return fail("invalid_input", "job has not started; a result can only be recorded for a running job");
      }
      if (isTerminal(existing.status)) {
        return fail("conflict", "job already reached a terminal state");
      }
      existing.completed_at_ms = nowMs();
      existing.status = result.status;
      existing.error = result.error;
      const stored: ResultEnvelope = { ...result };
      this.resultRows.push(stored);
      return { status: "ok", record: { ...stored } };
    }
    return fail("not_found", "no such job");
  }

  async result(auth: AuthContext, jobId: string): Promise<StoreResult<ResultEnvelope>> {
    const authFailure = validateAuth(auth);
    if (!authFailure.ok) return fail(authFailure.status, authFailure.message);
    let jobOwned = false;
    for (const existing of this.jobRows) {
      if (existing.job.job_id === jobId) {
        if (!isOwner(auth, existing.owner_user_id)) {
          return fail("not_found", "no such job");
        }
        jobOwned = true;
        break;
      }
    }
    if (!jobOwned) return fail("not_found", "no such job");
    const found = this.resultRows.find((existing) => existing.job_id === jobId);
    if (found === undefined) {
      return fail("not_found", "no result has been recorded for this job");
    }
    return { status: "ok", record: { ...found } };
  }
}
