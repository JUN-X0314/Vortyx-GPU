// Platform request router (Phase 11, service surface in Phase 15) — the
// API layer's ONE pipeline.
//
// Every Vercel function in api/ is a thin adapter that feeds its request
// into handlePlatformRequest(); all real logic lives here so the whole
// surface is testable without HTTP. The pipeline is exactly the documented
// contract order:
//
//   1. Route resolution        -> 404 unknown route, 405 wrong method
//   2. Authentication (AuthN)  -> Bearer token verified by the configured
//                                 verifier (user routes) or the worker
//                                 token (worker routes, Phase 15)
//   3. Request parsing (schema)-> 400 malformed JSON, 422 schema violations
//   4. Store operation (AuthZ) -> the store applies the ownership/role rule
//   5. Response serialization  -> 200 with the documented schema, or the
//                                 unified {"error":{code,message}} body
//
// Unauthenticated / forbidden / not-found outcomes come back from the store
// and are mapped with the SAME status mapping the C++ contract pins
// (contract.ts httpStatus + storeErrorCode, service-types.ts
// serviceHttpStatus) — one vocabulary everywhere.

import { createHash, timingSafeEqual } from "node:crypto";

import type { AuthContext, TokenVerifier } from "./auth.ts";
import type { IDistributedStore } from "./distributed.ts";
import { InMemoryDistributedStore } from "./distributed.ts";
import type { IPlatformStore } from "./store.ts";
import {
  errorBody,
  ERR_INTERNAL,
  ERR_INVALID_JSON,
  ERR_METHOD_NOT_ALLOWED,
  ERR_NOT_FOUND,
  ERR_UNAUTHENTICATED,
  httpStatus,
  parseCreateDistributedJob,
  parseCreateJob,
  parseRegisterDevice,
  platformInfo,
  serializeClusterView,
  serializeDevice,
  serializeDistributedJob,
  serializeDistributedShards,
  serializeJob,
  storeErrorCode,
} from "./contract.ts";
import { isValidId } from "./ids.ts";
import {
  parseArtifactRegister,
  parseMemberAdd,
  parsePage,
  parseProjectCreate,
  parseQuota,
  parseSubmitJob,
  parseWorkerClaim,
  parseWorkerComplete,
  parseWorkerHeartbeat,
  serializeArtifact,
  serializeAuditEvent,
  serializeClaimedJob,
  serializeJob as serializeServiceJob,
  serializeMember,
  serializeMetrics,
  serializeProject,
  serializeQuota,
  serializeUsage,
  serviceLimits,
} from "./service-contract.ts";
import { serviceHttpStatus } from "./service-types.ts";
import { InMemoryServiceStore, type IServiceStore } from "./service-store.ts";
import type { ServiceStatus } from "./service-types.ts";
import { PROTOCOL_VERSION } from "./types.ts";

export interface PlatformRequest {
  method: string; // "GET" | "POST" | "PATCH" | ...
  path: string; // e.g. "/api/jobs/abc/cancel" (no query string)
  /** Raw request body. A string that fails JSON.parse becomes 400. */
  body: unknown;
  /** The Authorization header value (may be undefined). */
  authorization?: string;
  /** Parsed query parameters (limit/offset on the paged service routes). */
  query?: Record<string, string>;
}

export interface PlatformResponse {
  status: number;
  body: unknown;
  /** Extra headers (CORS when configured). */
  headers?: Record<string, string>;
}

export interface PlatformDeps {
  store: IPlatformStore;
  verifier: TokenVerifier;
  storeKind: string;
  softwareVersion: string;
  /** Present only to report configuration readiness in /api/health. */
  configError?: string | null;
  /**
   * The Phase 12 distributed record store (cluster/job metadata). When a
   * caller omits it, the router lazily creates an in-memory instance —
   * the documented local/mock behavior; production deployments inject
   * their configured implementation.
   */
  distributed?: IDistributedStore;
  /**
   * The Phase 15 service store (projects/members/jobs/quota/artifacts/
   * audit + worker coordination). Lazily created in-memory when absent
   * (local/mock); production injects the Supabase-backed implementation.
   */
  service?: IServiceStore;
  /**
   * The worker bearer token (server-side secret). When unset the worker
   * endpoints report 503 config_error — the boundary is disabled, never
   * faked.
   */
  workerToken?: string;
  /** The reconcile secret (also accepts the worker token). */
  reconcileToken?: string;
  /** The CORS origin allowed to call this API from a browser ("" = none). */
  allowedOrigin?: string;
}

interface Failure {
  status: number;
  body: unknown;
}

function failure(platformStatus: PlatformStatus, code: string, message: string): Failure {
  const status = httpStatus(platformStatus, code);
  return { status, body: errorBody(code, message) };
}

function serviceFailure(outcome: { status: Exclude<ServiceStatus, "ok">; error: string }): Failure {
  return {
    status: serviceHttpStatus(outcome.status),
    body: errorBody(outcome.status, outcome.error),
  };
}

/**
 * Constant-time secret comparison (the worker/reconcile tokens): both sides
 * are hashed first, so the comparison length is fixed and the token length
 * leaks nothing.
 */
function secretMatches(configured: string | null | undefined, provided: string | null | undefined): boolean {
  if (configured === null || configured === undefined || configured.length === 0) return false;
  if (provided === null || provided === undefined || provided.length === 0) return false;
  const digest = (value: string): Buffer => createHash("sha256").update(value).digest();
  return timingSafeEqual(digest(configured), digest(provided));
}

function bearerOf(authorization: string | undefined): string | null {
  if (authorization === undefined) return null;
  const match = /^Bearer\s+(.+)$/i.exec(authorization.trim());
  return match === null ? null : match[1];
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
    const headers: Record<string, string> = { ...outcome.headers };
    if (deps.allowedOrigin !== undefined && deps.allowedOrigin.length > 0) {
      headers["Access-Control-Allow-Origin"] = deps.allowedOrigin;
      headers["Vary"] = "Origin";
      headers["Access-Control-Allow-Headers"] = "Authorization, Content-Type";
      headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
    }
    return { status: outcome.status, body: outcome.body, headers };
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

  // ---- 0. CORS preflight ---------------------------------------------------
  if (method === "OPTIONS") {
    return { status: 204, body: {} };
  }

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
    return { status: 200, body: { ...platformInfo(deps.softwareVersion), limits: serviceLimits() } };
  }

  // ---- 1b. worker/internal routes (WORKER-TOKEN authenticated) -----------
  // The native execution boundary (Phase 15). A separate trust domain from
  // the user routes: the worker token is a server-side shared secret, not a
  // user identity. When no token is configured the boundary is disabled —
  // 503, never a fake claim.
  if (path === "/api/worker/claim" || path === "/api/internal/reconcile" ||
      /^\/api\/worker\/jobs\/[^/]+\/(heartbeat|complete|fail)$/.test(path)) {
    const token = bearerOf(request.authorization);
    const reconcileRoute = path === "/api/internal/reconcile";
    const tokenOk = secretMatches(deps.workerToken, token) ||
      (reconcileRoute && secretMatches(deps.reconcileToken, token));
    if (!tokenOk) {
      if (deps.workerToken === undefined || deps.workerToken.length === 0) {
        return { status: 503, body: errorBody("config_error", "the worker protocol is not configured on this deployment") };
      }
      return { status: 401, body: errorBody("unauthenticated", "worker token rejected") };
    }
    const service = deps.service ?? (deps.service = new InMemoryServiceStore());
    return dispatchWorker(method, path, request, service);
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

  // The Phase 12 distributed surface (lazy local/mock store when the
  // caller supplied none — see PlatformDeps.distributed).
  const distributed = deps.distributed ?? (deps.distributed = new InMemoryDistributedStore());

  // The Phase 15 service surface (same lazy local/mock rule).
  const service = deps.service ?? (deps.service = new InMemoryServiceStore());

  // ---- Phase 15: the service control-plane surface -----------------------
  if (SERVICE_TARGETS.has(target.kind)) {
    return dispatchService(target as ServiceTarget, request, deps, auth, service);
  }

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

    // ---- Phase 12: the distributed surface --------------------------------

    case "cluster_view": {
      const result = await distributed.clusterView(auth);
      return storeRespond(result, (view) => serializeClusterView(view));
    }

    case "create_distributed_job": {
      const parsed = parseBody(request.body);
      if (!parsed.ok) return malformedBody();
      const parsedJob = parseCreateDistributedJob(parsed.value);
      if (!parsedJob.ok) {
        return {
          status: httpStatus("invalid_input", parsedJob.code),
          body: errorBody(parsedJob.code, parsedJob.message),
        };
      }
      const result = await distributed.createDistributedJob(auth, parsedJob.value);
      return storeRespond(result, (record) => serializeDistributedJob(record), {
        created: true,
      });
    }

    case "distributed_jobs_list": {
      const result = await distributed.distributedJobs(auth);
      return storeRespond(result, (records) => ({
        jobs: records.map(serializeDistributedJob),
      }));
    }

    case "distributed_job_detail": {
      const result = await distributed.distributedJob(auth, target.id);
      return storeRespond(result, (record) => serializeDistributedJob(record));
    }

    case "distributed_job_shards": {
      const result = await distributed.distributedJob(auth, target.id);
      return storeRespond(result, (record) => serializeDistributedShards(record));
    }

    case "cancel_distributed_job": {
      const result = await distributed.cancelDistributedJob(auth, target.id);
      return storeRespond(result, (record) => serializeDistributedJob(record));
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
  | { kind: "cluster_view" }
  | { kind: "create_distributed_job" }
  | { kind: "distributed_jobs_list" }
  | { kind: "distributed_job_detail"; id: string }
  | { kind: "distributed_job_shards"; id: string }
  | { kind: "cancel_distributed_job"; id: string }
  // Phase 15: the service control-plane surface.
  | { kind: "me" }
  | { kind: "projects_create" }
  | { kind: "projects_list" }
  | { kind: "project_detail"; id: string }
  | { kind: "project_archive"; id: string }
  | { kind: "project_members"; id: string }
  | { kind: "project_member"; id: string; user: string }
  | { kind: "project_quota"; id: string }
  | { kind: "project_usage"; id: string }
  | { kind: "project_jobs"; id: string }
  | { kind: "project_audit"; id: string }
  | { kind: "project_artifacts"; id: string }
  | { kind: "service_jobs_list" }
  | { kind: "service_job_detail"; id: string }
  | { kind: "service_job_cancel"; id: string }
  | { kind: "service_artifact"; id: string }
  | { kind: "audit_tail" }
  | { kind: "metrics" }
  | { kind: "not_found" }
  | { kind: "method_not_allowed" };

/** The service-surface target kinds (routed to dispatchService). */
const SERVICE_TARGETS: ReadonlySet<string> = new Set([
  "me",
  "projects_create",
  "projects_list",
  "project_detail",
  "project_archive",
  "project_members",
  "project_member",
  "project_quota",
  "project_usage",
  "project_jobs",
  "project_audit",
  "project_artifacts",
  "service_jobs_list",
  "service_job_detail",
  "service_job_cancel",
  "service_artifact",
  "audit_tail",
  "metrics",
]);

type ServiceTarget = Extract<Target, { kind: "me" | "projects_create" | "projects_list" | "project_detail" | "project_archive" | "project_members" | "project_member" | "project_quota" | "project_usage" | "project_jobs" | "project_audit" | "project_artifacts" | "service_jobs_list" | "service_job_detail" | "service_job_cancel" | "service_artifact" | "audit_tail" | "metrics" }>;

function resolveTarget(method: string, path: string): Target {
  // Phase 15 service routes first (fixed paths; the dynamic matchers below
  // cannot collide with them).
  switch (path) {
    case "/api/me":
      return method === "GET" ? { kind: "me" } : { kind: "method_not_allowed" };
    case "/api/projects":
      if (method === "POST") return { kind: "projects_create" };
      if (method === "GET") return { kind: "projects_list" };
      return { kind: "method_not_allowed" };
    case "/api/jobs":
      // The Phase 11 direct job surface — BYTE-COMPATIBLE, never remapped
      // (service jobs live under /api/service/jobs and /api/projects/:id/jobs).
      if (method === "POST") return { kind: "create_job" };
      if (method === "GET") return { kind: "jobs_list" };
      return { kind: "method_not_allowed" };
    case "/api/service/jobs":
      if (method === "GET") return { kind: "service_jobs_list" };
      return { kind: "method_not_allowed" };
    case "/api/audit":
      return method === "GET" ? { kind: "audit_tail" } : { kind: "method_not_allowed" };
    case "/api/metrics":
      return method === "GET" ? { kind: "metrics" } : { kind: "method_not_allowed" };
    default:
      break;
  }

  // Dynamic project routes.
  const projectSub = matchProjectSubresource(path);
  if (projectSub !== null) {
    const { id, sub } = projectSub;
    if (sub === "") {
      if (method === "GET") return { kind: "project_detail", id };
      return { kind: "method_not_allowed" };
    }
    if (sub === "archive") {
      if (method === "POST") return { kind: "project_archive", id };
      return { kind: "method_not_allowed" };
    }
    if (sub === "members") {
      if (method === "GET") return { kind: "project_members", id };
      if (method === "POST") return { kind: "project_members", id }; // POST adds (same route kind)
      return { kind: "method_not_allowed" };
    }
    if (sub === "quota") {
      if (method === "GET") return { kind: "project_quota", id };
      if (method === "PUT") return { kind: "project_quota", id };
      return { kind: "method_not_allowed" };
    }
    if (sub === "usage") {
      if (method === "GET") return { kind: "project_usage", id };
      return { kind: "method_not_allowed" };
    }
    if (sub === "jobs") {
      if (method === "GET") return { kind: "project_jobs", id };
      if (method === "POST") return { kind: "project_jobs", id }; // POST submits
      return { kind: "method_not_allowed" };
    }
    if (sub === "audit") {
      if (method === "GET") return { kind: "project_audit", id };
      return { kind: "method_not_allowed" };
    }
    if (sub === "artifacts") {
      if (method === "GET") return { kind: "project_artifacts", id };
      if (method === "POST") return { kind: "project_artifacts", id }; // POST registers
      return { kind: "method_not_allowed" };
    }
    return { kind: "not_found" };
  }

  const memberRoute = matchProjectMember(path);
  if (memberRoute !== null) {
    if (method !== "DELETE") return { kind: "method_not_allowed" };
    return isValidId(memberRoute.project) && isValidId(memberRoute.user)
      ? { kind: "project_member", id: memberRoute.project, user: memberRoute.user }
      : { kind: "not_found" };
  }

  const artifactRoute = matchServiceArtifact(path);
  if (artifactRoute !== null) {
    if (method !== "DELETE") return { kind: "method_not_allowed" };
    return isValidId(artifactRoute)
      ? { kind: "service_artifact", id: artifactRoute }
      : { kind: "not_found" };
  }

  switch (path) {
    case "/api/devices/register":
      return method === "POST" ? { kind: "register" } : { kind: "method_not_allowed" };
    case "/api/devices":
      return method === "GET" ? { kind: "devices_list" } : { kind: "method_not_allowed" };
    // Phase 12: the distributed surface (resolved before the dynamic
    // matchers below so /api/distributed/... can never collide with them).
    case "/api/cluster":
      return method === "GET" ? { kind: "cluster_view" } : { kind: "method_not_allowed" };
    case "/api/distributed/jobs":
      if (method === "POST") return { kind: "create_distributed_job" };
      if (method === "GET") return { kind: "distributed_jobs_list" };
      return { kind: "method_not_allowed" };
    default:
      break;
  }

  // Dynamic segments. Ids are charset-restricted; a segment outside the
  // charset can never name a real resource -> not_found (no decoding, no
  // exception path).
  const distributedShards = matchDistributedShards(path);
  if (distributedShards !== null) {
    if (method !== "GET") return { kind: "method_not_allowed" };
    return isValidId(distributedShards)
      ? { kind: "distributed_job_shards", id: distributedShards }
      : { kind: "not_found" };
  }
  const distributedCancel = matchDistributedCancel(path);
  if (distributedCancel !== null) {
    if (method !== "POST") return { kind: "method_not_allowed" };
    return isValidId(distributedCancel)
      ? { kind: "cancel_distributed_job", id: distributedCancel }
      : { kind: "not_found" };
  }
  const distributedJob = matchDistributedJobId(path);
  if (distributedJob !== null) {
    if (method !== "GET") return { kind: "method_not_allowed" };
    return isValidId(distributedJob)
      ? { kind: "distributed_job_detail", id: distributedJob }
      : { kind: "not_found" };
  }

  // Phase 15 service job routes (the /api/service/jobs namespace — the
  // Phase 11 /api/jobs routes keep their documented shapes untouched).
  const serviceCancel = matchServiceJobCancel(path);
  if (serviceCancel !== null) {
    if (method !== "POST") return { kind: "method_not_allowed" };
    return isValidId(serviceCancel)
      ? { kind: "service_job_cancel", id: serviceCancel }
      : { kind: "not_found" };
  }
  const serviceJob = matchServiceJobId(path);
  if (serviceJob !== null) {
    if (method !== "GET") return { kind: "method_not_allowed" };
    return isValidId(serviceJob)
      ? { kind: "service_job_detail", id: serviceJob }
      : { kind: "not_found" };
  }

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

const PROJECT_SUB_PATH = /^\/api\/projects\/([^/]+)(?:\/([a-z]+))?$/;

function matchProjectSubresource(path: string): { id: string; sub: string } | null {
  const match = PROJECT_SUB_PATH.exec(path);
  if (match === null) return null;
  return { id: match[1], sub: match[2] ?? "" };
}

const PROJECT_MEMBER_PATH = /^\/api\/projects\/([^/]+)\/members\/([^/]+)$/;

function matchProjectMember(path: string): { project: string; user: string } | null {
  const match = PROJECT_MEMBER_PATH.exec(path);
  if (match === null) return null;
  return { project: match[1], user: match[2] };
}

const SERVICE_ARTIFACT_PATH = /^\/api\/artifacts\/([^/]+)$/;

function matchServiceArtifact(path: string): string | null {
  const match = SERVICE_ARTIFACT_PATH.exec(path);
  return match === null ? null : match[1];
}

const SERVICE_JOB_CANCEL_PATH = /^\/api\/service\/jobs\/([^/]+)\/cancel$/;
const SERVICE_JOB_ID_PATH = /^\/api\/service\/jobs\/([^/]+)$/;

function matchServiceJobCancel(path: string): string | null {
  const match = SERVICE_JOB_CANCEL_PATH.exec(path);
  return match === null ? null : match[1];
}

function matchServiceJobId(path: string): string | null {
  const match = SERVICE_JOB_ID_PATH.exec(path);
  return match === null ? null : match[1];
}

function matchDistributedShards(path: string): string | null {
  const match = /^\/api\/distributed\/jobs\/([^/]+)\/shards$/.exec(path);
  return match === null ? null : match[1];
}

function matchDistributedCancel(path: string): string | null {
  const match = /^\/api\/distributed\/jobs\/([^/]+)\/cancel$/.exec(path);
  return match === null ? null : match[1];
}

function matchDistributedJobId(path: string): string | null {
  const match = /^\/api\/distributed\/jobs\/([^/]+)$/.exec(path);
  return match === null ? null : match[1];
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

// ---------------------------------------------------------------------------
// Phase 15: the service control-plane surface
// ---------------------------------------------------------------------------

function parseFailed(failure: { ok: false; code: string; message: string }): Failure {
  return {
    status: httpStatus("invalid_input", failure.code),
    body: errorBody(failure.code, failure.message),
  };
}

async function dispatchService(
  target: ServiceTarget,
  request: PlatformRequest,
  deps: PlatformDeps,
  auth: AuthContext,
  service: IServiceStore,
): Promise<Failure | { status: number; body: unknown }> {
  const method = request.method.toUpperCase();
  const query = request.query ?? {};

  switch (target.kind) {
    case "me":
      // The verified subject (no client-claimed identity ever reaches here).
      return { status: 200, body: { user_id: auth.user_id } };

    case "projects_create": {
      const parsed = parseBody(request.body);
      if (!parsed.ok) return malformedBody();
      const request_ = parseProjectCreate(parsed.value);
      if (!request_.ok) return parseFailed(request_);
      const result = await service.createProject(auth, request_.value.name);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: serializeProject(result.record) };
    }

    case "projects_list": {
      const result = await service.projects(auth);
      if (result.status !== "ok") return serviceFailure(result);
      return {
        status: 200,
        body: { projects: result.record.map((p) => ({ ...serializeProject(p), role: p.role })) },
      };
    }

    case "project_detail": {
      const result = await service.project(auth, target.id);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: serializeProject(result.record) };
    }

    case "project_archive": {
      const result = await service.archiveProject(auth, target.id);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: serializeProject(result.record) };
    }

    case "project_members": {
      if (method === "GET") {
        const result = await service.members(auth, target.id);
        if (result.status !== "ok") return serviceFailure(result);
        return { status: 200, body: { members: result.record.map(serializeMember) } };
      }
      const parsed = parseBody(request.body);
      if (!parsed.ok) return malformedBody();
      const request_ = parseMemberAdd(parsed.value);
      if (!request_.ok) return parseFailed(request_);
      const result = await service.addMember(auth, target.id, request_.value.user_id,
        request_.value.role as Parameters<IServiceStore["addMember"]>[3]);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: serializeMember(result.record) };
    }

    case "project_member": {
      const result = await service.removeMember(auth, target.id, target.user);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: { removed: true } };
    }

    case "project_quota": {
      if (method === "GET") {
        const result = await service.quotaPolicy(auth, target.id);
        if (result.status !== "ok") return serviceFailure(result);
        return { status: 200, body: serializeQuota(result.record) };
      }
      const parsed = parseBody(request.body);
      if (!parsed.ok) return malformedBody();
      const request_ = parseQuota(parsed.value);
      if (!request_.ok) return parseFailed(request_);
      const result = await service.setQuota(auth, target.id, request_.value);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: serializeQuota(result.record) };
    }

    case "project_usage": {
      const result = await service.usage(auth, target.id);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: serializeUsage(result.record) };
    }

    case "project_jobs": {
      if (method === "GET") {
        const page = parsePage(query);
        const result = await service.jobs(auth, target.id, page.limit, page.offset);
        if (result.status !== "ok") return serviceFailure(result);
        return {
          status: 200,
          body: { jobs: result.record.items.map(serializeServiceJob), next_offset: result.record.next_offset },
        };
      }
      const parsed = parseBody(request.body);
      if (!parsed.ok) return malformedBody();
      const request_ = parseSubmitJob(parsed.value);
      if (!request_.ok) return parseFailed(request_);
      const result = await service.submitJob(auth, target.id, request_.value);
      if (result.status !== "ok") return serviceFailure(result);
      return {
        status: 200,
        body: { ...serializeServiceJob(result.record), created: result.created ?? true },
      };
    }

    case "project_audit": {
      const page = parsePage(query);
      const result = await service.projectAudit(auth, target.id, page.limit);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: { events: result.record.map(serializeAuditEvent) } };
    }

    case "project_artifacts": {
      if (method === "GET") {
        const result = await service.artifacts(auth, target.id);
        if (result.status !== "ok") return serviceFailure(result);
        return { status: 200, body: { artifacts: result.record.map(serializeArtifact) } };
      }
      const parsed = parseBody(request.body);
      if (!parsed.ok) return malformedBody();
      const request_ = parseArtifactRegister(parsed.value);
      if (!request_.ok) return parseFailed(request_);
      const result = await service.registerArtifact(auth, target.id, request_.value.name,
        request_.value.declared_byte_size);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: serializeArtifact(result.record) };
    }

    case "service_jobs_list": {
      const page = parsePage(query);
      const filter = query["project_id"] ?? null;
      const result = await service.jobs(auth, filter, page.limit, page.offset);
      if (result.status !== "ok") return serviceFailure(result);
      return {
        status: 200,
        body: { jobs: result.record.items.map(serializeServiceJob), next_offset: result.record.next_offset },
      };
    }

    case "service_job_detail": {
      const result = await service.job(auth, target.id);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: serializeServiceJob(result.record) };
    }

    case "service_job_cancel": {
      const result = await service.cancelJob(auth, target.id);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: serializeServiceJob(result.record) };
    }

    case "service_artifact": {
      const result = await service.deleteArtifact(auth, target.id);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: { deleted: true } };
    }

    case "audit_tail": {
      const page = parsePage(query);
      const result = await service.auditTail(auth, page.limit);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: { events: result.record.map(serializeAuditEvent) } };
    }

    case "metrics": {
      const result = await service.metrics(auth);
      if (result.status !== "ok") return serviceFailure(result);
      return { status: 200, body: serializeMetrics(result.record) };
    }
  }
}

// ---------------------------------------------------------------------------
// Phase 15: the native worker protocol (worker-token authenticated)
// ---------------------------------------------------------------------------

async function dispatchWorker(
  method: string,
  path: string,
  request: PlatformRequest,
  service: IServiceStore,
): Promise<Failure | { status: number; body: unknown }> {
  if (path === "/api/internal/reconcile") {
    if (method !== "POST" && method !== "GET") return methodNotAllowed();
    const recovered = await service.reconcile();
    return { status: 200, body: { recovered_stale_jobs: recovered } };
  }

  if (path === "/api/worker/claim") {
    if (method !== "POST") return methodNotAllowed();
    const parsed = parseBody(request.body);
    if (!parsed.ok) return malformedBody();
    const request_ = parseWorkerClaim(parsed.value);
    if (!request_.ok) return parseFailed(request_);
    const outcome = await service.workerClaim(request_.value.worker_id, request_.value.lease_ms);
    if (!outcome.ok) {
      return { status: 422, body: errorBody("invalid_value", outcome.error ?? "claim refused") };
    }
    if (outcome.job === null || outcome.job === undefined) {
      return { status: 200, body: { claimed: false } };
    }
    return { status: 200, body: { claimed: true, job: serializeClaimedJob(outcome.job) } };
  }

  const jobMatch = /^\/api\/worker\/jobs\/([^/]+)\/(heartbeat|complete|fail)$/.exec(path);
  if (jobMatch === null) return notFoundRoute();
  const jobId = jobMatch[1];
  const action = jobMatch[2];
  if (!isValidId(jobId)) return notFoundRoute();
  if (method !== "POST") return methodNotAllowed();

  const parsed = parseBody(request.body);
  if (!parsed.ok) return malformedBody();

  if (action === "heartbeat") {
    const request_ = parseWorkerHeartbeat(parsed.value);
    if (!request_.ok) return parseFailed(request_);
    const outcome = await service.workerHeartbeat(request_.value.worker_id, jobId, 60000);
    if (!outcome.ok) {
      return { status: 422, body: errorBody("invalid_value", outcome.error ?? "heartbeat refused") };
    }
    if (!outcome.accepted) {
      return { status: 409, body: errorBody("conflict", "the job is not claimed by this worker") };
    }
    return {
      status: 200,
      body: {
        accepted: true,
        cancel_requested: outcome.cancel_requested ?? false,
        lease_expires_at_ms: outcome.lease_expires_at_ms ?? null,
      },
    };
  }

  // complete | fail (fail is complete with status forced to failed).
  const request_ = parseWorkerComplete(parsed.value);
  if (!request_.ok) return parseFailed(request_);
  const report = request_.value;
  const status = action === "fail" ? "failed" : report.status;
  if (action === "fail" && report.status !== "failed") {
    return {
      status: 422,
      body: errorBody("invalid_value", "the /fail endpoint requires status 'failed'"),
    };
  }
  const outcome = await service.workerComplete(report.worker_id, jobId, {
    status,
    error: report.error,
    backend: report.backend,
    result_element_count: report.result_element_count,
    shards_total: report.shards_total ?? undefined,
    shards_succeeded: report.shards_succeeded ?? undefined,
    shards_failed: report.shards_failed ?? undefined,
  });
  if (!outcome.ok) {
    const notClaimed = (outcome.error ?? "").includes("not claimed");
    return {
      status: notClaimed ? 409 : 422,
      body: errorBody(notClaimed ? "conflict" : "invalid_value", outcome.error ?? "report refused"),
    };
  }
  return { status: 200, body: { recorded: outcome.recorded ?? false, status: outcome.status } };
}
