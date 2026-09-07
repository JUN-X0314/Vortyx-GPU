// API contract (Phase 11) — the TypeScript mirror of src/platform/contract.*.
//
// Same routes, same request schemas, same response field order, same stable
// error codes, same HTTP status mapping. The C++ contract tests and the
// tests under test/ pin the SAME vocabulary so the two layers cannot drift.
//
// Error schema (every failing response):
//   { "error": { "code": "<stable machine code>", "message": "<human text>" } }
//
// The 400/422 split matches the C++ rule: unparseable JSON is 400; a parsed
// request that violates the schema is 422. `config_error` is a server-side
// extension (environment misconfiguration) — the C++ device side never
// produces it.

import type { JobEnvelope, PlatformStatus } from "./types.ts";
import { KNOWN_BACKENDS, KNOWN_OPERATIONS, MAX_JOB_ELEMENT_COUNT, PROTOCOL_VERSION } from "./types.ts";
import { isValidId } from "./ids.ts";

// ---------------------------------------------------------------------------
// Stable error codes (error.code vocabulary)
// ---------------------------------------------------------------------------

export const ERR_INVALID_JSON = "invalid_json"; // 400
export const ERR_MISSING_FIELD = "missing_field"; // 422
export const ERR_INVALID_TYPE = "invalid_type"; // 422
export const ERR_INVALID_ENUM = "invalid_enum"; // 422
export const ERR_INVALID_ID = "invalid_id"; // 422
export const ERR_INVALID_VALUE = "invalid_value"; // 422
export const ERR_UNSUPPORTED_PROTOCOL = "unsupported_protocol_version"; // 422
export const ERR_UNAUTHENTICATED = "unauthenticated"; // 401
export const ERR_FORBIDDEN = "forbidden"; // 403
export const ERR_NOT_FOUND = "not_found"; // 404
export const ERR_CONFLICT = "conflict"; // 409
export const ERR_METHOD_NOT_ALLOWED = "method_not_allowed"; // 405
export const ERR_INTERNAL = "internal_error"; // 500
export const ERR_CONFIG = "config_error"; // 500 (server env only)

// ---------------------------------------------------------------------------
// HTTP status mapping (mirror of contract_http_status)
// ---------------------------------------------------------------------------

export function httpStatus(status: PlatformStatus, errorCode: string): number {
  switch (status) {
    case "ok":
      return 200;
    case "invalid_input":
      return errorCode === ERR_INVALID_JSON ? 400 : 422;
    case "unauthenticated":
      return 401;
    case "forbidden":
      return 403;
    case "not_found":
      return 404;
    case "conflict":
      return 409;
    case "internal":
      return 500;
  }
}

/** Maps a store outcome to its stable error code (mirror of store_error_code). */
export function storeErrorCode(status: PlatformStatus): string {
  switch (status) {
    case "ok":
      return "";
    case "invalid_input":
      return "invalid_request";
    case "unauthenticated":
      return ERR_UNAUTHENTICATED;
    case "forbidden":
      return ERR_FORBIDDEN;
    case "not_found":
      return ERR_NOT_FOUND;
    case "conflict":
      return ERR_CONFLICT;
    case "internal":
      return ERR_INTERNAL;
  }
}

export interface ErrorBody {
  error: { code: string; message: string };
}

export function errorBody(code: string, message: string): ErrorBody {
  return { error: { code, message } };
}

// ---------------------------------------------------------------------------
// Request validation
// ---------------------------------------------------------------------------

export interface ParseOk<T> {
  ok: true;
  value: T;
}

export interface ParseFailure {
  ok: false;
  status: "invalid_input";
  code: string;
  message: string;
}

export type ParseResult<T> = ParseOk<T> | ParseFailure;

function fail<T>(code: string, message: string): ParseResult<T> {
  return { ok: false, status: "invalid_input", code, message };
}

/** True when the value is a JSON object (not an array, not null). */
function isJsonObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function requireString(source: Record<string, unknown>, key: string): ParseResult<string> {
  const value = source[key];
  if (value === undefined) return fail(ERR_MISSING_FIELD, `missing required field '${key}'`);
  if (typeof value !== "string") return fail(ERR_INVALID_TYPE, `field '${key}' must be a string`);
  return { ok: true, value };
}

function optionalString(
  source: Record<string, unknown>,
  key: string,
  fallback: string,
): ParseResult<string> {
  const value = source[key];
  if (value === undefined) return { ok: true, value: fallback };
  if (typeof value !== "string") return fail(ERR_INVALID_TYPE, `field '${key}' must be a string`);
  return { ok: true, value };
}

function requireIntegralNumber(
  source: Record<string, unknown>,
  key: string,
): ParseResult<number> {
  const value = source[key];
  if (value === undefined) return fail(ERR_MISSING_FIELD, `missing required field '${key}'`);
  if (typeof value !== "number" || !Number.isFinite(value)) {
    return fail(ERR_INVALID_TYPE, `field '${key}' must be a number`);
  }
  if (!Number.isInteger(value) || Math.abs(value) >= 9007199254740992) {
    return fail(ERR_INVALID_VALUE, `field '${key}' must be an integral number below 2^53`);
  }
  return { ok: true, value };
}

function optionalIntegralNumber(
  source: Record<string, unknown>,
  key: string,
): ParseResult<number | null> {
  const value = source[key];
  if (value === undefined) return { ok: true, value: null };
  if (typeof value !== "number" || !Number.isFinite(value)) {
    return fail(ERR_INVALID_TYPE, `field '${key}' must be a number`);
  }
  if (!Number.isInteger(value) || Math.abs(value) >= 9007199254740992) {
    return fail(ERR_INVALID_VALUE, `field '${key}' must be an integral number below 2^53`);
  }
  return { ok: true, value };
}

function optionalStringArray(
  source: Record<string, unknown>,
  key: string,
): ParseResult<string[] | null> {
  const value = source[key];
  if (value === undefined) return { ok: true, value: null };
  if (!Array.isArray(value)) {
    return fail(ERR_INVALID_TYPE, `field '${key}' must be an array`);
  }
  for (const entry of value) {
    if (typeof entry !== "string") {
      return fail(ERR_INVALID_TYPE, `every entry of '${key}' must be a string`);
    }
  }
  return { ok: true, value: value as string[] };
}

/** Rejects anything the contract does not define — typo'd fields fail loudly. */
function rejectUnknownFields(
  source: Record<string, unknown>,
  known: readonly string[],
): ParseResult<null> {
  for (const key of Object.keys(source)) {
    if (!known.includes(key)) {
      return fail(ERR_INVALID_VALUE, `unknown field '${key}'`);
    }
  }
  return { ok: true, value: null };
}

// POST /api/devices — register a device.
export interface RegisterDeviceRequest {
  device_id: string;
  metadata: {
    protocol_version: string;
    software_version: string;
    operating_system: string;
    architecture: string;
    backends: string[];
    operations: string[];
    display_name: string;
  };
}

const REGISTER_DEVICE_FIELDS = [
  "device_id",
  "protocol_version",
  "software_version",
  "operating_system",
  "architecture",
  "display_name",
  "backends",
  "operations",
] as const;

export function parseRegisterDevice(body: unknown): ParseResult<RegisterDeviceRequest> {
  if (!isJsonObject(body)) {
    return fail(ERR_INVALID_TYPE, "request body must be a JSON object");
  }
  const unknown = rejectUnknownFields(body, REGISTER_DEVICE_FIELDS);
  if (!unknown.ok) return unknown;

  const device_id = requireString(body, "device_id");
  if (!device_id.ok) return device_id;
  const protocol_version = requireString(body, "protocol_version");
  if (!protocol_version.ok) return protocol_version;
  const software_version = requireString(body, "software_version");
  if (!software_version.ok) return software_version;
  const operating_system = optionalString(body, "operating_system", "");
  if (!operating_system.ok) return operating_system;
  const architecture = optionalString(body, "architecture", "");
  if (!architecture.ok) return architecture;
  const display_name = optionalString(body, "display_name", "");
  if (!display_name.ok) return display_name;
  const backends = optionalStringArray(body, "backends");
  if (!backends.ok) return backends;
  const operations = optionalStringArray(body, "operations");
  if (!operations.ok) return operations;

  if (!isValidId(device_id.value)) {
    return fail(ERR_INVALID_ID, "device_id is invalid (allowed: A-Z a-z 0-9 . _ -, length 1..128)");
  }

  // Model-level rules — identical to the C++ validate_device_metadata.
  if (protocol_version.value !== PROTOCOL_VERSION) {
    return fail(
      ERR_UNSUPPORTED_PROTOCOL,
      `unsupported protocol version '${protocol_version.value}' (this control plane speaks '${PROTOCOL_VERSION}')`,
    );
  }
  if (software_version.value.length === 0) {
    return fail(
      ERR_INVALID_VALUE,
      "software_version is required (a node must honestly report what it runs)",
    );
  }
  const parsedBackends = backends.value ?? [];
  for (const backend of parsedBackends) {
    if (!(KNOWN_BACKENDS as readonly string[]).includes(backend)) {
      return fail(ERR_INVALID_ENUM, `unknown backend '${backend}' in backends (known: cpu, vulkan)`);
    }
  }
  if (new Set(parsedBackends).size !== parsedBackends.length) {
    return fail(ERR_INVALID_VALUE, "duplicate backend entry");
  }
  const parsedOperations = operations.value ?? [];
  for (const operation of parsedOperations) {
    if (!(KNOWN_OPERATIONS as readonly string[]).includes(operation)) {
      return fail(ERR_INVALID_ENUM, `unknown operation '${operation}' in operations`);
    }
  }
  if (new Set(parsedOperations).size !== parsedOperations.length) {
    return fail(ERR_INVALID_VALUE, "duplicate operation entry");
  }

  return {
    ok: true,
    value: {
      device_id: device_id.value,
      metadata: {
        protocol_version: protocol_version.value,
        software_version: software_version.value,
        operating_system: operating_system.value,
        architecture: architecture.value,
        backends: parsedBackends,
        operations: parsedOperations,
        display_name: display_name.value,
      },
    },
  };
}

// POST /api/jobs — submit a job.
export interface CreateJobRequest {
  envelope: {
    job_id: string;
    operation: (typeof KNOWN_OPERATIONS)[number];
    element_count: number;
    requested_backend: string;
    priority: number;
    protocol_version: string;
    created_at_ms: number | null;
  };
  /** Optional device reference; the store proves it exists AND is owned. */
  submitted_by_device_id: string | null;
}

const CREATE_JOB_FIELDS = [
  "job_id",
  "operation",
  "element_count",
  "requested_backend",
  "priority",
  "protocol_version",
  "created_at_ms",
  "submitted_by_device_id",
] as const;

export function parseCreateJob(body: unknown): ParseResult<CreateJobRequest> {
  if (!isJsonObject(body)) {
    return fail(ERR_INVALID_TYPE, "request body must be a JSON object");
  }
  const unknown = rejectUnknownFields(body, CREATE_JOB_FIELDS);
  if (!unknown.ok) return unknown;

  const job_id = requireString(body, "job_id");
  if (!job_id.ok) return job_id;
  const operation = requireString(body, "operation");
  if (!operation.ok) return operation;
  const element_count = requireIntegralNumber(body, "element_count");
  if (!element_count.ok) return element_count;
  const requested_backend = optionalString(body, "requested_backend", "");
  if (!requested_backend.ok) return requested_backend;
  const priority = optionalIntegralNumber(body, "priority");
  if (!priority.ok) return priority;
  const protocol_version = requireString(body, "protocol_version");
  if (!protocol_version.ok) return protocol_version;
  const created_at_ms = optionalIntegralNumber(body, "created_at_ms");
  if (!created_at_ms.ok) return created_at_ms;
  const submitted_by = optionalString(body, "submitted_by_device_id", "");
  if (!submitted_by.ok) return submitted_by;

  if (!isValidId(job_id.value)) {
    return fail(ERR_INVALID_ID, "job_id is invalid (allowed: A-Z a-z 0-9 . _ -, length 1..128)");
  }

  if (!(KNOWN_OPERATIONS as readonly string[]).includes(operation.value)) {
    return fail(
      ERR_INVALID_ENUM,
      `unknown operation '${operation.value}' (known: vector_add, vector_multiply, vector_scale)`,
    );
  }
  if (element_count.value <= 0) {
    return fail(ERR_INVALID_VALUE, "field 'element_count' must be greater than 0");
  }
  if (element_count.value > MAX_JOB_ELEMENT_COUNT) {
    return fail(
      ERR_INVALID_VALUE,
      `element_count exceeds the control-plane contract cap (${MAX_JOB_ELEMENT_COUNT})`,
    );
  }
  if (
    requested_backend.value !== "" &&
    !(KNOWN_BACKENDS as readonly string[]).includes(requested_backend.value)
  ) {
    return fail(
      ERR_INVALID_ENUM,
      `unknown requested_backend '${requested_backend.value}' (known: cpu, vulkan)`,
    );
  }
  const parsedPriority = priority.value ?? 0;
  if (parsedPriority < -2147483648 || parsedPriority > 2147483647) {
    return fail(ERR_INVALID_VALUE, "field 'priority' must fit a signed 32-bit integer");
  }
  if (protocol_version.value !== PROTOCOL_VERSION) {
    return fail(
      ERR_UNSUPPORTED_PROTOCOL,
      `unsupported protocol version '${protocol_version.value}' (this control plane speaks '${PROTOCOL_VERSION}')`,
    );
  }

  return {
    ok: true,
    value: {
      envelope: {
        job_id: job_id.value,
        operation: operation.value as CreateJobRequest["envelope"]["operation"],
        element_count: element_count.value,
        requested_backend: requested_backend.value,
        priority: parsedPriority,
        protocol_version: protocol_version.value,
        created_at_ms: created_at_ms.value,
      },
      submitted_by_device_id: submitted_by.value.length > 0 ? submitted_by.value : null,
    },
  };
}

// ---------------------------------------------------------------------------
// Response serialization (documented field order — mirror of the C++ codecs)
// ---------------------------------------------------------------------------

import type { DeviceRecord, JobRecord, ResultEnvelope } from "./types.ts";

export function serializeDevice(record: DeviceRecord): Record<string, unknown> {
  return {
    device_id: record.device_id,
    owner_user_id: record.owner_user_id,
    display_name: record.metadata.display_name,
    protocol_version: record.metadata.protocol_version,
    software_version: record.metadata.software_version,
    operating_system: record.metadata.operating_system,
    architecture: record.metadata.architecture,
    backends: record.metadata.backends,
    operations: record.metadata.operations,
    status: record.status,
    last_seen_ms: record.last_seen_ms,
    created_at_ms: record.created_at_ms,
  };
}

export function serializeJob(record: JobRecord): Record<string, unknown> {
  return {
    job_id: record.job.job_id,
    owner_user_id: record.owner_user_id,
    submitted_by_device_id: record.submitted_by_device_id,
    operation: record.job.operation,
    element_count: record.job.element_count,
    requested_backend: record.job.requested_backend,
    priority: record.job.priority,
    protocol_version: record.job.protocol_version,
    status: record.status,
    error: record.error,
    created_at_ms: record.created_at_ms,
    started_at_ms: record.started_at_ms,
    completed_at_ms: record.completed_at_ms,
  };
}

export function serializeResult(result: ResultEnvelope): Record<string, unknown> {
  return {
    job_id: result.job_id,
    status: result.status,
    backend: result.backend,
    error: result.error,
    result_element_count: result.result_element_count,
  };
}

/** GET /api/platform/info payload. */
export function platformInfo(softwareVersion: string): Record<string, unknown> {
  return {
    protocol_version: PROTOCOL_VERSION,
    software_version: softwareVersion,
    operations: [...KNOWN_OPERATIONS],
    backends: [...KNOWN_BACKENDS],
  };
}

// ---------------------------------------------------------------------------
// Distributed surface (Phase 12) — the wire contract for the distributed
// endpoints, mirroring src/distributed/contract_distributed.* field for
// field: same schemas, same error codes, same status mapping. Metadata
// only — a distributed submission carries NO compute payload, and any
// unknown field is rejected.
// ---------------------------------------------------------------------------

import type {
  ClusterView,
  DistributedJobRecord,
  DistributedShardRecord,
} from "./distributed.ts";
import type { DeviceView } from "./distributed.ts";

const CREATE_DISTRIBUTED_JOB_FIELDS = [
  "job_id",
  "operation",
  "element_count",
  "requested_shard_count",
  "requested_backend",
  "priority",
  "protocol_version",
  "created_at_ms",
] as const;

export interface CreateDistributedJobRequest {
  envelope: JobEnvelope;
  requested_shard_count: number;
}

export function parseCreateDistributedJob(body: unknown): ParseResult<CreateDistributedJobRequest> {
  if (!isJsonObject(body)) {
    return fail(ERR_INVALID_TYPE, "request body must be a JSON object");
  }
  const unknown = rejectUnknownFields(body, CREATE_DISTRIBUTED_JOB_FIELDS);
  if (!unknown.ok) return unknown;

  const job_id = requireString(body, "job_id");
  if (!job_id.ok) return job_id;
  const operation = requireString(body, "operation");
  if (!operation.ok) return operation;
  const element_count = requireIntegralNumber(body, "element_count");
  if (!element_count.ok) return element_count;
  // The multi-device choice is EXPLICIT on the distributed surface.
  const requested_shard_count = requireIntegralNumber(body, "requested_shard_count");
  if (!requested_shard_count.ok) return requested_shard_count;
  const requested_backend = optionalString(body, "requested_backend", "");
  if (!requested_backend.ok) return requested_backend;
  const priority = optionalIntegralNumber(body, "priority");
  if (!priority.ok) return priority;
  const protocol_version = requireString(body, "protocol_version");
  if (!protocol_version.ok) return protocol_version;
  const created_at_ms = optionalIntegralNumber(body, "created_at_ms");
  if (!created_at_ms.ok) return created_at_ms;

  if (!isValidId(job_id.value)) {
    return fail(ERR_INVALID_ID, "job_id is invalid (allowed: A-Z a-z 0-9 . _ -, length 1..128)");
  }
  if (!(KNOWN_OPERATIONS as readonly string[]).includes(operation.value)) {
    return fail(ERR_INVALID_ENUM, `unknown operation '${operation.value}'`);
  }
  if (element_count.value <= 0) {
    return fail(ERR_INVALID_VALUE, "field 'element_count' must be greater than 0");
  }
  if (element_count.value > MAX_JOB_ELEMENT_COUNT) {
    return fail(
      ERR_INVALID_VALUE,
      `element_count exceeds the control-plane contract cap (${MAX_JOB_ELEMENT_COUNT})`,
    );
  }
  if (requested_shard_count.value < 1 || requested_shard_count.value > 4294967295) {
    return fail(
      ERR_INVALID_VALUE,
      "field 'requested_shard_count' must be between 1 and 2^32-1",
    );
  }
  if (
    requested_backend.value !== "" &&
    !(KNOWN_BACKENDS as readonly string[]).includes(requested_backend.value)
  ) {
    return fail(
      ERR_INVALID_ENUM,
      `unknown requested_backend '${requested_backend.value}' (known: cpu, vulkan)`,
    );
  }
  const parsedPriority = priority.value ?? 0;
  if (parsedPriority < -2147483648 || parsedPriority > 2147483647) {
    return fail(ERR_INVALID_VALUE, "field 'priority' must fit a signed 32-bit integer");
  }
  if (protocol_version.value !== PROTOCOL_VERSION) {
    return fail(
      ERR_UNSUPPORTED_PROTOCOL,
      `unsupported protocol version '${protocol_version.value}' (this control plane speaks '${PROTOCOL_VERSION}')`,
    );
  }

  return {
    ok: true,
    value: {
      envelope: {
        job_id: job_id.value,
        operation: operation.value as JobEnvelope["operation"],
        element_count: element_count.value,
        requested_backend: requested_backend.value,
        priority: parsedPriority,
        protocol_version: protocol_version.value,
        created_at_ms: created_at_ms.value,
      },
      requested_shard_count: requested_shard_count.value,
    },
  };
}

function serializeShardEntry(shard: DistributedShardRecord): Record<string, unknown> {
  return {
    shard_id: shard.shard_id,
    index: shard.index,
    state: shard.state,
    element_begin: shard.element_begin,
    element_end: shard.element_end,
    device_id: shard.device_id,
    attempt: shard.attempt,
    retry_count: shard.retry_count,
    failure_code: shard.failure_code,
  };
}

/** One distributed job (GET /api/distributed/jobs/:id and list entries). */
export function serializeDistributedJob(record: DistributedJobRecord): Record<string, unknown> {
  return {
    job_id: record.job_id,
    operation: record.operation,
    element_count: record.element_count,
    requested_backend: record.requested_backend,
    requested_shard_count: record.requested_shard_count,
    status: record.status,
    error: record.error,
    shards: record.shards.map(serializeShardEntry),
    shard_count: record.shards.length,
    succeeded: record.shards.filter((shard) => shard.state === "completed").length,
    failed: record.shards.filter((shard) => shard.state === "failed").length,
    cancelled: record.shards.filter((shard) => shard.state === "cancelled").length,
    created_at_ms: record.created_at_ms,
    completed_at_ms: record.completed_at_ms,
  };
}

/** The shard table alone (GET /api/distributed/jobs/:id/shards). */
export function serializeDistributedShards(
  record: DistributedJobRecord,
): Record<string, unknown> {
  return {
    job_id: record.job_id,
    shards: record.shards.map(serializeShardEntry),
  };
}

function serializeResourceVector(vector: {
  compute_units: number;
  memory_bytes: number;
  concurrent_jobs: number;
}): Record<string, unknown> {
  return {
    compute_units: vector.compute_units,
    memory_bytes: vector.memory_bytes,
    concurrent_jobs: vector.concurrent_jobs,
  };
}

/** GET /api/cluster payload (the caller's device scheduling view). */
export function serializeClusterView(view: ClusterView): Record<string, unknown> {
  return {
    revision: view.revision,
    devices: view.devices.map((device: DeviceView) => ({
      device_id: device.device_id,
      state: device.state,
      health: device.health,
      capacity: serializeResourceVector(device.capacity),
      allocated: serializeResourceVector(device.allocated),
      backends: device.backends,
      running_shards: device.running_shards,
      last_heartbeat_ms: device.last_heartbeat_ms,
    })),
  };
}
