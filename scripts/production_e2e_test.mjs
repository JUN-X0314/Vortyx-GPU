#!/usr/bin/env node
// Phase 17 — production end-to-end verification: REAL Supabase Auth, REAL
// CRUD through the deployed API, the REAL worker protocol, REAL
// reconciliation, and the RLS boundary probed directly at the database.
//
// Identity strategy (no production data is touched or left behind):
//   * two throwaway auth users are created per run via the service-role
//     Auth admin API (email namespace phase17-e2e-*@e2e.vortyx.test,
//     metadata {"e2e":"phase17","run":<runId>}),
//   * every project/job created during the run hangs off those users,
//   * cleanup DELETES both users (auth.users cascade removes profiles,
//     projects, members, service_jobs, artifacts, audit actor links) and
//     VERIFIES the deletion — cleanup failure fails the run.
//
// Secrets are consumed from the environment and NEVER printed:
//   VORTYX_PROD_URL               the deployed API origin
//   VORTYX_E2E_SUPABASE_URL       the Supabase project URL
//   VORTYX_E2E_ANON_KEY           the publishable key
//   VORTYX_E2E_SERVICE_ROLE_KEY   server-only (admin API + RLS probes)
//   VORTYX_E2E_WORKER_TOKEN       the deployed worker boundary secret
//   VORTYX_E2E_RECONCILE_TOKEN    the deployed reconcile secret
//
// Exit codes: 0 = every check (including cleanup) passed; 1 = otherwise.
const base = (process.env.VORTYX_PROD_URL ?? "").replace(/\/+$/, "");
const sbUrl = (process.env.VORTYX_E2E_SUPABASE_URL ?? "").replace(/\/+$/, "");
const anonKey = process.env.VORTYX_E2E_ANON_KEY ?? "";
const serviceKey = process.env.VORTYX_E2E_SERVICE_ROLE_KEY ?? "";
const workerToken = process.env.VORTYX_E2E_WORKER_TOKEN ?? "";
const reconcileToken = process.env.VORTYX_E2E_RECONCILE_TOKEN ?? "";

for (const [name, value] of Object.entries({
  VORTYX_PROD_URL: base, VORTYX_E2E_SUPABASE_URL: sbUrl, VORTYX_E2E_ANON_KEY: anonKey,
  VORTYX_E2E_SERVICE_ROLE_KEY: serviceKey, VORTYX_E2E_WORKER_TOKEN: workerToken,
  VORTYX_E2E_RECONCILE_TOKEN: reconcileToken,
})) {
  if (value.length === 0) {
    console.error(`usage: ${name} (and the rest) must be set`);
    process.exit(1);
  }
}

const runId = `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 8)}`;
let failures = 0;
function check(name, ok, detail = "") {
  console.log(`${ok ? "PASS" : "FAIL"}: ${name}${detail ? ` — ${detail}` : ""}`);
  if (!ok) failures += 1;
}
function notOk(status) {
  return status < 200 || status >= 300;
}

const createdUserIds = [];
const createdCredentials = [];
async function cleanup() {
  let cleanupFailures = 0;
  for (const userId of createdUserIds) {
    const response = await fetch(`${sbUrl}/auth/v1/admin/users/${encodeURIComponent(userId)}`, {
      method: "DELETE",
      headers: { apikey: serviceKey, Authorization: `Bearer ${serviceKey}` },
    });
    if (notOk(response.status)) {
      console.log(`FAIL: cleanup — deleting user ${userId} returned ${response.status}`);
      cleanupFailures += 1;
    }
  }
  // VERIFY the cleanup: a deleted identity must not authenticate anymore.
  for (const { email, password } of createdCredentials) {
    const login = await fetch(`${sbUrl}/auth/v1/token?grant_type=password`, {
      method: "POST",
      headers: { apikey: anonKey, "Content-Type": "application/json" },
      body: JSON.stringify({ email, password }),
    });
    if (login.status === 200) {
      console.log(`FAIL: cleanup — user ${email.split("@")[0].slice(0, 24)}… still authenticates after deletion`);
      cleanupFailures += 1;
    }
  }
  createdUserIds.length = 0;
  createdCredentials.length = 0;
  return cleanupFailures;
}

async function api(method, path, token, body) {
  const response = await fetch(`${base}${path}`, {
    method,
    headers: {
      ...(token !== null && token !== undefined ? { Authorization: `Bearer ${token}` } : {}),
      ...(body !== undefined ? { "Content-Type": "application/json" } : {}),
    },
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  let json = null;
  try {
    json = await response.json();
  } catch {
    json = null;
  }
  return { status: response.status, body: json };
}

async function authAdmin(method, path, payload) {
  const response = await fetch(`${sbUrl}${path}`, {
    method,
    headers: { apikey: serviceKey, Authorization: `Bearer ${serviceKey}`, "Content-Type": "application/json" },
    body: payload === undefined ? undefined : JSON.stringify(payload),
  });
  let json = null;
  try {
    json = await response.json();
  } catch {
    json = null;
  }
  return { status: response.status, body: json };
}

async function signUp(tag) {
  const email = `phase17-e2e-${tag}-${runId}@e2e.vortyx.test`;
  const password = `Px${runId}${Math.random().toString(36).slice(2, 10)}!aA1`;
  const created = await authAdmin("POST", "/auth/v1/admin/users", {
    email,
    password,
    email_confirm: true,
    user_metadata: { e2e: "phase17", run: runId },
  });
  const userId = created.body?.id;
  if (notOk(created.status) || userId === undefined) {
    throw new Error(`admin user create failed (${created.status})`);
  }
  createdUserIds.push(userId);
  createdCredentials.push({ email, password });
  const login = await fetch(`${sbUrl}/auth/v1/token?grant_type=password`, {
    method: "POST",
    headers: { apikey: anonKey, "Content-Type": "application/json" },
    body: JSON.stringify({ email, password }),
  });
  const session = await login.json();
  if (login.status !== 200 || typeof session.access_token !== "string") {
    throw new Error(`password login failed (${login.status})`);
  }
  return { userId, email, password, token: session.access_token };
}

try {
  // ---------------------------------------------------------------- setup
  const alice = await signUp("a");
  const bob = await signUp("b");
  check("two isolated E2E identities created and logged in", true, `run ${runId}`);

  // ------------------------------------------- 1. anonymous / bad tokens
  {
    const anonymous = await api("GET", "/api/projects", null);
    check("anonymous /api/projects -> 401", anonymous.status === 401, `got ${anonymous.status}`);
    const garbage = await api("GET", "/api/projects", "not-a-real-token");
    check("garbage token /api/projects -> 401", garbage.status === 401, `got ${garbage.status}`);
  }

  // ---------------------------------------------- 2. the verified subject
  {
    const me = await api("GET", "/api/me", alice.token);
    check(
      "GET /api/me returns the VERIFIED subject (not a client claim)",
      me.status === 200 && me.body?.user_id === alice.userId,
      `status ${me.status}`,
    );
  }

  // --------------------------------------- 3. project CRUD + role contract
  let projectId = null;
  {
    const created = await api("POST", "/api/projects", alice.token, { name: `phase17-e2e-${runId}` });
    check("POST /api/projects creates the project", created.status === 200 && typeof created.body?.project_id === "string", `status ${created.status}`);
    projectId = created.body?.project_id ?? null;
    if (projectId === null) throw new Error("no project id — cannot continue the CRUD flow");

    const listed = await api("GET", "/api/projects", alice.token);
    const entry = (listed.body?.projects ?? []).find((p) => p.project_id === projectId);
    check(
      "GET /api/projects lists the project with the caller's role",
      listed.status === 200 && entry?.role === "owner",
      `role ${JSON.stringify(entry?.role)}`,
    );

    const detail = await api("GET", `/api/projects/${projectId}`, alice.token);
    check("GET /api/projects/:id -> 200 with the same id", detail.status === 200 && detail.body?.project_id === projectId);
  }

  // ------------------------------- 4. cross-user isolation (anti-enumeration)
  {
    const foreignDetail = await api("GET", `/api/projects/${projectId}`, bob.token);
    check("foreign project detail -> 404 (anti-enumeration, never 403)", foreignDetail.status === 404, `got ${foreignDetail.status}`);
    const foreignSubmit = await api("POST", `/api/projects/${projectId}/jobs`, bob.token, {
      job_id: `e2e-${runId}-foreign`, operation: "vector_add", element_count: 8,
      requested_backend: "", requested_shard_count: 1,
    });
    check("foreign project job submission -> 404", foreignSubmit.status === 404, `got ${foreignSubmit.status}`);
    const bobList = await api("GET", "/api/projects", bob.token);
    check(
      "B's project list never contains A's project",
      bobList.status === 200 && !(bobList.body?.projects ?? []).some((p) => p.project_id === projectId),
    );
  }

  // ------------------------------------------------------ 5. quota policy
  {
    const quota = await api("PUT", `/api/projects/${projectId}/quota`, alice.token, {
      max_concurrent_jobs: 1, max_running_shards: 8, max_memory_bytes: 1073741824,
    });
    check("PUT quota (owner) -> 200", quota.status === 200, `status ${quota.status}`);
  }

  // ------------------------------------ 6. submission, idempotency, quota
  const j1 = `e2e-${runId}-j1`;
  {
    const submit = await api("POST", `/api/projects/${projectId}/jobs`, alice.token, {
      job_id: j1, operation: "vector_add", element_count: 8, requested_backend: "", requested_shard_count: 1,
    });
    check("job submission -> queued", submit.status === 200 && submit.body?.status === "queued" && submit.body?.created === true, `status ${submit.status}`);

    const replay = await api("POST", `/api/projects/${projectId}/jobs`, alice.token, {
      job_id: j1, operation: "vector_add", element_count: 8, requested_backend: "", requested_shard_count: 1,
    });
    check("same-payload resubmission is an idempotent replay (created=false)", replay.status === 200 && replay.body?.created === false, `status ${replay.status}`);

    const conflict = await api("POST", `/api/projects/${projectId}/jobs`, alice.token, {
      job_id: j1, operation: "vector_add", element_count: 16, requested_backend: "", requested_shard_count: 1,
    });
    check("same id + different payload -> 409 conflict", conflict.status === 409, `got ${conflict.status}`);

    const [r2, r3] = await Promise.all([
      api("POST", `/api/projects/${projectId}/jobs`, alice.token, {
        job_id: `e2e-${runId}-j2`, operation: "vector_add", element_count: 8, requested_backend: "", requested_shard_count: 1,
      }),
      api("POST", `/api/projects/${projectId}/jobs`, alice.token, {
        job_id: `e2e-${runId}-j3`, operation: "vector_add", element_count: 8, requested_backend: "", requested_shard_count: 1,
      }),
    ]);
    check(
      "concurrent submissions beyond max_concurrent_jobs=1 are both refused (429, quota enforced server-side)",
      r2.status === 429 && r3.status === 429,
      `got ${r2.status}, ${r3.status}`,
    );

    const bumped = await api("PUT", `/api/projects/${projectId}/quota`, alice.token, {
      max_concurrent_jobs: 4, max_running_shards: 16, max_memory_bytes: 1073741824,
    });
    check("quota restored for the worker flow", bumped.status === 200);
  }

  // ------------------------------------------------ 7. the worker boundary
  const workerId = `e2e-worker-${runId}`;
  const workerId2 = `e2e-worker2-${runId}`;
  {
    const noToken = await api("POST", "/api/worker/claim", null, { worker_id: workerId, lease_ms: 60000 });
    check("worker claim without token -> 401 (boundary enforced)", noToken.status === 401, `got ${noToken.status}`);
    const badToken = await api("POST", "/api/worker/claim", "wrong-worker-secret", { worker_id: workerId, lease_ms: 60000 });
    check("worker claim with a wrong secret -> 401", badToken.status === 401, `got ${badToken.status}`);
    const userToken = await api("POST", "/api/worker/claim", alice.token, { worker_id: workerId, lease_ms: 60000 });
    check(
      "worker claim with a USER identity token -> 401 (the worker boundary is not a user surface)",
      userToken.status === 401,
      `got ${userToken.status}`,
    );

    const claim = await api("POST", "/api/worker/claim", workerToken, { worker_id: workerId, lease_ms: 60000 });
    check(
      "valid worker claim -> the queued job J1",
      claim.status === 200 && claim.body?.claimed === true && claim.body?.job?.job_id === j1,
      `status ${claim.status}; job ${JSON.stringify(claim.body?.job?.job_id)}`,
    );

    const heartbeat = await api("POST", `/api/worker/jobs/${j1}/heartbeat`, workerToken, { worker_id: workerId, lease_ms: 60000 });
    check("heartbeat from the claim holder -> accepted", heartbeat.status === 200 && heartbeat.body?.accepted === true, `status ${heartbeat.status}`);
    const foreignHeartbeat = await api("POST", `/api/worker/jobs/${j1}/heartbeat`, workerToken, { worker_id: workerId2, lease_ms: 60000 });
    check("heartbeat from a non-holder -> 409", foreignHeartbeat.status === 409, `got ${foreignHeartbeat.status}`);

    const emptyFail = await api("POST", `/api/worker/jobs/${j1}/fail`, workerToken, {
      worker_id: workerId, status: "failed", error: "", backend: "cpu", result_element_count: null,
    });
    check("a failed report without its reason -> 422", emptyFail.status === 422, `got ${emptyFail.status}`);

    const foreignComplete = await api("POST", `/api/worker/jobs/${j1}/complete`, workerToken, {
      worker_id: workerId2, status: "completed", error: "", backend: "cpu", result_element_count: 8,
    });
    check("completion from a worker that does not hold the claim -> 409", foreignComplete.status === 409, `got ${foreignComplete.status}`);

    const complete = await api("POST", `/api/worker/jobs/${j1}/complete`, workerToken, {
      worker_id: workerId, status: "completed", error: "", backend: "cpu", result_element_count: 8,
    });
    check("valid completion -> recorded", complete.status === 200 && complete.body?.recorded === true && complete.body?.status === "completed", `status ${complete.status}`);

    const duplicate = await api("POST", `/api/worker/jobs/${j1}/complete`, workerToken, {
      worker_id: workerId, status: "completed", error: "", backend: "cpu", result_element_count: 8,
    });
    check("duplicate completion is an idempotent replay (recorded=false)", duplicate.status === 200 && duplicate.body?.recorded === false && duplicate.body?.status === "completed", `status ${duplicate.status}`);

    const detail = await api("GET", `/api/service/jobs/${j1}`, alice.token);
    check(
      "the job record carries the honest execution summary",
      detail.status === 200 && detail.body?.status === "completed" && detail.body?.result_element_count === 8,
      `status ${JSON.stringify(detail.body?.status)}`,
    );
  }

  // ------------------------------------- 8. cancellation vs. the claim race
  const j4 = `e2e-${runId}-j4`;
  {
    const submit = await api("POST", `/api/projects/${projectId}/jobs`, alice.token, {
      job_id: j4, operation: "vector_scale", element_count: 4, requested_backend: "", requested_shard_count: 1,
    });
    check("J4 queued for the cancellation case", submit.status === 200);
    const cancel = await api("POST", `/api/service/jobs/${j4}/cancel`, alice.token);
    check("cancel a queued job -> cancelled", cancel.status === 200 && cancel.body?.status === "cancelled", `status ${cancel.status}`);

    const claim = await api("POST", "/api/worker/claim", workerToken, { worker_id: workerId, lease_ms: 60000 });
    const claimedId = claim.body?.job?.job_id ?? null;
    check(
      "a cancelled job is NEVER claimed afterwards",
      claim.status === 200 && claimedId !== j4,
      `claimed ${JSON.stringify(claimedId)}`,
    );

    const replay = await api("POST", `/api/worker/jobs/${j4}/complete`, workerToken, {
      worker_id: workerId, status: "completed", error: "", backend: "cpu", result_element_count: 4,
    });
    check(
      "completing a cancelled job replays its existing terminal state (never resurrects)",
      replay.status === 200 && replay.body?.recorded === false && replay.body?.status === "cancelled",
      `status ${replay.status}; body ${JSON.stringify(replay.body)}`,
    );
    if (claimedId !== null && claimedId !== j4) {
      // Park the claimed job so the usage ledger stays clean for the next checks.
      await api("POST", `/api/worker/jobs/${claimedId}/complete`, workerToken, {
        worker_id: workerId, status: "completed", error: "", backend: "cpu", result_element_count: null,
      });
    }
  }

  // ------------------------------------------- 9. stale lease + reconcile
  const j5 = `e2e-${runId}-j5`;
  {
    const submit = await api("POST", `/api/projects/${projectId}/jobs`, alice.token, {
      job_id: j5, operation: "vector_scale", element_count: 4, requested_backend: "", requested_shard_count: 1,
    });
    const claim = await api("POST", "/api/worker/claim", workerToken, { worker_id: workerId, lease_ms: 1000 });
    const claimedId = claim.body?.job?.job_id;
    check("J5 claimed with a 1s lease", submit.status === 200 && claimedId === j5, `claimed ${JSON.stringify(claimedId)}`);

    const userReconcile = await api("POST", "/api/internal/reconcile", alice.token);
    check("reconcile with a USER token -> 401 (privileged boundary)", userReconcile.status === 401, `got ${userReconcile.status}`);
    const badReconcile = await api("POST", "/api/internal/reconcile", "wrong-reconcile-secret");
    check("reconcile with a wrong secret -> 401", badReconcile.status === 401, `got ${badReconcile.status}`);

    await new Promise((resolve) => setTimeout(resolve, 1500));
    const reconcile = await api("POST", "/api/internal/reconcile", reconcileToken);
    check("reconcile with the configured secret -> 200 and recovers the stale lease", reconcile.status === 200 && reconcile.body?.recovered_stale_jobs >= 1, `status ${reconcile.status}; body ${JSON.stringify(reconcile.body)}`);

    const stale = await api("GET", `/api/service/jobs/${j5}`, alice.token);
    check(
      "the stale job is honestly failed with its reason",
      stale.status === 200 && stale.body?.status === "failed" && stale.body?.error === "worker_lease_expired",
      `status ${stale.body?.status}; error ${JSON.stringify(stale.body?.error)}`,
    );
  }

  // -------------------------------------------- 10. usage release + audit
  {
    const usage = await api("GET", `/api/projects/${projectId}/usage`, alice.token);
    check(
      "usage returns to zero after terminal outcomes (released exactly once)",
      usage.status === 200 && usage.body?.active_jobs === 0,
      `body ${JSON.stringify(usage.body)}`,
    );
    const audit = await api("GET", "/api/audit", alice.token);
    check(
      "the audit trail records the run's actions",
      audit.status === 200 && (audit.body?.events ?? []).some((e) => e.action === "job_submit" && e.outcome === "ok"),
      `events ${JSON.stringify(audit.body?.events?.length)}`,
    );
    const metrics = await api("GET", "/api/metrics", alice.token);
    check("metrics are visible to the authenticated caller", metrics.status === 200 && (metrics.body?.total_jobs ?? 0) >= 3, `body ${JSON.stringify(metrics.body)}`);
  }

  // ----------------------- 11. the RLS boundary, probed DIRECTLY (no API)
  {
    const listResponse = await fetch(`${sbUrl}/rest/v1/projects?select=id`, {
      headers: { apikey: anonKey, Authorization: `Bearer ${bob.token}` },
    });
    const listBody = await listResponse.json();
    check(
      "direct REST as B: project visibility is membership-scoped (RLS)",
      listResponse.status === 200 && Array.isArray(listBody) && !listBody.some((row) => row.id === projectId),
      `status ${listResponse.status}`,
    );

    const insertResponse = await fetch(`${sbUrl}/rest/v1/service_jobs`, {
      method: "POST",
      headers: { apikey: anonKey, Authorization: `Bearer ${bob.token}`, "Content-Type": "application/json", Prefer: "return=representation" },
      body: JSON.stringify({
        job_id: `e2e-${runId}-rls`, project_id: projectId, submitted_by: bob.userId,
        operation: "vector_add", element_count: 8, requested_backend: "", requested_shard_count: 1, submitted_at_ms: Date.now(),
      }),
    });
    check(
      "direct REST as B: inserting into A's project is refused by RLS",
      notOk(insertResponse.status),
      `status ${insertResponse.status}`,
    );

    const workerRpc = await fetch(`${sbUrl}/rest/v1/rpc/vortyx_worker_claim`, {
      method: "POST",
      headers: { apikey: anonKey, Authorization: `Bearer ${alice.token}`, "Content-Type": "application/json" },
      body: JSON.stringify({ p_worker_id: "e2e-rls-probe", p_lease_ms: 60000 }),
    });
    check(
      "direct RPC as a USER: the worker-protocol function is locked to service_role (0005)",
      notOk(workerRpc.status),
      `status ${workerRpc.status}`,
    );

    const anonResponse = await fetch(`${sbUrl}/rest/v1/projects?select=id`, {
      headers: { apikey: anonKey },
    });
    const anonBody = await anonResponse.json();
    check(
      "direct REST anonymous: no project rows leak",
      anonResponse.status === 200 && Array.isArray(anonBody) && anonBody.length === 0,
      `status ${anonResponse.status}; rows ${JSON.stringify(anonBody?.length)}`,
    );
  }

  // ---------------------------------------------- 12. B's own project works
  {
    const created = await api("POST", "/api/projects", bob.token, { name: `phase17-e2e-b-${runId}` });
    check("B can create and use their OWN project", created.status === 200 && typeof created.body?.project_id === "string");
    const bobSubmit = await api("POST", `/api/projects/${created.body?.project_id}/jobs`, bob.token, {
      job_id: `e2e-${runId}-b1`, operation: "vector_scale", element_count: 4, requested_backend: "", requested_shard_count: 1,
    });
    check("B's own submission -> queued", bobSubmit.status === 200 && bobSubmit.body?.status === "queued", `status ${bobSubmit.status}`);
  }
} catch (error) {
  check("E2E flow completed without an unexpected error", false, error instanceof Error ? error.message : String(error));
} finally {
  // ------------------------------------------------------- cleanup (§55)
  const cleanupFailures = await cleanup();
  if (cleanupFailures === 0) {
    check("cleanup deleted every E2E identity (verified by re-authentication)", true);
  }
}

console.log(failures === 0 ? "[PASS] production E2E" : "[FAIL] production E2E");
process.exit(failures === 0 ? 0 : 1);
