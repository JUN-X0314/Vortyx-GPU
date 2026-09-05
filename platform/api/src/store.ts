// Provider-neutral store interface (Phase 11) — the TypeScript mirror of
// src/platform/store.hpp (IPlatformStore).
//
// The API layer speaks ONLY to this interface. Concrete providers:
//   * memory-store.ts  — local/mock implementation (tests + local dev).
//   * supabase-store.ts — the real backend adapter; imports @supabase/
//     supabase-js and is NEVER imported by tests or by the local dev path.
//
// Every method applies the ownership rule from auth.ts with the caller's
// AuthContext; provider code never invents its own authorization. Outcomes
// use the platform Status vocabulary; failures carry a human message.
// Methods are async because the real backend is I/O-bound.

import type { AuthContext } from "./auth.ts";
import type {
  DeviceMetadata,
  DeviceRecord,
  JobEnvelope,
  JobRecord,
  JobStatus,
  PlatformStatus,
  ResultEnvelope,
} from "./types.ts";

export interface StoreOk<T> {
  status: "ok";
  record: T;
  /** createJob only: false = idempotent replay of an existing submission. */
  created?: boolean;
}

export interface StoreFailure {
  status: Exclude<PlatformStatus, "ok">;
  error: string;
}

export type StoreResult<T> = StoreOk<T> | StoreFailure;

export interface IPlatformStore {
  registerDevice(
    auth: AuthContext,
    deviceId: string,
    metadata: DeviceMetadata,
  ): Promise<StoreResult<DeviceRecord>>;

  device(auth: AuthContext, deviceId: string): Promise<StoreResult<DeviceRecord>>;

  devices(auth: AuthContext): Promise<StoreResult<DeviceRecord[]>>;

  heartbeatDevice(auth: AuthContext, deviceId: string): Promise<StoreResult<DeviceRecord>>;

  createJob(
    auth: AuthContext,
    envelope: JobEnvelope,
    submittedBy: string | null,
  ): Promise<StoreResult<JobRecord>>;

  job(auth: AuthContext, jobId: string): Promise<StoreResult<JobRecord>>;

  jobs(auth: AuthContext): Promise<StoreResult<JobRecord[]>>;

  updateJob(
    auth: AuthContext,
    jobId: string,
    to: JobStatus,
    errorReason: string,
  ): Promise<StoreResult<JobRecord>>;

  cancelJob(auth: AuthContext, jobId: string): Promise<StoreResult<JobRecord>>;

  putResult(
    auth: AuthContext,
    result: ResultEnvelope,
  ): Promise<StoreResult<ResultEnvelope>>;

  result(auth: AuthContext, jobId: string): Promise<StoreResult<ResultEnvelope>>;
}
