// Router tests (Phase 11) — end-to-end API contract checks over the real
// pipeline (route resolution -> AuthN -> request validation -> store ->
// response). Local/mock mode only: the InMemoryPlatformStore plus the
// documented local token scheme ("Bearer local:<user_id>"). No network, no
// Supabase, no secrets.

import { test } from "node:test";
import assert from "node:assert/strict";

import { InMemoryPlatformStore } from "../src/memory-store.ts";
import { makeAuthenticated } from "../src/auth.ts";
import { handlePlatformRequest, type PlatformDeps, type PlatformRequest } from "../src/router.ts";
import { PROTOCOL_VERSION } from "../src/types.ts";

const aliceToken = "local:user-alice";
const bobToken = "local:user-bob";
const alice = makeAuthenticated("user-alice");

function deps(): PlatformDeps {
  return {
    store: new InMemoryPlatformStore(),
    verifier: async (token) => {
      if (!token.startsWith("local:")) return null;
      const userId = token.slice("local:".length);
      return userId.length > 0 ? makeAuthenticated(userId) : null;
    },
    storeKind: "memory",
    softwareVersion: "0.11.0",
  };
}

function request(
  method: string,
  path: string,
  options: { body?: unknown; authorization?: string } = {},
): PlatformRequest {
  return {
    method,
    path,
    body: options.body,
    authorization: options.authorization,
  };
}

const REGISTER_BODY = {
  device_id: "dev-1",
  protocol_version: PROTOCOL_VERSION,
  software_version: "0.11.0",
  operating_system: "linux",
  architecture: "x86_64",
  display_name: "workstation",
  backends: ["cpu"],
  operations: ["vector_add"],
};

test("public endpoints need no authentication", async () => {
  const d = deps();
  const health = await handlePlatformRequest(request("GET", "/api/health"), d);
  assert.equal(health.status, 200);
  const body = health.body as Record<string, unknown>;
  assert.equal(body["status"], "ok");
  assert.equal(body["protocol_version"], PROTOCOL_VERSION);
  assert.equal(body["store"], "memory");
  assert.equal(body["config_error"], null);

  const info = await handlePlatformRequest(request("GET", "/api/platform/info"), d);
  assert.equal(info.status, 200);
  const infoBody = info.body as Record<string, unknown>;
  assert.equal((infoBody["operations"] as string[]).length, 3);
  assert.equal((infoBody["backends"] as string[]).length, 2);
});

test("unknown routes and wrong methods are honest", async () => {
  const d = deps();
  const unknown = await handlePlatformRequest(request("GET", "/api/unknown"), d);
  assert.equal(unknown.status, 404);
  const notAllowed = await handlePlatformRequest(request("DELETE", "/api/health"), d);
  assert.equal(notAllowed.status, 405);
  const notAllowed2 = await handlePlatformRequest(request("PUT", "/api/jobs"), {
    ...d,
  });
  assert.equal(notAllowed2.status, 405);
});

test("authentication is required on data routes", async () => {
  const d = deps();
  const noToken = await handlePlatformRequest(request("GET", "/api/devices"), d);
  assert.equal(noToken.status, 401);
  assert.deepEqual(noToken.body, {
    error: { code: "unauthenticated", message: "authentication required" },
  });
  const badToken = await handlePlatformRequest(
    request("GET", "/api/devices", { authorization: "Bearer nope" }),
    d,
  );
  assert.equal(badToken.status, 401);
  const garbage = await handlePlatformRequest(
    request("GET", "/api/devices", { authorization: "Basic abc" }),
    d,
  );
  assert.equal(garbage.status, 401);
});

test("device registration flows end to end (with idempotent store rules)", async () => {
  const d = deps();
  const missingAuth = await handlePlatformRequest(
    request("POST", "/api/devices/register", { body: REGISTER_BODY }),
    d,
  );
  assert.equal(missingAuth.status, 401);

  const ok = await handlePlatformRequest(
    request("POST", "/api/devices/register", { body: REGISTER_BODY, authorization: `Bearer ${aliceToken}` }),
    d,
  );
  assert.equal(ok.status, 200);
  const device = ok.body as Record<string, unknown>;
  assert.equal(device["device_id"], "dev-1");
  assert.equal(device["owner_user_id"], "user-alice");
  assert.equal(device["status"], "online");

  const duplicate = await handlePlatformRequest(
    request("POST", "/api/devices/register", { body: REGISTER_BODY, authorization: `Bearer ${bobToken}` }),
    d,
  );
  assert.equal(duplicate.status, 409);

  const list = await handlePlatformRequest(
    request("GET", "/api/devices", { authorization: `Bearer ${aliceToken}` }),
    d,
  );
  assert.equal(list.status, 200);
  assert.equal((list.body as { devices: unknown[] }).devices.length, 1);

  const foreignList = await handlePlatformRequest(
    request("GET", "/api/devices", { authorization: `Bearer ${bobToken}` }),
    d,
  );
  assert.equal((foreignList.body as { devices: unknown[] }).devices.length, 0);
});

test("request validation maps through the unified error schema", async () => {
  const d = deps();
  const malformed = await handlePlatformRequest(
    request("POST", "/api/devices/register", {
      body: "{invalid json",
      authorization: `Bearer ${aliceToken}`,
    }),
    d,
  );
  assert.equal(malformed.status, 400);
  assert.equal((malformed.body as { error: { code: string } }).error.code, "invalid_json");

  const badEnum = await handlePlatformRequest(
    request("POST", "/api/jobs", {
      body: {
        job_id: "job-1",
        operation: "matrix_multiply",
        element_count: 4,
        protocol_version: PROTOCOL_VERSION,
      },
      authorization: `Bearer ${aliceToken}`,
    }),
    d,
  );
  assert.equal(badEnum.status, 422);
  assert.equal((badEnum.body as { error: { code: string } }).error.code, "invalid_enum");

  const badId = await handlePlatformRequest(
    request("POST", "/api/jobs", {
      body: { job_id: "bad id", operation: "vector_add", element_count: 4, protocol_version: PROTOCOL_VERSION },
      authorization: `Bearer ${aliceToken}`,
    }),
    d,
  );
  assert.equal(badId.status, 422);
  assert.equal((badId.body as { error: { code: string } }).error.code, "invalid_id");

  const unsupported = await handlePlatformRequest(
    request("POST", "/api/devices/register", {
      body: { ...REGISTER_BODY, protocol_version: "9" },
      authorization: `Bearer ${aliceToken}`,
    }),
    d,
  );
  assert.equal(unsupported.status, 422);
  assert.equal(
    (unsupported.body as { error: { code: string } }).error.code,
    "unsupported_protocol_version",
  );

  const unknownField = await handlePlatformRequest(
    request("POST", "/api/jobs", {
      body: { job_id: "j", operation: "vector_add", element_count: 4, protocol_version: "1", ghost: 1 },
      authorization: `Bearer ${aliceToken}`,
    }),
    d,
  );
  assert.equal(unknownField.status, 422);
  assert.equal((unknownField.body as { error: { code: string } }).error.code, "invalid_value");
});

test("job lifecycle flows through the API surface", async () => {
  const d = deps();
  await handlePlatformRequest(
    request("POST", "/api/devices/register", { body: REGISTER_BODY, authorization: `Bearer ${aliceToken}` }),
    d,
  );
  const submit = await handlePlatformRequest(
    request("POST", "/api/jobs", {
      body: {
        job_id: "job-1",
        operation: "vector_add",
        element_count: 1024,
        requested_backend: "cpu",
        protocol_version: PROTOCOL_VERSION,
        submitted_by_device_id: "dev-1",
      },
      authorization: `Bearer ${aliceToken}`,
    }),
    d,
  );
  assert.equal(submit.status, 200);
  assert.equal((submit.body as Record<string, unknown>)["created"], true);
  assert.equal((submit.body as Record<string, unknown>)["status"], "queued");

  const replay = await handlePlatformRequest(
    request("POST", "/api/jobs", {
      body: {
        job_id: "job-1",
        operation: "vector_add",
        element_count: 1024,
        requested_backend: "cpu",
        protocol_version: PROTOCOL_VERSION,
        submitted_by_device_id: "dev-1",
      },
      authorization: `Bearer ${aliceToken}`,
    }),
    d,
  );
  assert.equal(replay.status, 200);
  assert.equal((replay.body as Record<string, unknown>)["created"], false);

  const detail = await handlePlatformRequest(
    request("GET", "/api/jobs/job-1", { authorization: `Bearer ${aliceToken}` }),
    d,
  );
  assert.equal(detail.status, 200);
  assert.equal((detail.body as Record<string, unknown>)["job_id"], "job-1");

  const foreignDetail = await handlePlatformRequest(
    request("GET", "/api/jobs/job-1", { authorization: `Bearer ${bobToken}` }),
    d,
  );
  assert.equal(foreignDetail.status, 404, "foreign jobs are invisible");

  const unknownDetail = await handlePlatformRequest(
    request("GET", "/api/jobs/never-submitted", { authorization: `Bearer ${aliceToken}` }),
    d,
  );
  assert.equal(unknownDetail.status, 404);

  const cancel = await handlePlatformRequest(
    request("POST", "/api/jobs/job-1/cancel", { authorization: `Bearer ${aliceToken}` }),
    d,
  );
  assert.equal(cancel.status, 200);
  assert.equal((cancel.body as Record<string, unknown>)["status"], "cancelled");
  assert.equal((cancel.body as Record<string, unknown>)["error"], "cancelled");

  const cancelAgain = await handlePlatformRequest(
    request("POST", "/api/jobs/job-1/cancel", { authorization: `Bearer ${aliceToken}` }),
    d,
  );
  assert.equal(cancelAgain.status, 422, "cancelling a terminal job is an illegal transition");
});

test("heartbeat endpoint behaves end to end", async () => {
  const d = deps();
  await handlePlatformRequest(
    request("POST", "/api/devices/register", { body: REGISTER_BODY, authorization: `Bearer ${aliceToken}` }),
    d,
  );
  const beat = await handlePlatformRequest(
    request("PATCH", "/api/devices/dev-1/heartbeat", { authorization: `Bearer ${aliceToken}` }),
    d,
  );
  assert.equal(beat.status, 200);
  assert.equal((beat.body as Record<string, unknown>)["status"], "online");

  const foreign = await handlePlatformRequest(
    request("PATCH", "/api/devices/dev-1/heartbeat", { authorization: `Bearer ${bobToken}` }),
    d,
  );
  assert.equal(foreign.status, 404);

  const missing = await handlePlatformRequest(
    request("PATCH", "/api/devices/dev-missing/heartbeat", { authorization: `Bearer ${aliceToken}` }),
    d,
  );
  assert.equal(missing.status, 404);

  const wrongMethod = await handlePlatformRequest(
    request("POST", "/api/devices/dev-1/heartbeat", { authorization: `Bearer ${aliceToken}` }),
    d,
  );
  assert.equal(wrongMethod.status, 405);
});

test("lists require authentication and return the unified schema", async () => {
  const d = deps();
  await handlePlatformRequest(
    request("POST", "/api/jobs", {
      body: { job_id: "job-9", operation: "vector_scale", element_count: 8, protocol_version: PROTOCOL_VERSION },
      authorization: `Bearer ${aliceToken}`,
    }),
    d,
  );
  const list = await handlePlatformRequest(
    request("GET", "/api/jobs", { authorization: `Bearer ${aliceToken}` }),
    d,
  );
  assert.equal(list.status, 200);
  const jobs = (list.body as { jobs: Array<Record<string, unknown>> }).jobs;
  assert.equal(jobs.length, 1);
  assert.equal(jobs[0]["job_id"], "job-9");
  assert.equal(jobs[0]["operation"], "vector_scale");
});

test("auth context sanity: the local verifier only trusts the local scheme", async () => {
  assert.ok(alice.authenticated);
  const d = deps();
  const verdict = await d.verifier("local:someone");
  assert.ok(verdict !== null && verdict.authenticated);
  assert.equal(await d.verifier("supabase-token"), null);
});
