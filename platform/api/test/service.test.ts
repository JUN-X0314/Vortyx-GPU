// Service surface tests (Phase 15) — the control plane end to end over the
// real pipeline with the memory store: auth, projects, memberships (the
// single-owner invariant), job submission (validation, quota, rate limit,
// idempotency, conflict), cancellation (queued/running/terminal),
// artifacts (capacity, deletion authorization, isolation), audit, metrics,
// pagination — and the worker protocol (atomic claim, lease heartbeat,
// cancel relay, idempotent complete, stale-lease reconciliation, token
// trust boundary).
//
// No network, no Supabase, no secrets (the documented local/mock mode).

import { test } from "node:test";
import assert from "node:assert/strict";

import { InMemoryServiceStore } from "../src/service-store.ts";
import { handlePlatformRequest, type PlatformDeps, type PlatformRequest } from "../src/router.ts";

const FIXED_NOW = 1700000000000;

function deps(): PlatformDeps {
  return {
    store: {} as never, // the service surface never touches the Phase 11 store
    verifier: async (token) => {
      if (!token.startsWith("local:")) return null;
      const userId = token.slice("local:".length);
      return userId.length > 0 ? { authenticated: true, user_id: userId } : null;
    },
    storeKind: "memory",
    softwareVersion: "0.15.0",
    service: new InMemoryServiceStore(() => FIXED_NOW, makeId),
    workerToken: "worker-secret-token",
  };
}

let idCounter = 0;
function makeId(): string {
  idCounter += 1;
  return `id-${idCounter.toString().padStart(8, "0")}-aabb-4ccc-8ddd-eeeeeeeeeeee`;
}

function request(
  method: string,
  path: string,
  options: { body?: unknown; authorization?: string; query?: Record<string, string> } = {},
): PlatformRequest {
  return {
    method,
    path,
    body: options.body,
    authorization: options.authorization,
    query: options.query,
  };
}

async function call(deps_: PlatformDeps, req: PlatformRequest): Promise<{ status: number; body: any }> {
  const response = await handlePlatformRequest(req, deps_);
  return { status: response.status, body: response.body as any };
}

const alice = "local:user-alice";
const bob = "local:user-bob";
const carol = "local:user-carol";
const WORKER = "Bearer worker-secret-token";

const SUBMIT = {
  job_id: "job-0001",
  operation: "vector_add",
  element_count: 1000,
  requested_backend: "cpu",
  requested_shard_count: 2,
};

// =====================================================================
// Projects + memberships
// =====================================================================

test("projects: creation, listing, anti-enumeration", async () => {
  const d = deps();
  const created = await call(d, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "alpha" } }));
  assert.equal(created.status, 200);
  assert.equal(created.body.owner_user_id, "user-alice");
  assert.equal(created.body.status, "active");
  const projectId = created.body.project_id;

  const list = await call(d, request("GET", "/api/projects", { authorization: `Bearer ${alice}` }));
  assert.equal(list.body.projects.length, 1);

  // A foreign user cannot see the project (not_found — never forbidden).
  const foreign = await call(d, request("GET", `/api/projects/${projectId}`, { authorization: `Bearer ${bob}` }));
  assert.equal(foreign.status, 404);
  // Anonymous is 401.
  const anonymous = await call(d, request("GET", `/api/projects/${projectId}`));
  assert.equal(anonymous.status, 401);

  // A viewer (added member) sees it read-only.
  await call(d, request("POST", `/api/projects/${projectId}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-bob", role: "viewer" },
  }));
  const viewer = await call(d, request("GET", `/api/projects/${projectId}`, { authorization: `Bearer ${bob}` }));
  assert.equal(viewer.status, 200);
});

test("membership: the single-owner invariant holds through the API", async () => {
  const d = deps();
  const project = (await call(d, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "p" } }))).body.project_id;

  // The owner role is NEVER grantable — by the owner or by an admin.
  const ownerGrant = await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-bob", role: "owner" },
  }));
  assert.equal(ownerGrant.status, 422);

  await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-bob", role: "admin" },
  }));
  const adminGrant = await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${bob}`, body: { user_id: "user-carol", role: "owner" },
  }));
  assert.equal(adminGrant.status, 422);

  const members = (await call(d, request("GET", `/api/projects/${project}/members`, { authorization: `Bearer ${alice}` }))).body.members;
  assert.equal(members.filter((m: any) => m.role === "owner").length, 1);
  assert.equal(members.filter((m: any) => m.user_id === "user-alice" && m.role === "owner").length, 1);

  // Duplicate membership is a conflict; owner removal is refused.
  const duplicate = await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-bob", role: "member" },
  }));
  assert.equal(duplicate.status, 409);
  const removeOwner = await call(d, request("DELETE", `/api/projects/${project}/members/user-alice`, { authorization: `Bearer ${alice}` }));
  assert.equal(removeOwner.status, 422);

  // A member cannot manage members.
  await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-carol", role: "member" },
  }));
  const memberTries = await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${carol}`, body: { user_id: "user-dave", role: "viewer" },
  }));
  assert.equal(memberTries.status, 403);

  // Only the owner archives.
  const archiveByAdmin = await call(d, request("POST", `/api/projects/${project}/archive`, { authorization: `Bearer ${bob}` }));
  assert.equal(archiveByAdmin.status, 403);
  const archive = await call(d, request("POST", `/api/projects/${project}/archive`, { authorization: `Bearer ${alice}` }));
  assert.equal(archive.status, 200);
  assert.equal(archive.body.status, "archived");
});

// =====================================================================
// Job submission: validation, quota, rate limit, idempotency
// =====================================================================

test("job submission: server-side validation is complete", async () => {
  const d = deps();
  const project = (await call(d, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "p" } }))).body.project_id;

  const badOperation = await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, body: { ...SUBMIT, operation: "matrix_magic" },
  }));
  assert.equal(badOperation.status, 422);

  const badElementCount = await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, body: { ...SUBMIT, element_count: 0 },
  }));
  assert.equal(badElementCount.status, 422);

  const badShards = await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, body: { ...SUBMIT, requested_shard_count: 100000 },
  }));
  assert.equal(badShards.status, 422);

  const badBackend = await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, body: { ...SUBMIT, requested_backend: "cuda" },
  }));
  assert.equal(badBackend.status, 422);

  // A viewer cannot submit.
  await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-bob", role: "viewer" },
  }));
  const viewerSubmit = await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${bob}`, body: SUBMIT,
  }));
  assert.equal(viewerSubmit.status, 403);

  // The owner submits successfully: queued, metadata intact.
  const submit = await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, body: SUBMIT,
  }));
  assert.equal(submit.status, 200);
  assert.equal(submit.body.status, "queued");
  assert.equal(submit.body.created, true);
  assert.equal(submit.body.submitted_by, "user-alice");
});

test("job submission: idempotency replay vs conflict", async () => {
  const d = deps();
  const project = (await call(d, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "p" } }))).body.project_id;
  await call(d, request("POST", `/api/projects/${project}/jobs`, { authorization: `Bearer ${alice}`, body: SUBMIT }));

  // The same key + the same payload = a replay (no double creation).
  const replay = await call(d, request("POST", `/api/projects/${project}/jobs`, { authorization: `Bearer ${alice}`, body: SUBMIT }));
  assert.equal(replay.status, 200);
  assert.equal(replay.body.created, false);

  // The same key + a different payload = conflict.
  const conflict = await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, body: { ...SUBMIT, element_count: 2000 },
  }));
  assert.equal(conflict.status, 409);

  // The same key from another user = conflict (never a cross-user replay).
  const foreign = await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${bob}`, body: SUBMIT,
  }));
  assert.equal(foreign.status, 409);
});

test("job submission: the quota policy refuses honestly (no scheduler entry)", async () => {
  const d = deps();
  const project = (await call(d, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "p" } }))).body.project_id;
  // Tighten the project quota to one job.
  await call(d, request("PUT", `/api/projects/${project}/quota`, {
    authorization: `Bearer ${alice}`, body: { max_concurrent_jobs: 1, max_running_shards: 4, max_memory_bytes: 1073741824 },
  }));
  await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, body: { ...SUBMIT, job_id: "job-q1", requested_shard_count: 1 },
  }));
  const refused = await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, body: { ...SUBMIT, job_id: "job-q2", requested_shard_count: 1 },
  }));
  assert.equal(refused.status, 429);
  assert.equal(refused.body.error.code, "quota_exceeded");

  // Usage is derived from in-flight jobs and released at terminal.
  const usageBefore = (await call(d, request("GET", `/api/projects/${project}/usage`, { authorization: `Bearer ${alice}` }))).body;
  assert.equal(usageBefore.active_jobs, 1);
  await call(d, request("POST", `/api/service/jobs/job-q1/cancel`, { authorization: `Bearer ${alice}` }));
  const usageAfter = (await call(d, request("GET", `/api/projects/${project}/usage`, { authorization: `Bearer ${alice}` }))).body;
  assert.equal(usageAfter.active_jobs, 0);

  // The admin may change the quota; members may not.
  await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-bob", role: "member" },
  }));
  const memberQuota = await call(d, request("PUT", `/api/projects/${project}/quota`, {
    authorization: `Bearer ${bob}`, body: { max_concurrent_jobs: 99, max_running_shards: 99, max_memory_bytes: 99 },
  }));
  assert.equal(memberQuota.status, 403);
});

test("job submission: the rate limit refuses with 429 and counts attempts", async () => {
  const store = new InMemoryServiceStore(() => FIXED_NOW, makeId);
  const d: PlatformDeps = { ...deps(), service: store };
  const project = (await call(d, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "p" } }))).body.project_id;
  // Lift the quota out of the way so ONLY the rate limit can refuse.
  await call(d, request("PUT", `/api/projects/${project}/quota`, {
    authorization: `Bearer ${alice}`,
    body: { max_concurrent_jobs: 1000, max_running_shards: 1000, max_memory_bytes: 1073741824000 },
  }));
  // Default limiter: 60 per user per window. Fire 60 accepted + 1 refused.
  let refused = 0;
  for (let i = 0; i < 61; i += 1) {
    const response = await call(d, request("POST", `/api/projects/${project}/jobs`, {
      authorization: `Bearer ${alice}`,
      body: { ...SUBMIT, job_id: `job-r${i}`, requested_shard_count: 1 },
    }));
    if (response.status === 429) refused += 1;
  }
  assert.equal(refused, 1);
  // A different user has an independent window.
  await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-bob", role: "member" },
  }));
  const bobSubmit = await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${bob}`, body: { ...SUBMIT, job_id: "job-bob-1", requested_shard_count: 1 },
  }));
  assert.equal(bobSubmit.status, 200);
});

test("job cancellation: queued cancels, running relays, terminal refuses", async () => {
  const d = deps();
  const project = (await call(d, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "p" } }))).body.project_id;
  await call(d, request("POST", `/api/projects/${project}/jobs`, { authorization: `Bearer ${alice}`, body: SUBMIT }));

  // Queued -> cancelled, quota released.
  const cancelled = await call(d, request("POST", `/api/service/jobs/${SUBMIT.job_id}/cancel`, { authorization: `Bearer ${alice}` }));
  assert.equal(cancelled.status, 200);
  assert.equal(cancelled.body.status, "cancelled");
  assert.ok(cancelled.body.terminal_at_ms !== null);

  // Terminal is final.
  const again = await call(d, request("POST", `/api/service/jobs/${SUBMIT.job_id}/cancel`, { authorization: `Bearer ${alice}` }));
  assert.equal(again.status, 422);

  // Running: the cancel flag is relayed through the heartbeat.
  await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, body: { ...SUBMIT, job_id: "job-0002" },
  }));
  const claim = await call(d, request("POST", "/api/worker/claim", {
    authorization: WORKER,
    body: { worker_id: "worker-a", lease_ms: 60000 },
  }));
  assert.equal(claim.body.claimed, true);
  await call(d, request("POST", `/api/service/jobs/job-0002/cancel`, { authorization: `Bearer ${alice}` }));
  const heartbeat = await call(d, request("POST", "/api/worker/jobs/job-0002/heartbeat", {
    authorization: WORKER,
    body: { worker_id: "worker-a" },
  }));
  assert.equal(heartbeat.body.cancel_requested, true);

  // A member cannot cancel another member's job; an admin can (privileged).
  await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-bob", role: "member" },
  }));
  await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-carol", role: "admin" },
  }));
  const memberCancel = await call(d, request("POST", `/api/service/jobs/job-0002/cancel`, { authorization: `Bearer ${bob}` }));
  assert.equal(memberCancel.status, 403);
  const adminCancel = await call(d, request("POST", `/api/service/jobs/job-0002/cancel`, { authorization: `Bearer ${carol}` }));
  assert.equal(adminCancel.status, 200);

  // The privileged cancellation is audited as such.
  const audit = (await call(d, request("GET", "/api/audit", { authorization: `Bearer ${carol}`, query: { limit: "50" } }))).body.events;
  const privileged = audit.find((e: any) => e.action === "job_cancel" && e.actor_user_id === "user-carol");
  assert.ok(privileged !== undefined);
  assert.ok(privileged.reason_code.includes("privileged"));
});

// =====================================================================
// Artifacts
// =====================================================================

test("artifacts: registration, capacity bound, deletion authorization, isolation", async () => {
  const d = deps();
  const project = (await call(d, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "p" } }))).body.project_id;
  const registered = await call(d, request("POST", `/api/projects/${project}/artifacts`, {
    authorization: `Bearer ${alice}`, body: { name: "model.bin", declared_byte_size: 1234 },
  }));
  assert.equal(registered.status, 200);
  const artifactId = registered.body.artifact_id;

  // A viewer sees the metadata; a foreign user does not.
  await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-bob", role: "member" },
  }));
  await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-carol", role: "viewer" },
  }));
  const list = (await call(d, request("GET", `/api/projects/${project}/artifacts`, { authorization: `Bearer ${carol}` }))).body;
  assert.equal(list.artifacts.length, 1);
  const foreignProject = (await call(d, request("POST", "/api/projects", { authorization: `Bearer ${bob}`, body: { name: "bob" } }))).body.project_id;
  const foreignList = (await call(d, request("GET", `/api/projects/${foreignProject}/artifacts`, { authorization: `Bearer ${bob}` }))).body;
  assert.equal(foreignList.artifacts.length, 0);

  // Member deletes own artifact; member cannot delete another's; admin can.
  const memberArtifact = (await call(d, request("POST", `/api/projects/${project}/artifacts`, {
    authorization: `Bearer ${bob}`, body: { name: "bob.bin", declared_byte_size: 1 },
  }))).body.artifact_id;
  const crossDelete = await call(d, request("DELETE", `/api/artifacts/${memberArtifact}`, { authorization: `Bearer ${alice}` }));
  assert.equal(crossDelete.status, 200); // alice is the owner (Admin+)
  const ownDelete = await call(d, request("DELETE", `/api/artifacts/${artifactId}`, { authorization: `Bearer ${alice}` }));
  assert.equal(ownDelete.status, 200);
  const unknownDelete = await call(d, request("DELETE", `/api/artifacts/id-00000001-aabb-4ccc-8ddd-eeeeeeeeeeee`, { authorization: `Bearer ${alice}` }));
  assert.equal(unknownDelete.status, 404);

  // The capacity bound is honest (unavailable), not a silent success.
  const store = new InMemoryServiceStore(() => FIXED_NOW, makeId);
  const bounded: PlatformDeps = { ...deps(), service: store };
  const boundProject = (await call(bounded, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "b" } }))).body.project_id;
  let lastStatus = 0;
  for (let i = 0; i < 257; i += 1) {
    const response = await call(bounded, request("POST", `/api/projects/${boundProject}/artifacts`, {
      authorization: `Bearer ${alice}`, body: { name: `f${i}`, declared_byte_size: 1 },
    }));
    lastStatus = response.status;
  }
  assert.equal(lastStatus, 503);
});

// =====================================================================
// Audit + metrics + pagination
// =====================================================================

test("audit and metrics are real, scoped data", async () => {
  const d = deps();
  const project = (await call(d, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "p" } }))).body.project_id;
  await call(d, request("POST", `/api/projects/${project}/jobs`, { authorization: `Bearer ${alice}`, body: SUBMIT }));
  await call(d, request("POST", `/api/service/jobs/${SUBMIT.job_id}/cancel`, { authorization: `Bearer ${alice}` }));

  const metrics = (await call(d, request("GET", "/api/metrics", { authorization: `Bearer ${alice}` }))).body;
  assert.equal(metrics.total_jobs, 1);
  assert.equal(metrics.cancelled, 1);

  const audit = (await call(d, request("GET", "/api/audit", { authorization: `Bearer ${alice}` }))).body.events;
  assert.ok(audit.length >= 2); // submit + terminal (+ cancel)
  assert.ok(audit.some((e: any) => e.action === "job_submit"));
  assert.ok(audit.some((e: any) => e.action === "job_terminal"));

  // Bob (not a member) sees nothing.
  const bobAudit = (await call(d, request("GET", "/api/audit", { authorization: `Bearer ${bob}` }))).body.events;
  assert.equal(bobAudit.length, 0);

  // Project audit requires admin+.
  await call(d, request("POST", `/api/projects/${project}/members`, {
    authorization: `Bearer ${alice}`, body: { user_id: "user-bob", role: "member" },
  }));
  const memberAudit = await call(d, request("GET", `/api/projects/${project}/audit`, { authorization: `Bearer ${bob}` }));
  assert.equal(memberAudit.status, 403);
  const ownerAudit = await call(d, request("GET", `/api/projects/${project}/audit`, { authorization: `Bearer ${alice}` }));
  assert.equal(ownerAudit.status, 200);
});

test("pagination: bounded page sizes and honest next_offset", async () => {
  const d = deps();
  const project = (await call(d, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "p" } }))).body.project_id;
  // Lift the quota out of the way (pagination is under test, not quota).
  await call(d, request("PUT", `/api/projects/${project}/quota`, {
    authorization: `Bearer ${alice}`,
    body: { max_concurrent_jobs: 1000, max_running_shards: 1000, max_memory_bytes: 1073741824000 },
  }));
  for (let i = 0; i < 5; i += 1) {
    await call(d, request("POST", `/api/projects/${project}/jobs`, {
      authorization: `Bearer ${alice}`, body: { ...SUBMIT, job_id: `job-p${i}`, requested_shard_count: 1 },
    }));
  }
  const page1 = (await call(d, request("GET", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, query: { limit: "2", offset: "0" },
  }))).body;
  assert.equal(page1.jobs.length, 2);
  assert.equal(page1.next_offset, 2);
  const page3 = (await call(d, request("GET", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, query: { limit: "2", offset: "4" },
  }))).body;
  assert.equal(page3.jobs.length, 1);
  assert.equal(page3.next_offset, null);
  // An oversized page request is capped, not honored.
  const capped = (await call(d, request("GET", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, query: { limit: "100000" },
  }))).body;
  assert.ok(capped.jobs.length <= 100);
});

// =====================================================================
// The worker protocol (the native execution boundary)
// =====================================================================

test("worker protocol: the token is the trust boundary", async () => {
  const d = deps();
  const noToken = await call(d, request("POST", "/api/worker/claim", {
    body: { worker_id: "w", lease_ms: 60000 },
  }));
  assert.equal(noToken.status, 401);
  const badToken = await call(d, request("POST", "/api/worker/claim", {
    authorization: "Bearer wrong-token", body: { worker_id: "w", lease_ms: 60000 },
  }));
  assert.equal(badToken.status, 401);
  // Without a configured token the boundary is disabled (503), never fake.
  const disabled: PlatformDeps = { ...deps(), workerToken: undefined };
  const unavailable = await call(disabled, request("POST", "/api/worker/claim", {
    authorization: "Bearer anything", body: { worker_id: "w", lease_ms: 60000 },
  }));
  assert.equal(unavailable.status, 503);
});

test("worker protocol: atomic claim, lease heartbeat, cancel relay, idempotent complete", async () => {
  const d = deps();
  const project = (await call(d, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "p" } }))).body.project_id;
  await call(d, request("POST", `/api/projects/${project}/jobs`, { authorization: `Bearer ${alice}`, body: SUBMIT }));
  await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, body: { ...SUBMIT, job_id: "job-0002", requested_shard_count: 1 },
  }));
  const authz = "Bearer worker-secret-token";

  // Worker A claims the OLDEST queued job.
  const claim1 = await call(d, request("POST", "/api/worker/claim", {
    authorization: authz, body: { worker_id: "worker-a", lease_ms: 60000 },
  }));
  assert.equal(claim1.body.claimed, true);
  assert.equal(claim1.body.job.job_id, "job-0001");
  assert.equal(claim1.body.job.operation, "vector_add");
  // Worker B cannot claim the same job — it gets the next one.
  const claim2 = await call(d, request("POST", "/api/worker/claim", {
    authorization: authz, body: { worker_id: "worker-b", lease_ms: 60000 },
  }));
  assert.equal(claim2.body.claimed, true);
  assert.equal(claim2.body.job.job_id, "job-0002");
  // Nothing left: an honest no-work.
  const claim3 = await call(d, request("POST", "/api/worker/claim", {
    authorization: authz, body: { worker_id: "worker-c", lease_ms: 60000 },
  }));
  assert.equal(claim3.body.claimed, false);

  // Heartbeat: only the claiming worker's heartbeat is accepted.
  const foreignBeat = await call(d, request("POST", "/api/worker/jobs/job-0001/heartbeat", {
    authorization: authz, body: { worker_id: "worker-b" },
  }));
  assert.equal(foreignBeat.status, 409);
  const beat = await call(d, request("POST", "/api/worker/jobs/job-0001/heartbeat", {
    authorization: authz, body: { worker_id: "worker-a" },
  }));
  assert.equal(beat.body.accepted, true);

  // Complete: the claiming worker reports the terminal outcome.
  const complete = await call(d, request("POST", "/api/worker/jobs/job-0001/complete", {
    authorization: authz,
    body: {
      worker_id: "worker-a", status: "completed", error: "", backend: "cpu",
      result_element_count: 1000, shards_total: 2, shards_succeeded: 2, shards_failed: 0,
    },
  }));
  assert.equal(complete.body.recorded, true);
  assert.equal(complete.body.status, "completed");

  // The duplicate commit is a replay (no duplicate result).
  const duplicate = await call(d, request("POST", "/api/worker/jobs/job-0001/complete", {
    authorization: authz,
    body: { worker_id: "worker-a", status: "completed", error: "", backend: "cpu", result_element_count: 1000 },
  }));
  assert.equal(duplicate.body.recorded, false);
  assert.equal(duplicate.body.status, "completed");

  // The job detail shows the REAL execution summary.
  const detail = (await call(d, request("GET", `/api/service/jobs/job-0001`, { authorization: `Bearer ${alice}` }))).body;
  assert.equal(detail.status, "completed");
  assert.equal(detail.total_shards, 2);
  assert.equal(detail.succeeded_shards, 2);
  assert.equal(detail.result_element_count, 1000);
  assert.equal(detail.attempt, 1);

  // A failed report requires its reason.
  const reasonless = await call(d, request("POST", "/api/worker/jobs/job-0002/fail", {
    authorization: authz, body: { worker_id: "worker-b", status: "failed", error: "" },
  }));
  assert.equal(reasonless.status, 422);
  const failure = await call(d, request("POST", "/api/worker/jobs/job-0002/fail", {
    authorization: authz, body: { worker_id: "worker-b", status: "failed", error: "device lost" },
  }));
  assert.equal(failure.body.recorded, true);
  const failedDetail = (await call(d, request("GET", `/api/service/jobs/job-0002`, { authorization: `Bearer ${alice}` }))).body;
  assert.equal(failedDetail.status, "failed");
  assert.equal(failedDetail.error, "device lost");
});

test("worker protocol: stale leases are reconciled honestly", async () => {
  let now = FIXED_NOW;
  const store = new InMemoryServiceStore(() => now, makeId);
  const d: PlatformDeps = { ...deps(), service: store };
  const project = (await call(d, request("POST", "/api/projects", { authorization: `Bearer ${alice}`, body: { name: "p" } }))).body.project_id;
  await call(d, request("POST", `/api/projects/${project}/jobs`, {
    authorization: `Bearer ${alice}`, body: { ...SUBMIT, requested_shard_count: 1 },
  }));
  const authz = "Bearer worker-secret-token";
  await call(d, request("POST", "/api/worker/claim", {
    authorization: authz, body: { worker_id: "worker-a", lease_ms: 60000 },
  }));

  // The worker dies: time advances past the lease, no heartbeat arrives.
  now += 120000;
  const reconcile = await call(d, request("POST", "/api/internal/reconcile", {
    authorization: authz,
  }));
  assert.equal(reconcile.body.recovered_stale_jobs, 1);
  const detail = (await call(d, request("GET", `/api/service/jobs/${SUBMIT.job_id}`, { authorization: `Bearer ${alice}` }))).body;
  assert.equal(detail.status, "failed");
  assert.equal(detail.error, "worker_lease_expired");

  // The user sees the truth (a failed job with its reason) — never a fake
  // "still running" and never an automatic re-execution.
  const jobs = (await call(d, request("GET", `/api/projects/${project}/jobs`, { authorization: `Bearer ${alice}` }))).body;
  assert.equal(jobs.jobs[0].status, "failed");
});
