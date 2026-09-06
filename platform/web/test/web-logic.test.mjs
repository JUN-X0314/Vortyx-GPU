// Web console logic tests (Phase 15) — the console is a no-build static
// SPA, so its LOGIC modules are tested directly with node:test against
// minimal DOM/fetch/localStorage stubs: the session lifecycle (persist,
// restore, refresh, honest signed-out), the API client (Bearer attach,
// 401 -> refresh -> retry, session-lost handoff, the unified error body)
// and the error view mapping. The rendered DOM itself is verified in the
// browser (documented); these tests pin the behavior that would silently
// rot.

import { test, mock } from "node:test";
import assert from "node:assert/strict";

// ---------------------------------------------------------------------------
// Minimal browser-environment stubs (installed before importing the modules)
// ---------------------------------------------------------------------------

const storage = new Map();
globalThis.localStorage = {
  getItem: (key) => (storage.has(key) ? storage.get(key) : null),
  setItem: (key, value) => storage.set(key, String(value)),
  removeItem: (key) => storage.delete(key),
};

let fetchHandler = async () => ({ ok: true, status: 200, json: async () => ({}) });
globalThis.fetch = async (...args) => fetchHandler(...args);

const { loadConfig, signUp, signIn, restoreSession, refreshToken, readStoredSession, clearLocalSession, SessionExpiredError } =
  await import("../js/auth.js");
const { ApiClient, ApiError, describeApiError } = await import("../js/api.js");

const CONFIG = { supabaseUrl: "https://auth.example.com", supabaseAnonKey: "anon-key" };
const API_CONFIG = { baseUrl: "" };

function authResponse(overrides = {}) {
  return {
    access_token: "access-1",
    refresh_token: "refresh-1",
    expires_in: 3600,
    expires_at: Math.floor(Date.now() / 1000) + 3600,
    user: { id: "user-1", email: "a@b.c" },
    ...overrides,
  };
}

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

test("config: missing publishable values are an honest configuration error", () => {
  assert.throws(() => loadConfig(), /not configured/);
});

test("signUp persists the session and reports confirmation-required honestly", async () => {
  clearLocalSession();
  // The confirmation flow: no tokens yet, the user must confirm by email.
  fetchHandler = async () => ({ ok: true, status: 200, json: async () => ({ user: { id: "u" } }) });
  await assert.rejects(() => signUp(CONFIG, "a@b.c", "password123"), /confirmation required/);
  assert.equal(readStoredSession(), null);

  // The confirmed flow: tokens come back and are persisted.
  fetchHandler = async () => ({ ok: true, status: 200, json: async () => authResponse() });
  const session = await signUp(CONFIG, "a@b.c", "password123");
  assert.equal(session.user_id, "user-1");
  assert.equal(readStoredSession().access_token, "access-1");

  // A real auth failure surfaces with its message (never a fake session).
  fetchHandler = async () => ({ ok: false, status: 400, json: async () => ({ msg: "User already registered" }) });
  await assert.rejects(() => signUp(CONFIG, "a@b.c", "password123"), /already registered/);
});

test("restoreSession: valid tokens are reused, stale ones are refreshed", async () => {
  clearLocalSession();
  assert.equal(await restoreSession(CONFIG), null); // nothing stored

  // A fresh session comes straight from storage.
  fetchHandler = async () => ({ ok: true, status: 200, json: async () => authResponse() });
  await signIn(CONFIG, "a@b.c", "password123");
  const restored = await restoreSession(CONFIG);
  assert.equal(restored.user_id, "user-1");

  // An expired session forces a refresh (rotation: the NEW refresh token
  // is what gets stored).
  const stored = readStoredSession();
  stored.expires_at = Math.floor(Date.now() / 1000) - 10;
  storage.set("vortyx.session", JSON.stringify(stored));
  fetchHandler = async () => ({
    ok: true,
    status: 200,
    json: async () => authResponse({ access_token: "access-2", refresh_token: "refresh-2" }),
  });
  const refreshed = await restoreSession(CONFIG);
  assert.equal(refreshed.access_token, "access-2");
  assert.equal(readStoredSession().refresh_token, "refresh-2");
});

test("restoreSession: an unusable refresh token is an honest signed-out state", async () => {
  const stored = readStoredSession();
  stored.expires_at = Math.floor(Date.now() / 1000) - 10;
  storage.set("vortyx.session", JSON.stringify(stored));
  fetchHandler = async () => ({ ok: false, status: 400, json: async () => ({ error: "invalid_grant" }) });
  assert.equal(await restoreSession(CONFIG), null);
  assert.equal(readStoredSession(), null); // cleared, not kept half-alive
});

// ---------------------------------------------------------------------------
// The API client
// ---------------------------------------------------------------------------

function makeClient(sessionGetter) {
  let current = sessionGetter;
  const client = new ApiClient(
    API_CONFIG,
    CONFIG,
    async () => current(),
    (refreshed) => {
      current = () => refreshed;
    },
    () => {
      current = () => null;
    },
  );
  return client;
}

test("api client: the Bearer credential is the live access token", async () => {
  let captured = null;
  fetchHandler = async (_url, init) => {
    captured = init.headers;
    return { ok: true, status: 200, json: async () => ({ ok: true }) };
  };
  const client = makeClient(() => authResponse());
  await client.get("/api/projects");
  assert.equal(captured.Authorization, "Bearer access-1");
});

test("api client: a 401 refreshes once and retries; a second 401 ends the session", async () => {
  const sessionState = { access_token: "access-1", refresh_token: "refresh-1" };
  const client = makeClient(() => sessionState);
  let calls = 0;
  let authCalls = 0;
  fetchHandler = async (url, init) => {
    if (String(url).includes("/auth/v1/token")) {
      authCalls += 1;
      return { ok: true, status: 200, json: async () => authResponse({ access_token: "access-2", refresh_token: "refresh-2" }) };
    }
    calls += 1;
    if (init.headers.Authorization === "Bearer access-1") {
      return { ok: false, status: 401, json: async () => ({}) };
    }
    return { ok: true, status: 200, json: async () => ({ ok: true }) };
  };
  await client.get("/api/projects");
  assert.equal(calls, 2); // original + retry
  assert.equal(authCalls, 1);
  // The refresh is rotation-safe: the client's live session (and storage)
  // hold the NEW tokens; the old object is never mutated in place.
  assert.equal(readStoredSession().access_token, "access-2");

  // The retry fails too: the session-lost handoff fires, and the next call
  // throws SessionExpiredError (never a fake success).
  let lost = 0;
  const losing = new ApiClient(
    API_CONFIG,
    CONFIG,
    async () => ({ access_token: "stale", refresh_token: "refresh-1", user_id: "u", email: "" }),
    () => {},
    () => {
      lost += 1;
    },
  );
  fetchHandler = async (url) => {
    if (String(url).includes("/auth/v1/token")) {
      return { ok: false, status: 400, json: async () => ({ error: "invalid_grant" }) };
    }
    return { ok: false, status: 401, json: async () => ({}) };
  };
  await assert.rejects(() => losing.get("/api/projects"), SessionExpiredError);
  assert.equal(lost, 1);
});

test("api client: the unified error body becomes a typed ApiError", async () => {
  const client = makeClient(() => authResponse());
  fetchHandler = async () => ({
    ok: false,
    status: 429,
    json: async () => ({ error: { code: "quota_exceeded", message: "max_running_shards would be exceeded" } }),
  });
  const error = await client.post("/api/projects/p/jobs", {}).catch((e) => e);
  assert.ok(error instanceof ApiError);
  assert.equal(error.status, 429);
  assert.equal(error.code, "quota_exceeded");
});

// ---------------------------------------------------------------------------
// The error view mapping (one vocabulary across the console)
// ---------------------------------------------------------------------------

test("error mapping: every refusal kind has its user-facing wording", () => {
  const forbidden = describeApiError(new ApiError(403, "forbidden", "no"));
  assert.equal(forbidden.kind, "forbidden");
  assert.ok(forbidden.title.length > 0);

  const notFound = describeApiError(new ApiError(404, "not_found", "no"));
  assert.equal(notFound.kind, "not_found");

  const quota = describeApiError(new ApiError(429, "quota_exceeded", "max_memory_bytes would be exceeded"));
  assert.equal(quota.kind, "quota");
  assert.ok(quota.detail.includes("max_memory_bytes"));

  const network = describeApiError(new TypeError("fetch failed"));
  assert.equal(network.kind, "network");

  const expired = describeApiError(new SessionExpiredError());
  assert.equal(expired.kind, "unauthorized");
});

// ---------------------------------------------------------------------------
// Phase 16: the job detail's execution-plan rows (pure logic — the console
// shows ONLY what the API sent, and renders an explicit not-available state
// otherwise)
// ---------------------------------------------------------------------------

const { planSummaryRows } = await import("../js/views/job-view.js");

test("plan rows: a present plan renders its recorded fields verbatim", () => {
  const rows = planSummaryRows({
    plan_version: 2,
    planner: "adaptive_fabric",
    planner_version: "0.16.0",
    cluster_revision: 9,
    devices: ["dev-a", "dev-b"],
    reason: "accepted: planned 1 workload; w1 -> dev-a",
  });
  assert.ok(Array.isArray(rows));
  assert.deepEqual(
    rows.map(([label]) => label),
    ["Plan version", "Planner", "Planner version", "Devices", "Reason"],
  );
  const byLabel = Object.fromEntries(rows);
  assert.equal(byLabel["Plan version"], "2");
  assert.equal(byLabel.Planner, "adaptive_fabric");
  assert.equal(byLabel.Planner, "adaptive_fabric");
  assert.equal(byLabel.Devices, "dev-a, dev-b");
  assert.ok(byLabel.Reason.includes("planned 1 workload"));
});

test("plan rows: a null/absent plan yields no rows (the caller shows not available)", () => {
  assert.equal(planSummaryRows(null), null);
  assert.equal(planSummaryRows(undefined), null);
  assert.equal(planSummaryRows("garbage"), null);
});

test("plan rows: missing fields fall back to placeholders, never invented values", () => {
  const rows = planSummaryRows({ plan_version: 1 });
  const byLabel = Object.fromEntries(rows);
  assert.equal(byLabel["Plan version"], "1");
  assert.equal(byLabel.Planner, "—");
  assert.equal(byLabel.Devices, "none recorded");
  assert.equal(byLabel.Reason, "—");
});
