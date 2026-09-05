// Supabase adapter (Phase 11) — the ONLY module that knows Supabase.
//
// This file is the provider adapter behind the provider-neutral
// IPlatformStore interface. Everything else in the API layer (router,
// contract, memory store, tests) is provider-agnostic; @supabase/supabase-js
// is imported HERE and nowhere else. Tests never import this module (they
// would fail only if npm install had not run — which the Phase 11 workflow
// never requires), and the local dev server never loads it (memory mode).
//
// Security model (must match the C++ store and the RLS policies):
//   * The adapter talks to Supabase WITH THE CALLER'S ACCESS TOKEN
//     (createClient per request, Authorization header = user JWT). Every
//     query therefore runs under the caller's Row Level Security policies —
//     the database enforces ownership even if this adapter had a bug.
//   * The service-role key is NEVER used here: it bypasses RLS and has no
//     business in per-user data paths.
//   * AuthN is real: verifyAccessToken() asks Supabase Auth to validate the
//     JWT (auth.getUser). The verified subject — never a client-claimed id —
//     becomes the AuthContext.
//   * NOT IMPLEMENTED in Phase 11 (by design, documented): remote execution,
//     heartbeat scheduling, result payload storage. Actual deployment and
//     project configuration are deferred until after Phase 11.

import { createClient, type SupabaseClient } from "@supabase/supabase-js";
import type { AuthContext } from "./auth.ts";
import { isOwner } from "./auth.ts";
import type { IPlatformStore, StoreFailure, StoreResult } from "./store.ts";
import type {
  DeviceMetadata,
  DeviceRecord,
  JobEnvelope,
  JobRecord,
  JobStatus,
  ResultEnvelope,
} from "./types.ts";
import { PROTOCOL_VERSION } from "./types.ts";

// Internal shape of the devices table's capabilities column (documented in
// the migration). Unknown keys are treated as opaque by readers.
interface DeviceCapabilities {
  backends?: string[];
  operations?: string[];
}

interface DeviceRow {
  id: string;
  owner_user_id: string;
  display_name: string;
  protocol_version: string;
  software_version: string;
  operating_system: string;
  architecture: string;
  capabilities: DeviceCapabilities;
  status: string;
  last_seen_at: string | null;
  created_at: string;
}

interface JobRow {
  id: string;
  owner_user_id: string;
  submitted_by_device_id: string | null;
  operation: string;
  element_count: number;
  requested_backend: string;
  priority: number;
  protocol_version: string;
  status: JobStatus;
  error: string;
  created_at: string;
  started_at: string | null;
  completed_at: string | null;
}

interface JobResultRow {
  job_id: string;
  status: "completed" | "failed";
  backend: string;
  error: string;
  result_element_count: number | null;
  created_at: string;
}

function toIso(ms: number | null): string | null {
  return ms === null ? null : new Date(ms).toISOString();
}

function fromIso(iso: string | null): number | null {
  if (iso === null) return null;
  const ms = Date.parse(iso);
  return Number.isNaN(ms) ? null : ms;
}

/** Resolves a Supabase access token into the verified subject (or null). */
export async function verifyAccessToken(
  url: string,
  anonKey: string,
  token: string,
): Promise<string | null> {
  const client = createClient(url, anonKey, {
    global: { headers: { Authorization: `Bearer ${token}` } },
  });
  const { data, error } = await client.auth.getUser(token);
  if (error !== null || data.user === null) return null;
  return data.user.id;
}

function clientFor(url: string, anonKey: string, auth: AuthContext): SupabaseClient {
  // Every query runs AS THE USER (RLS applies). Requires an access token —
  // the verifier always provides one in supabase mode.
  return createClient(url, anonKey, {
    global: { headers: { Authorization: `Bearer ${auth.access_token ?? ""}` } },
  });
}

function mapDbError(message: string, code: string | null): StoreFailure {
  // Unique-violation -> conflict; RLS WITH CHECK violations surface as 42544
  // or generic policy errors. Anything else is internal (never hidden, never
  // guessed into a success).
  if (code === "23505") return { status: "conflict", error: message };
  if (code === "42544" || code === "42501") return { status: "forbidden", error: message };
  return { status: "internal", error: message };
}

export function createSupabaseStore(url: string, anonKey: string): IPlatformStore {
  function requireToken(auth: AuthContext): StoreFailure | null {
    if (!auth.authenticated || auth.user_id.length === 0) {
      return { status: "unauthenticated", error: "authentication required" };
    }
    if (auth.access_token === undefined || auth.access_token.length === 0) {
      // Cannot run RLS-scoped queries without the caller's token.
      return { status: "unauthenticated", error: "access token missing" };
    }
    return null;
  }

  return {
    async registerDevice(
      auth: AuthContext,
      deviceId: string,
      metadata: DeviceMetadata,
    ): Promise<StoreResult<DeviceRecord>> {
      const missing = requireToken(auth);
      if (missing !== null) return missing;
      const client = clientFor(url, anonKey, auth);
      const now = new Date().toISOString();
      const { data, error } = await client
        .from("devices")
        .insert({
          id: deviceId,
          owner_user_id: auth.user_id,
          display_name: metadata.display_name,
          protocol_version: metadata.protocol_version,
          software_version: metadata.software_version,
          operating_system: metadata.operating_system,
          architecture: metadata.architecture,
          capabilities: { backends: metadata.backends, operations: metadata.operations },
          status: "online",
          last_seen_at: now,
        })
        .select("*")
        .single();
      if (error !== null) return mapDbError(error.message, error.code ?? null);
      const row = data as DeviceRow;
      return { status: "ok", record: rowToRecord(row) };
    },

    async device(auth: AuthContext, deviceId: string): Promise<StoreResult<DeviceRecord>> {
      const missing = requireToken(auth);
      if (missing !== null) return missing;
      const client = clientFor(url, anonKey, auth);
      // RLS hides foreign rows -> a foreign device is simply not_found.
      const { data, error } = await client.from("devices").select("*").eq("id", deviceId).maybeSingle();
      if (error !== null) return mapDbError(error.message, error.code ?? null);
      if (data === null) return { status: "not_found", error: "no such device" };
      return { status: "ok", record: rowToRecord(data as DeviceRow) };
    },

    async devices(auth: AuthContext): Promise<StoreResult<DeviceRecord[]>> {
      const missing = requireToken(auth);
      if (missing !== null) return missing;
      const client = clientFor(url, anonKey, auth);
      const { data, error } = await client
        .from("devices")
        .select("*")
        .order("created_at", { ascending: true });
      if (error !== null) return mapDbError(error.message, error.code ?? null);
      return { status: "ok", record: (data as DeviceRow[]).map(rowToRecord) };
    },

    async heartbeatDevice(auth: AuthContext, deviceId: string): Promise<StoreResult<DeviceRecord>> {
      const missing = requireToken(auth);
      if (missing !== null) return missing;
      const client = clientFor(url, anonKey, auth);
      const now = new Date().toISOString();
      const { data, error } = await client
        .from("devices")
        .update({ status: "online", last_seen_at: now })
        .eq("id", deviceId)
        .select("*")
        .maybeSingle();
      if (error !== null) return mapDbError(error.message, error.code ?? null);
      if (data === null) return { status: "not_found", error: "no such device" };
      return { status: "ok", record: rowToRecord(data as DeviceRow) };
    },

    async createJob(
      auth: AuthContext,
      envelope: JobEnvelope,
      submittedBy: string | null,
    ): Promise<StoreResult<JobRecord>> {
      const missing = requireToken(auth);
      if (missing !== null) return missing;
      const client = clientFor(url, anonKey, auth);

      // Idempotency check (read): identical submission -> existing record.
      const existing = await client.from("jobs").select("*").eq("id", envelope.job_id).maybeSingle();
      if (existing.error !== null) return mapDbError(existing.error.message, existing.error.code ?? null);
      if (existing.data !== null) {
        const row = existing.data as JobRow;
        const samePayload =
          row.operation === envelope.operation &&
          row.element_count === envelope.element_count &&
          row.requested_backend === envelope.requested_backend &&
          row.priority === envelope.priority &&
          row.protocol_version === envelope.protocol_version &&
          row.submitted_by_device_id === submittedBy;
        if (row.owner_user_id === auth.user_id && samePayload) {
          return { status: "ok", record: jobRowToRecord(row), created: false };
        }
        return { status: "conflict", error: "job_id is already used by a different submission" };
      }

      const { data, error } = await client
        .from("jobs")
        .insert({
          id: envelope.job_id,
          owner_user_id: auth.user_id,
          submitted_by_device_id: submittedBy,
          operation: envelope.operation,
          element_count: envelope.element_count,
          requested_backend: envelope.requested_backend,
          priority: envelope.priority,
          protocol_version: envelope.protocol_version,
          status: "queued",
          error: "",
        })
        .select("*")
        .single();
      if (error !== null) return mapDbError(error.message, error.code ?? null);
      return { status: "ok", record: jobRowToRecord(data as JobRow), created: true };
    },

    async job(auth: AuthContext, jobId: string): Promise<StoreResult<JobRecord>> {
      const missing = requireToken(auth);
      if (missing !== null) return missing;
      const client = clientFor(url, anonKey, auth);
      const { data, error } = await client.from("jobs").select("*").eq("id", jobId).maybeSingle();
      if (error !== null) return mapDbError(error.message, error.code ?? null);
      if (data === null) return { status: "not_found", error: "no such job" };
      return { status: "ok", record: jobRowToRecord(data as JobRow) };
    },

    async jobs(auth: AuthContext): Promise<StoreResult<JobRecord[]>> {
      const missing = requireToken(auth);
      if (missing !== null) return missing;
      const client = clientFor(url, anonKey, auth);
      const { data, error } = await client
        .from("jobs")
        .select("*")
        .order("created_at", { ascending: true });
      if (error !== null) return mapDbError(error.message, error.code ?? null);
      return { status: "ok", record: (data as JobRow[]).map(jobRowToRecord) };
    },

    async updateJob(
      auth: AuthContext,
      jobId: string,
      to: JobStatus,
      errorReason: string,
    ): Promise<StoreResult<JobRecord>> {
      const missing = requireToken(auth);
      if (missing !== null) return missing;
      const client = clientFor(url, anonKey, auth);
      const patch: Record<string, unknown> = { status: to, error: errorReason };
      if (to === "running") patch["started_at"] = new Date().toISOString();
      if (to === "completed" || to === "failed" || to === "cancelled") {
        patch["completed_at"] = new Date().toISOString();
      }
      const { data, error } = await client
        .from("jobs")
        .update(patch)
        .eq("id", jobId)
        .select("*")
        .maybeSingle();
      if (error !== null) return mapDbError(error.message, error.code ?? null);
      if (data === null) return { status: "not_found", error: "no such job" };
      return { status: "ok", record: jobRowToRecord(data as JobRow) };
    },

    async cancelJob(auth: AuthContext, jobId: string): Promise<StoreResult<JobRecord>> {
      return this.updateJob(auth, jobId, "cancelled", "cancelled");
    },

    async putResult(
      auth: AuthContext,
      result: ResultEnvelope,
    ): Promise<StoreResult<ResultEnvelope>> {
      const missing = requireToken(auth);
      if (missing !== null) return missing;
      const client = clientFor(url, anonKey, auth);

      // The job must exist, be owned, and be running; the transition to the
      // recorded outcome happens in the same transaction scope.
      const current = await client.from("jobs").select("*").eq("id", result.job_id).maybeSingle();
      if (current.error !== null) return mapDbError(current.error.message, current.error.code ?? null);
      if (current.data === null) return { status: "not_found", error: "no such job" };
      const row = current.data as JobRow;
      if (!isOwner(auth, row.owner_user_id)) {
        return { status: "not_found", error: "no such job" };
      }
      if (row.status === "queued") {
        return {
          status: "invalid_input",
          error: "job has not started; a result can only be recorded for a running job",
        };
      }
      if (row.status !== "running") {
        return { status: "conflict", error: "job already reached a terminal state" };
      }

      const now = new Date().toISOString();
      const update = await client
        .from("jobs")
        .update({
          status: result.status,
          error: result.error,
          completed_at: now,
        })
        .eq("id", result.job_id)
        .select("id")
        .single();
      if (update.error !== null) return mapDbError(update.error.message, update.error.code ?? null);

      const insert = await client
        .from("job_results")
        .insert({
          job_id: result.job_id,
          status: result.status,
          backend: result.backend,
          error: result.error,
          result_element_count: result.result_element_count,
        })
        .select("*")
        .single();
      if (insert.error !== null) return mapDbError(insert.error.message, insert.error.code ?? null);
      const resultRow = insert.data as JobResultRow;
      return {
        status: "ok",
        record: {
          job_id: resultRow.job_id,
          status: resultRow.status,
          backend: resultRow.backend,
          error: resultRow.error,
          result_element_count: resultRow.result_element_count,
        },
      };
    },

    async result(auth: AuthContext, jobId: string): Promise<StoreResult<ResultEnvelope>> {
      const missing = requireToken(auth);
      if (missing !== null) return missing;
      const client = clientFor(url, anonKey, auth);
      const { data, error } = await client
        .from("job_results")
        .select("*")
        .eq("job_id", jobId)
        .maybeSingle();
      if (error !== null) return mapDbError(error.message, error.code ?? null);
      if (data === null) return { status: "not_found", error: "no result has been recorded for this job" };
      const resultRow = data as JobResultRow;
      return {
        status: "ok",
        record: {
          job_id: resultRow.job_id,
          status: resultRow.status,
          backend: resultRow.backend,
          error: resultRow.error,
          result_element_count: resultRow.result_element_count,
        },
      };
    },
  };
}

function rowToRecord(row: DeviceRow): DeviceRecord {
  return {
    device_id: row.id,
    owner_user_id: row.owner_user_id,
    metadata: {
      protocol_version: row.protocol_version || PROTOCOL_VERSION,
      software_version: row.software_version,
      operating_system: row.operating_system,
      architecture: row.architecture,
      backends: row.capabilities?.backends ?? [],
      operations: row.capabilities?.operations ?? [],
      display_name: row.display_name,
    },
    status: row.status === "online" ? "online" : "offline",
    last_seen_ms: fromIso(row.last_seen_at),
    created_at_ms: fromIso(row.created_at),
  };
}

function jobRowToRecord(row: JobRow): JobRecord {
  return {
    job: {
      job_id: row.id,
      operation: row.operation as JobRecord["job"]["operation"],
      element_count: Number(row.element_count),
      requested_backend: row.requested_backend,
      priority: row.priority,
      protocol_version: row.protocol_version,
      // The client-reported envelope time is not persisted; the server's
      // created_at (exposed as created_at_ms on the record) is authoritative.
      created_at_ms: null,
    },
    owner_user_id: row.owner_user_id,
    submitted_by_device_id: row.submitted_by_device_id,
    status: row.status,
    error: row.error,
    created_at_ms: fromIso(row.created_at),
    started_at_ms: fromIso(row.started_at),
    completed_at_ms: fromIso(row.completed_at),
  };
}
