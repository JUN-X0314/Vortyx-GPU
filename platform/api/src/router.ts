// Platform request router (Phase 11) — the API layer's ONE pipeline.
//
// Every Vercel function in api/ is a thin adapter that feeds its request
// into handlePlatformRequest(); all real logic lives here so the whole
// surface is testable without HTTP. The pipeline is exactly the documented
// contract order:
//
//   1. Route resolution        -> 404 unknown route, 405 wrong method
//   2. Authentication (AuthN)  -> Bearer token verified by the configured
//                                 verifier; 401 when unusable
//   3. Request parsing (schema)-> 400 malformed JSON, 422 schema violations
//   4. Store operation (AuthZ) -> the store applies the ownership rule
//   5. Response serialization  -> 200 with the documented schema, or the
//                                 unified {"error":{code,message}} body
//
// Unauthenticated / forbidden / not-found outcomes come back from the store
// and are mapped with the SAME status mapping the C++ contract pins
// (contract.ts httpStatus + storeErrorCode) — one vocabulary everywhere.

import type { AuthContext, TokenVerifier } from "./auth.ts";
import type { IPlatformStore } from "./store.ts";
import type { PlatformStatus } from "./types.ts";
import {
  errorBody,
  ERR_INTERNAL,
  ERR_INVALID_JSON,
  ERR_METHOD_NOT_ALLOWED,
  ERR_NOT_FOUND,
  ERR_UNAUTHENTICATED,
  httpStatus,
  parseCreateJob,
  parseRegisterDevice,
  serializeDevice,
  serializeJob,
  platformInfo,
  storeErrorCode,
} from "./contract.ts";
import { isValidId } from "./ids.ts";
import { PROTOCOL_VERSION } from "./types.ts";

export interface PlatformRequest {
  method: string; // "GET" | "POST" | "PATCH" | ...
  path: string; // e.g. "/api/jobs/abc/cancel" (no query string)
  /** Raw request body. A string that fails JSON.parse becomes 400. */
  body: unknown;
  /** The Authorization header value (may be undefined). */
  authorization?: string;
}

export interface PlatformResponse {
  status: number;
  body: unknown;
}

export interface PlatformDeps {
  store: IPlatformStore;
  verifier: TokenVerifier;
  storeKind: string;
  softwareVersion: string;
  /** Present only to report configuration readiness in /api/health. */
  configError?: string | null;
}

interface Failure {
  status: number;
  body: unknown;
}

function failure(platformStatus: PlatformStatus, code: string, message: string): Failure {
  const status = httpStatus(platformStatus, code);
  return { status, body: errorBody(code, message) };
}

/**
 * Resolves the Bearer token into an AuthContext (or null). The memory-mode
 * local scheme ("local:<user_id>") is a documented LOCAL/MOCK convenience —
 * there is no real credential behind it, and production deployments run the
 * supabase verifier, which validates genuine Supabase Auth tokens.
 */
async function authenticate(
  authorization: string | undefined,
  verifier: TokenVerifier,
): Promise<AuthContext | null> {
  if (authorization === undefined) return null;
  const match = /^Bearer\s+(.+)$/i.exec(authorization.trim());
  if (match === null) return null;
  return verifier(match[1]);
}

function parseBody(body: unknown): ParseBodyOutcome {
  if (body === undefined || body === null) return { ok: false, status: 400 };
  if (typeof body === "string") {
    try {
      return { ok: true, value: JSON.parse(body) as unknown };
    } catch {
      return { ok: false, status: 400 };
    }
  }
  return { ok: true, value: body };
}

interface ParseBodyOutcome {
  ok: boolean;
  status: number;
  value?: unknown;
}

/**
 * The complete control-plane surface. Pure routing/dispatch: no business
 * rules live here — validation is contract.ts, rules are the store.
 */
export async function handlePlatformRequest(
  request: PlatformRequest,
  deps: PlatformDeps,
): Promise<PlatformResponse> {
  try {
    const outcome = await dispatch(request, deps);
    return { status: outcome.status, body: outcome.body };
  } catch (error) {
    // Store/verifier throw only on misconfiguration or unexpected failures —
    // reported honestly as 500, never converted into a fake success.
    const message = error instanceof Error ? error.message : "unexpected internal error";
    return { status: 500, body: errorBody(ERR_INTERNAL, message) };
  }
}

async function dispatch(request: PlatformRequest, deps: PlatformDeps): Promise<Failure | { status: number; body: unknown }> {
  const method = request.method.toUpperCase();
  const path = normalizePath(request.path);

  // ---- 1. public routes (no authentication required) ---------------------

  if (path === "/api/health") {
    if (method !== "GET") return methodNotAllowed();
    return {
      status: 200,
      body: {
        status: "ok",
        protocol_version: PROTOCOL_VERSION,
        store: deps.storeKind,
        config_error: deps.configError ?? null,
      },
    };
  }

  if (path === "/api/platform/info") {
    if (method !== "GET") return methodNotAllowed();
    return { status: 200, body: platformInfo(deps.softwareVersion) };
  }

  // ---- 2. route resolution BEFORE authentication -------------------------
  // (the documented pipeline order: an unknown route or a wrong method is
  // reported as such even without credentials — existence of a route is not
  // a secret, and it keeps API debugging honest)
  const target = resolveTarget(method, path);
  if (target.kind === "not_found") return notFoundRoute();
  if (target.kind === "method_not_allowed") return methodNotAllowed();

  // ---- 3. authentication (AuthN) -----------------------------------------

  let auth: AuthContext;
  try {
    const resolved = await authenticate(request.authorization, deps.verifier);
    if (resolved === null) {
      return failure("unauthenticated", ERR_UNAUTHENTICATED, "authentication required");
    }
    auth = resolved;
  } catch (error) {
    // Verifier failure (e.g. Supabase unreachable) is a server-side error,
    // not a 401: the caller may be perfectly valid.
    const message = error instanceof Error ? error.message : "token verification failed";
    return { status: 500, body: errorBody(ERR_INTERNAL, message) };
  }

  // ---- 4. request validation + 5. store operation ------------------------

  switch (target.kind) {
    case "register": {
      const parsed = parseBody(request.body);
      if (!parsed.ok) return malformedBody();
      const request_ = parseRegisterDevice(parsed.value);
      if (!request_.ok) {
        return {
          status: httpStatus("invalid_input", request_.code),
          body: errorBody(request_.code, request_.message),
        };
      }
      const result = await deps.store.registerDevice(auth, request_.value.device_id, request_.value.metadata);
      return storeRespond(result, (record) => serializeDevice(record));
    }

    case "devices_list": {
      const result = await deps.store.devices(auth);
      return storeRespond(result, (records) => ({ devices: records.map(serializeDevice) }));
    }

    case "heartbeat": {
      const result = await deps.store.heartbeatDevice(auth, target.id);
      return storeRespond(result, (record) => serializeDevice(record));
    }

    case "create_job": {
      const parsed = parseBody(request.body);
      if (!parsed.ok) return malformedBody();
      const request_ = parseCreateJob(parsed.value);
      if (!request_.ok) {
        return {
          status: httpStatus("invalid_input", request_.code),
          body: errorBody(request_.code, request_.message),
        };
      }
      const submitted = request_.value.submitted_by_device_id;
      const result = await deps.store.createJob(auth, request_.value.envelope, submitted);
      return storeRespond(result, (record) => serializeJob(record), { created: true });
    }

    case "jobs_list": {
      const result = await deps.store.jobs(auth);
      return storeRespond(result, (records) => ({ jobs: records.map(serializeJob) }));
    }

    case "job_detail": {
      const result = await deps.store.job(auth, target.id);
      return storeRespond(result, (record) => serializeJob(record));
    }

    case "cancel_job": {
      const result = await deps.store.cancelJob(auth, target.id);
      return storeRespond(result, (record) => serializeJob(record));
    }
  }
}

// ---------------------------------------------------------------------------
// Route resolution (method + path -> target; pure, before any authentication)
// ---------------------------------------------------------------------------

function normalizePath(path: string): string {
  let normalized = path.split("?")[0];
  if (normalized.length > 1 && normalized.endsWith("/")) {
    normalized = normalized.slice(0, -1);
  }
  return normalized;
}

type Target =
  | { kind: "register" }
  | { kind: "devices_list" }
  | { kind: "heartbeat"; id: string }
  | { kind: "create_job" }
  | { kind: "jobs_list" }
  | { kind: "job_detail"; id: string }
  | { kind: "cancel_job"; id: string }
  | { kind: "not_found" }
  | { kind: "method_not_allowed" };

function resolveTarget(method: string, path: string): Target {
  switch (path) {
    case "/api/devices/register":
      return method === "POST" ? { kind: "register" } : { kind: "method_not_allowed" };
    case "/api/devices":
      return method === "GET" ? { kind: "devices_list" } : { kind: "method_not_allowed" };
    case "/api/jobs":
      if (method === "POST") return { kind: "create_job" };
      if (method === "GET") return { kind: "jobs_list" };
      return { kind: "method_not_allowed" };
    default:
      break;
  }

  // Dynamic segments. Ids are charset-restricted; a segment outside the
  // charset can never name a real resource -> not_found (no decoding, no
  // exception path).
  const heartbeat = matchHeartbeat(path);
  if (heartbeat !== null) {
    if (method !== "PATCH") return { kind: "method_not_allowed" };
    return isValidId(heartbeat) ? { kind: "heartbeat", id: heartbeat } : { kind: "not_found" };
  }
  const cancel = matchCancel(path);
  if (cancel !== null) {
    if (method !== "POST") return { kind: "method_not_allowed" };
    return isValidId(cancel) ? { kind: "cancel_job", id: cancel } : { kind: "not_found" };
  }
  const jobId = matchJobId(path);
  if (jobId !== null) {
    if (method !== "GET") return { kind: "method_not_allowed" };
    return isValidId(jobId) ? { kind: "job_detail", id: jobId } : { kind: "not_found" };
  }
  return { kind: "not_found" };
}

function matchHeartbeat(path: string): string | null {
  const match = /^\/api\/devices\/([^/]+)\/heartbeat$/.exec(path);
  return match === null ? null : match[1]; // raw segment; ids never need decoding
}

function matchCancel(path: string): string | null {
  const match = /^\/api\/jobs\/([^/]+)\/cancel$/.exec(path);
  return match === null ? null : match[1];
}

function matchJobId(path: string): string | null {
  const match = /^\/api\/jobs\/([^/]+)$/.exec(path);
  return match === null ? null : match[1];
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------

function storeRespond<T>(
  result: { status: PlatformStatus; record?: T; error?: string; created?: boolean },
  serialize: (record: T) => unknown,
  extra?: { created?: boolean },
): { status: number; body: unknown } {
  if (result.status === "ok") {
    const body: Record<string, unknown> =
      extra?.created === true
        ? { ...serialize(result.record as T), created: result.created ?? true }
        : serialize(result.record as T);
    return { status: 200, body };
  }
  return {
    status: httpStatus(result.status, storeErrorCode(result.status)),
    body: errorBody(storeErrorCode(result.status), result.error ?? "store failure"),
  };
}

function methodNotAllowed(): Failure {
  // 405 is its own status (the documented contract vocabulary lists it for
  // wrong-method requests; it never flows through the store status mapping).
  return {
    status: 405,
    body: errorBody(ERR_METHOD_NOT_ALLOWED, "method not allowed for this route"),
  };
}

function malformedBody(): Failure {
  return failure("invalid_input", ERR_INVALID_JSON, "request body is not valid JSON");
}

function notFoundRoute(): Failure {
  return { status: 404, body: errorBody(ERR_NOT_FOUND, "no such resource") };
}
